#include "discord_rest_client.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "discord_json.h"
#include "discord_security.h"

#define DC_REST_LOG_TAG "rest"

#define DC_REST_ACK_ROUTE_MAX   384u
#define DC_REST_VALUE_MAX       32u
#define DC_REST_LOCATION_MAX    256u
#define DC_REST_FINGERPRINT_MAX 16u
#define DC_REST_HDR_KEY_MAX     32u
#define DC_REST_AUTH_MAX        (DC_REST_TOKEN_MAX + 8u)
#define DC_REST_USER_AGENT      "ESP32-DiscordBot/1.0"

#define DC_REST_HTTP_2XX_MIN 200
#define DC_REST_HTTP_2XX_MAX 299
#define DC_REST_HTTP_3XX_MIN 300
#define DC_REST_HTTP_3XX_MAX 399
#define DC_REST_HTTP_401     401
#define DC_REST_HTTP_403     403
#define DC_REST_HTTP_429     429
#define DC_REST_HTTP_5XX_MIN 500

typedef enum {
    DC_REST_PATH_REST = 0,
    DC_REST_PATH_ACK
} DcRestPath;

typedef enum {
    DC_GATE_OK = 0,
    DC_GATE_REFUSED = 1,
    DC_GATE_HOLD = 2
} DcGateResult;

typedef enum {
    DC_ROUND_DONE = 0,
    DC_ROUND_FOLLOW = 1,
    DC_ROUND_RETRY = 2,
    DC_ROUND_REFUSE = 3
} DcRoundAction;

typedef struct {
    char location[DC_REST_LOCATION_MAX];
    char retry_after[DC_REST_VALUE_MAX];
    char rl_remaining[DC_REST_VALUE_MAX];
    char rl_reset_after[DC_REST_VALUE_MAX];
    char rl_global[DC_REST_VALUE_MAX];
    char rl_scope[DC_REST_VALUE_MAX];
    char *body;
    size_t body_cap;
    size_t body_len;
    bool rl_seen;
} DcRestCapture;

typedef struct {
    const char *name;
    size_t off;
    size_t cap;
    bool rate_limit;
} DcHdrSlot;

static const DcHdrSlot HDR_SLOTS[] = {
    { "Location", offsetof(DcRestCapture, location),
      DC_REST_LOCATION_MAX, false },
    { "Retry-After", offsetof(DcRestCapture, retry_after),
      DC_REST_VALUE_MAX, true },
    { "X-RateLimit-Remaining", offsetof(DcRestCapture, rl_remaining),
      DC_REST_VALUE_MAX, true },
    { "X-RateLimit-Reset-After", offsetof(DcRestCapture, rl_reset_after),
      DC_REST_VALUE_MAX, true },
    { "X-RateLimit-Global", offsetof(DcRestCapture, rl_global),
      DC_REST_VALUE_MAX, true },
    { "X-RateLimit-Scope", offsetof(DcRestCapture, rl_scope),
      DC_REST_VALUE_MAX, true },
};

static const esp_http_client_method_t DC_REST_HTTP_METHODS[] = {
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_PATCH,
    HTTP_METHOD_DELETE,
};

static char g_token[DC_REST_TOKEN_MAX];
static DcRateLimiter *g_rl;
static DcRedirectGuard g_guard;
static DcRestCapture *g_active;
static uint16_t g_last_status;
static bool g_ready;
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

static uint64_t clock_ms(void)
{
    return (uint64_t)xTaskGetTickCount() * (uint64_t)portTICK_PERIOD_MS;
}

static uint32_t fnv1a_route(const char *s)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; s[i] != '\0'; i++) {
        hash ^= (uint8_t)s[i];
        hash *= 16777619u;
    }
    return hash;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    if (s == NULL || s[0] == '\0') {
        return false;
    }

    uint64_t v = 0u;
    for (size_t i = 0; s[i] != '\0'; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10u + (uint64_t)(s[i] - '0');
        if (v > UINT32_MAX) {
            return false;
        }
    }

    *out = (uint32_t)v;
    return true;
}

static bool parse_bool(const char *s)
{
    return s != NULL &&
           (strcasecmp(s, "true") == 0 || strcasecmp(s, "1") == 0);
}

static int backoff_of(int attempt)
{
    uint32_t ms = DC_REST_5XX_BASE_MS;
    for (int i = 0; i < attempt && ms < DC_REST_5XX_MAX_MS; i++) {
        ms *= 2u;
    }
    return ms > DC_REST_5XX_MAX_MS ? (int)DC_REST_5XX_MAX_MS : (int)ms;
}

static uint32_t retry_after_ms(const DcRestCapture *cap)
{
    uint32_t ms = 0u;
    if (parse_u32(cap->rl_reset_after, &ms)) {
        return ms;
    }

    uint32_t secs = 0u;
    if (parse_u32(cap->retry_after, &secs)) {
        if (secs > UINT32_MAX / 1000u) {
            return UINT32_MAX;
        }
        return secs * 1000u;
    }

    return 0u;
}

static void capture_reset(DcRestCapture *cap, char *body, size_t body_cap)
{
    memset(cap, 0, sizeof(*cap));
    cap->body = body;
    cap->body_cap = body_cap;
}

static void store_value(char *dst, size_t cap, const char *val, size_t len)
{
    if (cap == 0u) {
        return;
    }

    size_t n = len < cap - 1u ? len : cap - 1u;
    memcpy(dst, val, n);
    dst[n] = '\0';
}



static void capture_header_kv(DcRestCapture *cap, const char *key, const char *val)
{
    if (cap == NULL || key == NULL || val == NULL) {
        return;
    }

    size_t val_len = strlen(val);
    for (size_t i = 0; i < sizeof(HDR_SLOTS) / sizeof(HDR_SLOTS[0]); i++) {
        if (strcasecmp(key, HDR_SLOTS[i].name) == 0) {
            char *dst = (char *)((uint8_t *)cap + HDR_SLOTS[i].off);
            store_value(dst, HDR_SLOTS[i].cap, val, val_len);
            if (HDR_SLOTS[i].rate_limit) {
                cap->rl_seen = true;
            }
            return;
        }
    }
}

static void capture_body(DcRestCapture *cap, const char *data, size_t len)
{
    if (cap->body == NULL || cap->body_len >= cap->body_cap) {
        return;
    }

    size_t room = cap->body_cap - cap->body_len;
    size_t n = len < room ? len : room;
    memcpy(cap->body + cap->body_len, data, n);
    cap->body_len += n;
}

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    DcRestCapture *cap = g_active;
    if (cap == NULL || evt == NULL) {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (evt->header_key != NULL && evt->header_value != NULL) {
            capture_header_kv(cap, evt->header_key, evt->header_value);
        }
    } else if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (evt->data != NULL && evt->data_len > 0) {
            capture_body(cap, (const char *)evt->data, (size_t)evt->data_len);
        }
    }

    return ESP_OK;
}

static void rest_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void rest_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static DcGateResult limiter_gate(DcRestPath path, uint32_t hash,
                                     uint64_t now_ms, uint32_t *wait_ms)
{
    if (dc_ratelimit_invalid_tripped(g_rl, now_ms)) {
        ESP_LOGW(DC_REST_LOG_TAG, "invalid request breaker tripped, shedding");
        return DC_GATE_REFUSED;
    }

    if (path == DC_REST_PATH_ACK) {
        if (dc_ratelimit_global_blocked(g_rl, now_ms)) {
            ESP_LOGW(DC_REST_LOG_TAG,
                     "global 429 armed, interaction ACK held");
            return DC_GATE_REFUSED;
        }
        if (dc_ratelimit_interaction(g_rl, hash, now_ms) == DC_WAIT_OK) {
            return DC_GATE_OK;
        }
        *wait_ms = dc_ratelimit_wait_ms(g_rl, hash, now_ms);
        return DC_GATE_HOLD;
    }

    if (dc_ratelimit_rest(g_rl, hash, now_ms) == DC_WAIT_OK) {
        return DC_GATE_OK;
    }
    return DC_GATE_REFUSED;
}

static bool gate_request(DcRestPath path, uint32_t hash)
{
    uint64_t now_ms = clock_ms();
    uint32_t wait_ms = 0u;

    rest_lock();
    DcGateResult gate = g_ready ? limiter_gate(path, hash, now_ms, &wait_ms)
                                : DC_GATE_REFUSED;
    if (gate == DC_GATE_HOLD) {
        uint32_t hold = wait_ms < DC_REST_ACK_MAX_HOLD_MS
                            ? wait_ms : DC_REST_ACK_MAX_HOLD_MS;
        rest_unlock();
        if (hold > 0u) {
            vTaskDelay(pdMS_TO_TICKS((TickType_t)hold));
        }
        now_ms = clock_ms();
        rest_lock();
        gate = g_ready ? limiter_gate(path, hash, now_ms, &wait_ms)
                       : DC_GATE_REFUSED;
        if (gate == DC_GATE_HOLD) {
            ESP_LOGW(DC_REST_LOG_TAG,
                     "interaction ACK hold exceeded budget, shedding");
        }
    }
    rest_unlock();

    return gate == DC_GATE_OK;
}

static bool apply_headers(esp_http_client_handle_t client)
{
    if (!dc_header_allowed("Authorization") ||
        !dc_header_allowed("Content-Type") ||
        !dc_header_allowed("User-Agent") ||
        !dc_header_allowed("Accept")) {
        return false;
    }

    char auth[DC_REST_AUTH_MAX];
    int n = snprintf(auth, sizeof(auth), "Bot %s", g_token);
    if (n < 0 || (size_t)n >= sizeof(auth)) {
        return false;
    }

    if (esp_http_client_set_header(client, "Authorization", auth) != ESP_OK ||
        esp_http_client_set_header(client, "Content-Type",
                                   "application/json") != ESP_OK ||
        esp_http_client_set_header(client, "User-Agent",
                                   DC_REST_USER_AGENT) != ESP_OK ||
        esp_http_client_set_header(client, "Accept",
                                   "application/json") != ESP_OK) {
        return false;
    }

    return true;
}

static esp_http_client_handle_t s_client = NULL;

static int round_execute(DcRestMethod method, const char *url,
                             const char *body, DcRestCapture *cap, int timeout_ms,
                             bool allow_retry)
{
    bool was_reused = (s_client != NULL);

    if (s_client == NULL) {
        esp_http_client_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.url = url;
        cfg.timeout_ms = timeout_ms;
        cfg.disable_auto_redirect = true;
        cfg.use_global_ca_store = true;
        cfg.event_handler = on_http_event;
        cfg.keep_alive_enable = true;
        cfg.keep_alive_idle = 5;
        cfg.keep_alive_interval = 5;
        cfg.keep_alive_count = 3;

        s_client = esp_http_client_init(&cfg);
        if (s_client == NULL) {
            return -1;
        }
    } else {
        esp_http_client_set_url(s_client, url);
        esp_http_client_set_timeout_ms(s_client, timeout_ms);
    }

    esp_http_client_set_method(s_client, DC_REST_HTTP_METHODS[method]);

    if (body != NULL && body[0] != '\0') {
        esp_http_client_set_post_field(s_client, body, (int)strlen(body));
        if (!apply_headers(s_client)) {
            return -1;
        }
    } else {
        esp_http_client_set_post_field(s_client, NULL, 0);
        if (!apply_headers(s_client)) {
            return -1;
        }
        (void)esp_http_client_set_header(s_client, "Content-Type", NULL);
    }

    g_active = cap;
    esp_err_t err = esp_http_client_perform(s_client);
    g_active = NULL;
    int status = esp_http_client_get_status_code(s_client);

    /* The URL carries the interaction token for webhook followups; it stays
     * valid for 15 minutes, so it is never printed: serial logs outlive it. */
    ESP_LOGI(DC_REST_LOG_TAG, "HTTP perform status %d (err 0x%x)", status, (unsigned)err);

    if (err != ESP_OK) {
        esp_http_client_close(s_client);
        esp_http_client_cleanup(s_client);
        s_client = NULL;
        g_last_status = 0u;
        /* Transparent retry replays the request on a fresh socket. Safe
         * for idempotent REST, WRONG for the interaction ACK: the first
         * POST may have landed despite the timeout, and the replay
         * answers 400 (already acked). ACKs stay single-shot (spec: the
         * ACK path cannot afford a retry inside its deadline). */
        if (was_reused && allow_retry) {
            ESP_LOGW(DC_REST_LOG_TAG, "persistent connection failed (0x%x), retrying fresh...", (unsigned)err);
            return round_execute(method, url, body, cap, timeout_ms,
                                 allow_retry);
        }
        return -1;
    }

    g_last_status = status > 0 ? (uint16_t)status : 0u;
    return status;
}

static esp_err_t map_response(int status, const DcRestCapture *cap,
                                  uint32_t hash, uint64_t now_ms)
{
    if (cap->rl_seen) {
        DcRateLimitInfo info;
        memset(&info, 0, sizeof(info));
        info.present = true;
        info.remaining = UINT32_MAX;
        info.reset_after_ms = retry_after_ms(cap);
        info.global = parse_bool(cap->rl_global);
        info.shared = strcasecmp(cap->rl_scope, "shared") == 0;
        uint32_t remaining = 0u;
        if (parse_u32(cap->rl_remaining, &remaining)) {
            info.remaining = remaining;
        }
        dc_ratelimit_on_headers(g_rl, hash, &info, now_ms);
    }

    if (status >= DC_REST_HTTP_2XX_MIN && status <= DC_REST_HTTP_2XX_MAX) {
        return ESP_OK;
    }

    if (status == DC_REST_HTTP_401 || status == DC_REST_HTTP_403 ||
        status == DC_REST_HTTP_429) {
        if (status == DC_REST_HTTP_429) {
            dc_ratelimit_on_429(g_rl, hash, retry_after_ms(cap), now_ms);
        }
        dc_ratelimit_note_invalid(g_rl, now_ms);
    }

    if (status == DC_REST_HTTP_401) {
        char fp[DC_REST_FINGERPRINT_MAX];
        dc_token_fingerprint(g_token, strlen(g_token), fp, sizeof(fp));
        ESP_LOGE(DC_REST_LOG_TAG,
                 "401 unauthorized, token fingerprint %s, fatal", fp);
    }

    return ESP_FAIL;
}

static esp_err_t round_locked(DcRestMethod method, const char *url,
                                  const char *body, DcRestPath path,
                                  DcRestCapture *cap, uint32_t hash,
                                  bool *guard_ok, int *status)
{
    int timeout_ms = path == DC_REST_PATH_ACK ? DC_REST_ACK_TIMEOUT_MS
                                              : DC_REST_TIMEOUT_MS;
    uint64_t now_ms = clock_ms();

    rest_lock();
    *status = round_execute(method, url, body, cap, timeout_ms,
                            path != DC_REST_PATH_ACK);
    esp_err_t mapped = *status < 0
                           ? ESP_FAIL
                           : map_response(*status, cap, hash, now_ms);
    *guard_ok = false;
    if (*status >= DC_REST_HTTP_3XX_MIN && *status <= DC_REST_HTTP_3XX_MAX) {
        *guard_ok = dc_redirect_decide(&g_guard, cap->location);
    }
    rest_unlock();

    return mapped;
}

static DcRoundAction next_round(int status, bool guard_ok, int hop,
                                    DcRestPath path, int attempt, int attempts,
                                    int *backoff_ms, const char **url,
                                    const DcRestCapture *cap)
{
    if (status >= DC_REST_HTTP_3XX_MIN && status <= DC_REST_HTTP_3XX_MAX) {
        if (!guard_ok || hop >= (int)DC_REST_MAX_REDIRECTS ||
            path == DC_REST_PATH_ACK) {
            return DC_ROUND_REFUSE;
        }
        *url = cap->location;
        return DC_ROUND_FOLLOW;
    }

    if (status >= DC_REST_HTTP_5XX_MIN && attempt + 1 < attempts) {
        *backoff_ms = backoff_of(attempt);
        return DC_ROUND_RETRY;
    }

    return DC_ROUND_DONE;
}

static void deliver_body(char *out, size_t out_cap, size_t *out_len,
                             const DcRestCapture *cap)
{
    if (out_len == NULL) {
        return;
    }

    *out_len = cap->body_len;
    if (out != NULL && cap->body_len < out_cap) {
        out[cap->body_len] = '\0';
    }
}

static esp_err_t guarded_rounds(DcRestPath path, DcRestMethod method,
                                    const char *url, const char *route,
                                    const char *body, char *out, size_t out_cap,
                                    size_t *out_len)
{
    uint32_t hash = fnv1a_route(route);
    int attempts = path == DC_REST_PATH_ACK ? 1 : (int)DC_REST_5XX_ATTEMPTS;
    DcRestCapture cap;

    for (int attempt = 0; attempt < attempts; attempt++) {
        if (!gate_request(path, hash)) {
            return ESP_FAIL;
        }

        int backoff_ms = 0;
        for (int hop = 0; hop <= (int)DC_REST_MAX_REDIRECTS; hop++) {
            capture_reset(&cap, out, out_cap);

            int status = 0;
            bool guard_ok = false;
            esp_err_t mapped = round_locked(method, url, body, path,
                                            &cap, hash, &guard_ok, &status);
            if (status < 0) {
                return ESP_FAIL;
            }

            DcRoundAction action = next_round(status, guard_ok, hop, path,
                                              attempt, attempts, &backoff_ms,
                                              &url, &cap);
            if (action == DC_ROUND_REFUSE) {
                ESP_LOGW(DC_REST_LOG_TAG,
                         "redirect refused, status %d", status);
                return ESP_FAIL;
            }
            if (action == DC_ROUND_FOLLOW) {
                continue;
            }
            if (action == DC_ROUND_DONE) {
                deliver_body(out, out_cap, out_len, &cap);
                return mapped;
            }
            break;
        }

        if (backoff_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS((TickType_t)backoff_ms));
        }
    }

    return ESP_FAIL;
}

esp_err_t dc_rest_init(const char *token, DcRateLimiter *rl)
{
    if (token == NULL || rl == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(token);
    if (len == 0u || len >= sizeof(g_token) ||
        dc_token_looks_valid(token, len) != DC_SEC_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }

    memset(g_token, 0, sizeof(g_token));
    memcpy(g_token, token, len);
    g_rl = rl;
    dc_redirect_guard_init(&g_guard);
    g_last_status = 0u;
    g_ready = true;

    return ESP_OK;
}

void dc_rest_deinit(void)
{
    rest_lock();
    if (s_client != NULL) {
        esp_http_client_close(s_client);
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
    rest_unlock();

    memset(g_token, 0, sizeof(g_token));
    g_rl = NULL;
    g_ready = false;
    g_last_status = 0u;
    memset(&g_guard, 0, sizeof(g_guard));
}

esp_err_t dc_rest_prewarm(void)
{
    return dc_rest_request(DC_REST_GET, "/users/@me", NULL, NULL, 0u, NULL);
}

esp_err_t dc_rest_keepalive(void)
{
    return dc_rest_request(DC_REST_GET, "/gateway", NULL, NULL, 0u, NULL);
}

bool dc_rest_build_interaction_body(uint8_t kind, bool ephemeral, char *out,
                                        size_t cap, size_t *len)
{
    if (out == NULL || cap == 0u) {
        return false;
    }

    DcJson w;
    dc_json_init(&w, out, cap);
    dc_json_obj_begin(&w);
    dc_json_u64(&w, "type", (uint64_t)kind);
    if (kind == 4u) {
        dc_json_key(&w, "data");
        dc_json_obj_begin(&w);
        dc_json_str(&w, "content", "Pong! :ping_pong:");
        dc_json_obj_end(&w);
    } else if (kind == 5u && ephemeral) {
        dc_json_key(&w, "data");
        dc_json_obj_begin(&w);
        dc_json_u64(&w, "flags", 64u);
        dc_json_obj_end(&w);
    }
    dc_json_obj_end(&w);

    if (!dc_json_valid(&w)) {
        return false;
    }

    if (len != NULL) {
        *len = dc_json_len(&w);
    }
    return true;
}

esp_err_t dc_rest_post_interaction(const DiscordAck *ack,
                                       const char *json_body)
{
    if (ack == NULL || ack->id == 0u || ack->token[0] == '\0' ||
        ack->kind == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    char body[DC_REST_BODY_MAX];
    if (json_body == NULL) {
        if (!dc_rest_build_interaction_body(ack->kind, ack->ephemeral, body,
                                            sizeof(body), NULL)) {
            return ESP_ERR_INVALID_ARG;
        }
        json_body = body;
    } else if (json_body[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char route[DC_REST_ACK_ROUTE_MAX];
    int n = snprintf(route, sizeof(route), "/interactions/%llu/%s/callback",
                     (unsigned long long)ack->id, ack->token);
    if (n < 0 || (size_t)n >= sizeof(route)) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[DC_REST_URL_MAX];
    n = snprintf(url, sizeof(url), "%s%s", DC_REST_API_BASE, route);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        return ESP_ERR_INVALID_ARG;
    }

    return guarded_rounds(DC_REST_PATH_ACK, DC_REST_POST, url, route,
                          json_body, NULL, 0u, NULL);
}

esp_err_t dc_rest_request(DcRestMethod method, const char *route,
                              const char *body, char *out, size_t out_cap,
                              size_t *out_len)
{
    if (route == NULL || route[0] == '\0' || strlen(route) >= DC_REST_ROUTE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((int)method < (int)DC_REST_GET || (int)method > (int)DC_REST_DELETE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out == NULL && out_cap != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_len != NULL) {
        *out_len = 0u;
    }

    char url[DC_REST_URL_MAX];
    int n = snprintf(url, sizeof(url), "%s%s", DC_REST_API_BASE, route);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        return ESP_ERR_INVALID_ARG;
    }

    return guarded_rounds(DC_REST_PATH_REST, method, url, route, body,
                          out, out_cap, out_len);
}

uint16_t dc_rest_last_status(void)
{
    return g_last_status;
}

size_t dc_rest_footprint(void)
{
    return sizeof(g_token) + sizeof(g_guard) + sizeof(s_lock_buf) +
           sizeof(g_active) + sizeof(g_rl) + sizeof(g_ready) +
           sizeof(g_last_status);
}
