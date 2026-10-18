#include "discord_transport.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>

#include "bot_config.h"
#include "discord_ackq.h"
#include "discord_gateway.h"
#include "discord_intq.h"
#include "discord_jobq.h"
#include "discord_provision.h"
#include "discord_rest_client.h"
#include "discord_ratelimit.h"
#include "discord_root_ca.h"
#include "discord_state.h"
#include "discord_v2.h"
#include "discord_watchdog.h"
#include "discord_ws.h"

#define DC_TRANSPORT_TAG "dc_transport"

#define DC_TRANSPORT_TASK_COUNT 5

#define DC_WD_TOLERANCE 2u

#define DC_ACK_POLL_MS      10u
#define DC_GATEWAY_POLL_MS  100u
#define DC_REST_POLL_MS     10u
/* 20 ms, not 1 s: interaction handlers now run here, so this poll interval
 * is the latency every interaction pays before its handler starts. */
#define DC_BOT_POLL_MS      20u
#define DC_MONITOR_POLL_MS  1000u

#define DC_ACK_TASK_PERIOD_MS \
    (DC_REST_ACK_MAX_HOLD_MS + DC_REST_ACK_TIMEOUT_MS)
#define DC_GATEWAY_TASK_PERIOD_MS 2000u
#define DC_REST_TASK_PERIOD_MS \
    (DC_REST_TIMEOUT_MS + DC_REST_ACK_TIMEOUT_MS + \
     (DC_REST_5XX_ATTEMPTS - 1u) * DC_REST_5XX_MAX_MS)
/* A bot_task tick is no longer a delay: it runs interaction handlers, and the
 * slowest of them (/bug, /feature) issues two sequential REST calls (create
 * thread, then post). One call is bounded by DC_REST_TASK_PERIOD_MS, so two
 * is the honest period. Tolerance 2 then restarts the device only after a
 * handler has been stuck for ~106 s, which is the price of letting handlers
 * do network I/O; the WS task, which must answer Discord in 3 s, is free. */
#define DC_BOT_TASK_PERIOD_MS  (2u * DC_REST_TASK_PERIOD_MS)
#define DC_MONITOR_TASK_PERIOD_MS  1000u

#define DC_LOG_EVERY_S 60u

#define DC_TRANSPORT_HTTP_401 401

#define DC_NVS_TOKEN_KEY "bot_token"

enum {
    DC_TASK_ACK = 0,
    DC_TASK_GATEWAY,
    DC_TASK_REST,
    DC_TASK_BOT,
    DC_TASK_MONITOR
};

static void ack_task(void *arg);
static void gateway_task(void *arg);
static void rest_task(void *arg);
static void bot_task(void *arg);
static void monitor_task(void *arg);
static void route_interaction(const DiscordInteraction *it);
static void dispatch_pump(void);

static DiscordFeatureRegistry g_registry;
static DcBotState g_bot_state;
static DcRateLimiter g_rl;
static DcWatchdog g_wd;
static DcJobQueue g_jobs;
static DcIntq g_intq;
/* One handler at a time, whichever task it runs on. See on_message. */
static SemaphoreHandle_t g_dispatch_lock;
static DcWsMemberHandler g_member_handler;
static uint32_t g_msg_sheds;
static DcTransportState g_state = DC_TR_IDLE;
static bool g_initialized;
static char g_token[DC_REST_TOKEN_MAX];
static TaskHandle_t g_tasks[DC_TRANSPORT_TASK_COUNT];
static int g_slots[DC_TRANSPORT_TASK_COUNT] = { -1, -1, -1, -1, -1 };

static const char *const TASK_NAMES[DC_TRANSPORT_TASK_COUNT] = {
    "dc_ack", "dc_gateway", "dc_rest", "dc_bot", "dc_monitor"
};

static const TaskFunction_t TASK_FUNCS[DC_TRANSPORT_TASK_COUNT] = {
    ack_task, gateway_task, rest_task, bot_task, monitor_task
};

/* Bytes, not words: ESP-IDF redefines xTaskCreate's usStackDepth
 * (freertos/esp_additions/.../freertos_idf_additions_priv.h:305). dc_bot is
 * 8192 because it now carries what the WebSocket client task carried while it
 * ran handlers: TLS round trips plus the handlers' own frame (~2.2 KiB of
 * buffer for /bug). Proven adequate at exactly this depth on that task. */
static const uint32_t TASK_STACKS[DC_TRANSPORT_TASK_COUNT] = {
    10240u, 6144u, 10240u, 8192u, 3072u
};

static const UBaseType_t TASK_PRIOS[DC_TRANSPORT_TASK_COUNT] = {
    16u, 15u, 12u, 10u, 10u
};

static const uint32_t TASK_PERIODS[DC_TRANSPORT_TASK_COUNT] = {
    DC_ACK_TASK_PERIOD_MS, DC_GATEWAY_TASK_PERIOD_MS,
    DC_REST_TASK_PERIOD_MS, DC_BOT_TASK_PERIOD_MS,
    DC_MONITOR_TASK_PERIOD_MS
};

static uint64_t epoch_ms(void)
{
    return (uint64_t)time(NULL) * 1000u;
}

static uint64_t boot_ms(void)
{
    return (uint64_t)xTaskGetTickCount() * (uint64_t)portTICK_PERIOD_MS;
}

static const char *token_provider(size_t *len)
{
    if (g_token[0] == '\0') {
        return NULL;
    }
    *len = strlen(g_token);
    return g_token;
}

/* Producer side, WebSocket task. Nothing here may block: this callback runs
 * inside the frame reader, and every microsecond spent here is a microsecond
 * the gateway is not reading (AGENT.md §3). The handler chain — including the
 * synchronous REST calls /bug and /feature make — belongs to dc_bot. */
static void on_interaction(const DiscordInteraction *it)
{
    DcIntStatus st;

    if (it == NULL) {
        return;
    }
    ESP_LOGI(DC_TRANSPORT_TAG,
             "interaction id %llu type %u cmd '%s' cid '%s' user %llu vals %u",
             (unsigned long long)it->id, (unsigned)it->type,
             it->command, it->custom_id,
             (unsigned long long)it->user_id,
             (unsigned)it->value_count);
    st = dc_intq_push(&g_intq, it);
    if (st == DC_INTQ_FULL) {
        ESP_LOGW(DC_TRANSPORT_TAG,
                 "interaction id %llu shed, ring of %u full (drops %u)",
                 (unsigned long long)it->id, (unsigned)DC_INTQ_CAPACITY,
                 (unsigned)dc_intq_dropped(&g_intq));
    }
}

static void route_interaction(const DiscordInteraction *it)
{
    bool routed;

    routed = dc_registry_route_interaction(&g_registry, it);
    if (routed) {
        return;
    }
    if (!dc_registry_route_component(&g_registry, it)) {
        ESP_LOGW(DC_TRANSPORT_TAG,
                 "interaction id %llu unrouted (cmd '%s' cid '%s')",
                 (unsigned long long)it->id, it->command,
                 it->custom_id);
        if (it->command[0] != '\0') {
            (void)discord_interaction_reply(it, "Command not recognized by this firmware.");
        }
    }
}

/* Consumer side, bot task. Pets the watchdog per handled entry: one handler
 * may legally cost two REST round trips, which would otherwise read as a dead
 * task while it is only waiting on Discord. */
static void dispatch_pump(void)
{
    DiscordInteraction it;

    while (dc_intq_pop(&g_intq, &it)) {
        xSemaphoreTake(g_dispatch_lock, portMAX_DELAY);
        route_interaction(&it);
        xSemaphoreGive(g_dispatch_lock);
        dc_watchdog_pet(&g_wd, g_slots[DC_TASK_BOT], epoch_ms());
    }
}

static void on_message(const DiscordMessage *msg)
{
    /* MESSAGE_CREATE still arrives on the WebSocket task, so after the
     * interaction move it can now overlap a handler running on dc_bot. Both
     * sides touch g_bot_state, the record store and the reply statics, and
     * nothing else serialises them: this mutex is what keeps the old
     * "one handler at a time, on one task" invariant the reply statics rely on
     * (see discord_reply.c).
     *
     * The WebSocket task must never WAIT here — that is the very debt this
     * change repays — so it tries and sheds instead of blocking. A shed costs
     * one XP/spam sample while a /bug is doing its REST round trip; blocking
     * would instead stall heartbeats and get the gateway dropped. */
    if (g_dispatch_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(g_dispatch_lock, 0) != pdTRUE) {
        g_msg_sheds++;
        if (g_msg_sheds % 64u == 1u) {
            ESP_LOGW(DC_TRANSPORT_TAG,
                     "message shed while a handler ran (total %u)",
                     (unsigned)g_msg_sheds);
        }
        return;
    }
    dc_registry_dispatch_message(&g_registry, msg);
    xSemaphoreGive(g_dispatch_lock);
}

/* Member join/leave wrapper: the bot's member sink (welcome log + captcha
 * join DM) runs from the WebSocket task and touches shared state — the
 * captcha slots a /verify handler on dc_bot may read at the same time —
 * so it takes the same dispatch lock as messages. Try-lock plus shed, not
 * wait: joins are rare, but the task's no-blocking rule is uniform. A
 * shed join costs one log line and one captcha DM; the member can still
 * run /verify. */
static void member_event_sink(uint64_t guild_id, uint64_t user_id,
                              bool joined)
{
    if (g_member_handler == NULL) return;
    if (g_dispatch_lock == NULL) return;
    if (xSemaphoreTake(g_dispatch_lock, 0) != pdTRUE) {
        ESP_LOGW(DC_TRANSPORT_TAG,
                 "member event shed while a handler ran (user %llu)",
                 (unsigned long long)user_id);
        return;
    }
    g_member_handler(guild_id, user_id, joined);
    xSemaphoreGive(g_dispatch_lock);
}

static const char *modal_for_command(const char *command, const char *sub)
{
    const DiscordFeature *f;

    if (command == NULL) return NULL;
    f = dc_registry_find_command(&g_registry, command);
    if (f == NULL || !(f->flags & DC_FEATURE_WANTS_MODAL)) return NULL;
    /* A form that belongs to one subcommand must not open for the others:
     * "/bug open" shows the modal, "/bug status fixed" has to defer so the
     * handler can reach the thread. modal_sub NULL means the whole command
     * owns the form, which is how every subcommand-less form keeps working. */
    if (f->modal_sub != NULL &&
        (sub == NULL || strcmp(f->modal_sub, sub) != 0)) {
        return NULL;
    }
    return f->modal_json; /* NULL (flag without JSON) defers normally */
}

/* Command-level ephemeral lookup for the ACK path. Only reached when the
 * command does not open a modal (enqueue_ack consults it in the deferring
 * branch alone), which is what lets the reports commands carry both the
 * modal sub and the flag. An unknown command returns false, which is the
 * old public-defer behaviour. */
static bool ephemeral_for_command(const char *command)
{
    const DiscordFeature *f;

    if (command == NULL) return false;
    f = dc_registry_find_command(&g_registry, command);
    return f != NULL && (f->flags & DC_FEATURE_EPHEMERAL) != 0u;
}

static esp_err_t load_token(void)
{
    nvs_handle_t handle;
    esp_err_t err;
    size_t len = sizeof(g_token);

    err = nvs_open(DC_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(handle, DC_NVS_TOKEN_KEY, g_token, &len);
    nvs_close(handle);
    return err;
}

static void ack_task(void *arg)
{
    DiscordAck ack;

    (void)arg;
    for (;;) {
        if (dc_ackq_pop(dc_ws_ackq(), &ack)) {
            esp_err_t err;
            /* Kind 9 carries the feature's static modal document;
             * everything else builds {"type":kind}. */
            err = dc_rest_post_interaction(&ack, ack.modal_json);
            ESP_LOGI(DC_TRANSPORT_TAG,
                     "ack id %llu kind %u%s -> %s (status %u, depth %u)",
                     (unsigned long long)ack.id, (unsigned)ack.kind,
                     ack.ephemeral ? " ephemeral" : "",
                     err == ESP_OK ? "ok" : "FAILED",
                     (unsigned)dc_rest_last_status(),
                     (unsigned)dc_ackq_depth(dc_ws_ackq()));
        }
        dc_watchdog_pet(&g_wd, g_slots[DC_TASK_ACK], epoch_ms());
        vTaskDelay(pdMS_TO_TICKS(DC_ACK_POLL_MS));
    }
}

static void gateway_task(void *arg)
{
    uint64_t now;

    (void)arg;
    for (;;) {
        now = epoch_ms();
        dc_ws_gateway_tick(now);
        dc_watchdog_pet(&g_wd, g_slots[DC_TASK_GATEWAY], now);
        vTaskDelay(pdMS_TO_TICKS(DC_GATEWAY_POLL_MS));
    }
}

static char *job_payload(const DcJob *job, char *buf, size_t cap)
{
    size_t len = job->payload_len;

    if (len > DC_JOB_PAYLOAD) {
        len = DC_JOB_PAYLOAD;
    }
    if (len >= cap) {
        len = cap - 1u;
    }
    memcpy(buf, job->payload, len);
    buf[len] = '\0';
    return buf;
}

/* Role ID for ADD/REMOVE_ROLE: digit string from the payload (u64-safe),
 * aux_id fallback when the payload is empty. */
static unsigned long long role_from_payload(const DcJob *job)
{
    unsigned long long id = 0;
    uint8_t i;

    for (i = 0; i < job->payload_len && i < DC_JOB_PAYLOAD; i++) {
        char c = job->payload[i];
        if (c < '0' || c > '9') break;
        id = id * 10u + (unsigned long long)(c - '0');
    }
    if (id == 0) id = (unsigned long long)job->aux_id;
    return id;
}

static esp_err_t channel_route(const DcJob *job, char *route,
                                   size_t cap, DcRestMethod *method,
                                   bool *has_body)
{
    int n;

    *has_body = false;
    if (job->kind == DC_JOB_SEND_MESSAGE) {
        *method = DC_REST_POST;
        *has_body = true;
        n = snprintf(route, cap, "/channels/%llu/messages",
                     (unsigned long long)job->channel_id);
    } else if (job->kind == DC_JOB_EDIT_MESSAGE) {
        *method = DC_REST_PATCH;
        *has_body = true;
        n = snprintf(route, cap, "/channels/%llu/messages/%llu",
                     (unsigned long long)job->channel_id,
                     role_from_payload(job));
    } else {
        *method = DC_REST_DELETE;
        /* Message snowflakes are 64-bit like roles: digit string from the
         * payload (unused as body here), aux_id fallback. */
        n = snprintf(route, cap, "/channels/%llu/messages/%llu",
                     (unsigned long long)job->channel_id,
                     role_from_payload(job));
    }
    if (n < 0 || (size_t)n >= cap) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t guild_route(const DcJob *job, char *route,
                                 size_t cap, DcRestMethod *method,
                                 bool *has_body)
{
    int n;
    unsigned long long role_id;

    *has_body = false;
    if (job->kind == DC_JOB_TIMEOUT_USER) {
        *method = DC_REST_PUT;
        *has_body = true;
        n = snprintf(route, cap, "/guilds/%llu/members/%llu/timeout",
                     (unsigned long long)job->guild_id,
                     (unsigned long long)job->user_id);
    } else if (job->kind == DC_JOB_ADD_ROLE) {
        /* Role snowflakes are 64-bit: they ride as a digit string in the
         * 48-byte payload (19 digits fit), never in the u32 aux_id which
         * would truncate them. Empty payload falls back to aux_id. */
        role_id = role_from_payload(job);
        *method = DC_REST_PUT;
        n = snprintf(route, cap, "/guilds/%llu/members/%llu/roles/%llu",
                     (unsigned long long)job->guild_id,
                     (unsigned long long)job->user_id, role_id);
    } else {
        role_id = role_from_payload(job);
        *method = DC_REST_DELETE;
        n = snprintf(route, cap, "/guilds/%llu/members/%llu/roles/%llu",
                     (unsigned long long)job->guild_id,
                     (unsigned long long)job->user_id, role_id);
    }
    if (n < 0 || (size_t)n >= cap) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t job_route(const DcJob *job, const char *payload,
                               char *route, size_t cap,
                               DcRestMethod *method, bool *has_body)
{
    int n;

    *has_body = false;
    *method = DC_REST_GET;
    switch (job->kind) {
    case DC_JOB_SEND_MESSAGE:
    case DC_JOB_EDIT_MESSAGE:
    case DC_JOB_DELETE_MESSAGE:
        return channel_route(job, route, cap, method, has_body);
    case DC_JOB_TIMEOUT_USER:
    case DC_JOB_ADD_ROLE:
    case DC_JOB_REMOVE_ROLE:
        return guild_route(job, route, cap, method, has_body);
    case DC_JOB_CUSTOM:
        n = snprintf(route, cap, "%s", payload);
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    if (n < 0 || (size_t)n >= cap) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* TIMEOUT jobs carry duration seconds in aux_id, not a JSON body: the full
 * {"communication_disabled_until":"...Z"} document (~55 chars) does not fit
 * the 48-byte job payload, so the transport renders it here from the SNTP
 * clock. Returns ESP_OK with body written, or ESP_FAIL without a clock. */
static esp_err_t timeout_body(const DcJob *job, char *buf, size_t cap)
{
    time_t now;
    struct tm tm_utc;
    int n;

    if (job->aux_id == 0u || job->aux_id > 2419200u) return ESP_FAIL;
    now = time(NULL);
    if (now <= 0) return ESP_FAIL;
    now += (time_t)job->aux_id;
    if (gmtime_r(&now, &tm_utc) == NULL) return ESP_FAIL;
    n = snprintf(buf, cap,
                 "{\"communication_disabled_until\":\"%04d-%02d-%02dT%02d:%02d:%02dZ\"}",
                 tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                 tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    if (n < 0 || (size_t)n >= cap) return ESP_FAIL;
    return ESP_OK;
}

/* Bulk-delete support: one NUKE job fetches recent history, extracts up
 * to 10 message IDs and fires one bulk-delete. Discord REST supports up to 100;
 * the RX buffer (1 KiB) and the 48-byte job world cap us at 10 per job —
 * repeat the command for more. Discord also refuses messages older
 * than 14 days inside bulk-delete (partial success then). */

#define DC_NUKE_MAX 10u
#define DC_NUKE_BACKSCAN 192u
#define DC_DISCORD_EPOCH_MS 1420070400000ULL
#define DC_NUKE_FRESH_MS 1209600000ULL /* bulk-delete refuses older */

/* Nuke history buffer: 10 Discord messages run ~300-500 B each with
 * author objects in quiet channels, 1-2 KiB in rich ones — 8 KiB fits
 * a full page either way, so pages stop wasting round trips on
 * truncated halves. Static: the rest_task stack (10 KiB, hwm ~5 KiB)
 * keeps its margin. Cost: +8192 B static. */
#define DC_NUKE_HIST_MAX 8192u
static char s_nuke_hist[DC_NUKE_HIST_MAX];

/* Pacing: history GETs, bulk posts and singles in a tight loop trip
 * Discord's rate limiter (429 seen live), aborting `all` after a few
 * messages. One page costs ~2 s; the loop pets the rest-task watchdog
 * every page so long nukes never look stuck. */
#define DC_NUKE_PAGE_DELAY_MS 1200u
#define DC_NUKE_SINGLE_DELAY_MS 300u

/* Nearest `"id":"<digits>"` at or before `end`, searching back at most
 * DC_NUKE_BACKSCAN bytes from `start`. Returns parsed id, 0 when absent. */
static uint64_t nuke_prev_id(const char *start, const char *end)
{
    static const char pat[] = "\"id\":\"";
    const char *win = (end - start > DC_NUKE_BACKSCAN)
                          ? end - DC_NUKE_BACKSCAN
                          : start;
    const char *last = NULL;
    const char *p = win;

    while (p + sizeof(pat) - 1 <= (const char *)end) {
        size_t k;
        for (k = 0; k < sizeof(pat) - 1; k++) {
            if (p[k] != pat[k]) break;
        }
        if (k == sizeof(pat) - 1) last = p;
        p++;
    }
    if (last == NULL) return 0;
    {
        uint64_t id = 0;
        const char *d = last + sizeof(pat) - 1;
        if (d >= end || *d < '0' || *d > '9') return 0;
        while (d < end && *d >= '0' && *d <= '9') {
            uint64_t dig = (uint64_t)(*d - '0');
            if (id > (0xFFFFFFFFFFFFFFFFu - dig) / 10u) return 0;
            id = id * 10u + dig;
            d++;
        }
        return id;
    }
}

static uint8_t nuke_collect_ids(const char *body, size_t len,
                                const char *chan_digits,
                                uint64_t *ids, uint8_t want)
{
    static const char key[] = "\"channel_id\":\"";
    static const char key_sp[] = "\"channel_id\": \"";
    char needle[64];
    char needle_sp[64];
    size_t nlen, nlen_sp;
    uint8_t got = 0;
    uint32_t hits = 0;
    uint32_t hits_sp = 0;
    size_t i;
    int n;

    n = snprintf(needle, sizeof(needle), "%s%s\"", key, chan_digits);
    if (n < 0 || (size_t)n >= sizeof(needle)) return 0;
    nlen = (size_t)n;
    n = snprintf(needle_sp, sizeof(needle_sp), "%s%s\"", key_sp,
                 chan_digits);
    if (n < 0 || (size_t)n >= sizeof(needle_sp)) return 0;
    nlen_sp = (size_t)n;
    i = 0;
    while (i + nlen <= len && got < want) {
        size_t k;
        for (k = 0; k < nlen; k++) {
            if (body[i + k] != needle[k]) break;
        }
        if (k == nlen) {
            uint64_t id;
            hits++;
            id = nuke_prev_id(body, &body[i]);
            if (id != 0) {
                uint8_t d;
                for (d = 0; d < got; d++) {
                    if (ids[d] == id) break;
                }
                if (d == got) ids[got++] = id;
            }
            i += nlen;
        } else {
            i++;
        }
    }
    /* Spaced variant count (diagnostic only): tells whether Discord
     * mixes `"channel_id":".."` and `"channel_id": ".."` shapes. */
    for (i = 0; i + nlen_sp <= len; i++) {
        size_t k;
        for (k = 0; k < nlen_sp; k++) {
            if (body[i + k] != needle_sp[k]) break;
        }
        if (k == nlen_sp) {
            hits_sp++;
            i += nlen_sp;
        }
    }
    if (got < want) {
        ESP_LOGW(DC_TRANSPORT_TAG,
                 "nuke extract: %u id(s) from %u compact + %u spaced hits (%u B)",
                 (unsigned)got, (unsigned)hits, (unsigned)hits_sp,
                 (unsigned)len);
    }
    return got;
}

/* Snowflake timestamp split: bulk-delete refuses messages older than
 * 14 days (Discord-side, HTTP 400), but single DELETE accepts any age.
 * Fresh ids go bulk, old ones go one-by-one. No clock (or a future
 * timestamp) tries bulk and lets Discord decide. */
static bool nuke_is_fresh(uint64_t id, uint64_t now_ms)
{
    uint64_t ts = (id >> 22) + DC_DISCORD_EPOCH_MS;

    if (now_ms == 0 || ts > now_ms) return true;
    return now_ms - ts < DC_NUKE_FRESH_MS;
}

/* POST/DELETE with one 429 retry: Discord's bulk bucket is strict, and
 * aborting the whole nuke on the first 429 wastes the pages already
 * fetched. Waits 5 s (watchdog petted so the rest task never looks
 * stuck), retries once, then gives up with the usual repeat hint.
 * Returns ESP_OK on 2xx. */
static esp_err_t nuke_send_retry(DcRestMethod method, const char *route,
                                 const char *body)
{
    esp_err_t err = dc_rest_request(method, route, body, NULL, 0u, NULL);

    if (err != ESP_OK && dc_rest_last_status() == 429) {
        ESP_LOGW(DC_TRANSPORT_TAG, "nuke 429, waiting 5 s once");
        for (uint8_t w = 0; w < 10u; w++) {
            vTaskDelay(pdMS_TO_TICKS(500));
            dc_watchdog_pet(&g_wd, g_slots[DC_TASK_REST], epoch_ms());
        }
        err = dc_rest_request(method, route, body, NULL, 0u, NULL);
    }
    return err;
}

static void nuke_execute(const DcJob *job, char *work, size_t work_cap,
                         char *route, size_t route_cap)
{
    uint64_t ids[DC_NUKE_MAX];
    uint64_t fresh[DC_NUKE_MAX];
    char chan[24];
    uint8_t want, pages;
    uint64_t before = 0;
    uint64_t now_ms = epoch_ms();
    size_t len = 0;
    bool ok = true;
    bool drained = false;
    bool is_all;
    uint32_t bulk_n = 0;
    uint32_t single_n = 0;
    int n;
    esp_err_t err;

    /* aux 0 = "all": up to 15 newest-first pages, then stop with a
     * hint to repeat. Pages hold ~3-10 messages each (rich messages run
     * 1-2 KiB, so a 4 KiB page rarely fills): one run clears several
     * dozen progressively. Each page is 1 GET + 1 bulk + singles, paced
     * ~2 s with 429-retry and watchdog pets, ~35 s worst case. */
    if (job->aux_id == 0u) {
        want = DC_NUKE_MAX;
        pages = 15u;
    } else if (job->aux_id >= 1u && job->aux_id <= DC_NUKE_MAX) {
        want = (uint8_t)job->aux_id;
        pages = 1u;
    } else {
        ESP_LOGW(DC_TRANSPORT_TAG, "nuke job refused (count 1..10)");
        dc_jobq_note_executed(&g_jobs, false);
        return;
    }
    if (job->channel_id == 0) {
        dc_jobq_note_executed(&g_jobs, false);
        return;
    }
    n = snprintf(chan, sizeof(chan), "%llu",
                 (unsigned long long)job->channel_id);
    if (n < 0 || (size_t)n >= sizeof(chan)) {
        dc_jobq_note_executed(&g_jobs, false);
        return;
    }
    is_all = (job->aux_id == 0u);
    for (uint8_t pg = 0; pg < pages && ok; pg++) {
        uint8_t got, nfresh = 0;
        if (before == 0) {
            n = snprintf(route, route_cap,
                         "/channels/%llu/messages?limit=%u",
                         (unsigned long long)job->channel_id, want);
        } else {
            n = snprintf(route, route_cap,
                         "/channels/%llu/messages?limit=%u&before=%llu",
                         (unsigned long long)job->channel_id, want,
                         (unsigned long long)before);
        }
        if (n < 0 || (size_t)n >= route_cap) {
            ok = false;
            break;
        }
        err = dc_rest_request(DC_REST_GET, route, NULL, s_nuke_hist,
                              sizeof(s_nuke_hist), &len);
        if (err != ESP_OK) {
            ESP_LOGW(DC_TRANSPORT_TAG, "nuke page %u: history GET failed",
                     (unsigned)pg);
            ok = false;
            break;
        }
        if (len >= sizeof(s_nuke_hist)) {
            ESP_LOGW(DC_TRANSPORT_TAG,
                     "nuke page %u: history truncated (%u B in %u B)",
                     (unsigned)pg, (unsigned)len,
                     (unsigned)sizeof(s_nuke_hist));
        }
        got = nuke_collect_ids(s_nuke_hist, len, chan, ids, want);
        ESP_LOGI(DC_TRANSPORT_TAG, "nuke page %u: %u id(s) in %u B",
                 (unsigned)pg, (unsigned)got, (unsigned)len);
        if (got == 0) {
            /* Empty page ends history (later pages), but an empty
             * FIRST page means nothing matched at all. */
            if (pg == 0) {
                ESP_LOGW(DC_TRANSPORT_TAG, "nuke page 0: no ids extracted");
                ok = false;
            } else {
                drained = true;
            }
            break;
        }
        before = ids[got - 1];
        for (uint8_t i = 0; i < got; i++) {
            if (nuke_is_fresh(ids[i], now_ms)) fresh[nfresh++] = ids[i];
        }
        if (nfresh > 1) {
            n = snprintf(work, work_cap, "{\"messages\":[");
            if (n < 0 || (size_t)n >= work_cap) {
                ok = false;
                break;
            }
            len = (size_t)n;
            for (uint8_t i = 0; i < nfresh; i++) {
                n = snprintf(work + len,
                             (len < work_cap) ? work_cap - len : 0,
                             "%s\"%llu\"", (i == 0) ? "" : ",",
                             (unsigned long long)fresh[i]);
                if (n < 0 || (size_t)n >= work_cap - len) {
                    ok = false;
                    break;
                }
                len += (size_t)n;
            }
            if (!ok) break;
            n = snprintf(work + len, (len < work_cap) ? work_cap - len : 0,
                         "]}");
            if (n < 0 || (size_t)n >= work_cap - len) {
                ok = false;
                break;
            }
            n = snprintf(route, route_cap,
                         "/channels/%llu/messages/bulk-delete",
                         (unsigned long long)job->channel_id);
            if (n < 0 || (size_t)n >= route_cap) {
                ok = false;
                break;
            }
            if (nuke_send_retry(DC_REST_POST, route, work) != ESP_OK) {
                ESP_LOGW(DC_TRANSPORT_TAG, "nuke bulk %u -> failed (status %u)",
                         (unsigned)nfresh,
                         (unsigned)dc_rest_last_status());
                ok = false;
                break;
            }
            ESP_LOGI(DC_TRANSPORT_TAG, "nuke bulk %u -> ok",
                     (unsigned)nfresh);
            bulk_n += nfresh;
        } else if (nfresh == 1) {
            /* Discord refuses bulk-delete with a single id (HTTP 400):
             * it goes the single-DELETE path below like old messages. */
            ESP_LOGI(DC_TRANSPORT_TAG, "nuke: 1 fresh id, single delete");
        }
        /* Old messages: one DELETE each (no age limit on single delete). */
        for (uint8_t i = 0; i < got && ok; i++) {
            bool is_old = true;
            for (uint8_t f = 0; f < nfresh; f++) {
                if (fresh[f] == ids[i]) {
                    is_old = false;
                    break;
                }
            }
            if (!is_old) continue;
            n = snprintf(route, route_cap, "/channels/%llu/messages/%llu",
                         (unsigned long long)job->channel_id,
                         (unsigned long long)ids[i]);
            if (n < 0 || (size_t)n >= route_cap) {
                ok = false;
                break;
            }
            if (nuke_send_retry(DC_REST_DELETE, route, NULL) != ESP_OK) {
                ESP_LOGW(DC_TRANSPORT_TAG, "nuke single %llu -> failed (%u)",
                         (unsigned long long)ids[i],
                         (unsigned)dc_rest_last_status());
                ok = false;
            } else {
                single_n++;
                vTaskDelay(pdMS_TO_TICKS(DC_NUKE_SINGLE_DELAY_MS));
            }
        }
        before = ids[got - 1];
        dc_watchdog_pet(&g_wd, g_slots[DC_TASK_REST], epoch_ms());
        if (pg + 1u < pages && ok) {
            vTaskDelay(pdMS_TO_TICKS(DC_NUKE_PAGE_DELAY_MS));
        }
    }
    ESP_LOGI(DC_TRANSPORT_TAG, "nuke -> %s%s (bulk %u, single %u)",
             ok ? "ok" : "failed",
             (!ok || (is_all && !drained)) ? " (more remain, repeat)" : "",
             (unsigned)bulk_n, (unsigned)single_n);
    dc_jobq_note_executed(&g_jobs, ok);
}

/* First `"id":<space?>"<digits>"` in buf[0..len), 0 when absent. Discord
 * channel objects (the DM-create response) lead with their "id", so a
 * forward scan over the received frame is both sufficient and bounded by
 * len; the value is parsed digit-wise, never with strtoull on a possibly
 * unterminated buffer. */
static uint64_t extract_first_id(const char *buf, size_t len)
{
    static const char pat[] = "\"id\":";
    size_t i = 0u;

    while (i + sizeof(pat) - 1u <= len) {
        size_t k;
        uint64_t id = 0u;

        for (k = 0; k < sizeof(pat) - 1u; k++) {
            if (buf[i + k] != pat[k]) break;
        }
        if (k == sizeof(pat) - 1u) {
            size_t p = i + sizeof(pat) - 1u;
            if (p < len && buf[p] == ' ') p++;
            if (p >= len || buf[p] != '"') return 0u;
            p++;
            while (p < len && buf[p] >= '0' && buf[p] <= '9') {
                id = id * 10u + (uint64_t)(buf[p] - '0');
                p++;
            }
            return id;
        }
        i++;
    }
    return 0u;
}

/* SEND_DM is two chained REST calls: POST /users/@me/channels creates (or
 * fetches) the 1:1 channel with the recipient, then POST
 * /channels/{dm_id}/messages posts the text as a Components-V2 body. The
 * payload is RAW text; dc_v2_text_body builds the document into `out` —
 * posted verbatim, the captcha DM's real newline made Discord answer 400
 * (join capture 2026-10-14), the builder escapes it now. The step-1
 * response frame is free once the DM id is extracted, so no buffer
 * beyond what rest_task already owns is needed. */
static void dm_execute(const DcJob *job, char *payload, size_t payload_cap,
                       char *route, size_t route_cap, char *out,
                       size_t out_cap)
{
    size_t out_len = 0u;
    uint64_t dm_id;
    int n;
    esp_err_t err;

    n = snprintf(payload, payload_cap, "{\"recipient_id\":\"%llu\"}",
                 (unsigned long long)job->user_id);
    if (n < 0 || (size_t)n >= payload_cap) {
        dc_jobq_note_executed(&g_jobs, false);
        return;
    }
    err = dc_rest_request(DC_REST_POST, "/users/@me/channels", payload,
                          out, out_cap, &out_len);
    if (err != ESP_OK) {
        ESP_LOGW(DC_TRANSPORT_TAG, "dm create -> failed (%u)",
                 (unsigned)dc_rest_last_status());
        dc_jobq_note_executed(&g_jobs, false);
        return;
    }
    dm_id = extract_first_id(out, out_len);
    if (dm_id == 0u) {
        ESP_LOGW(DC_TRANSPORT_TAG, "dm create without id (%u B)",
                 (unsigned)out_len);
        dc_jobq_note_executed(&g_jobs, false);
        return;
    }
    n = snprintf(route, route_cap, "/channels/%llu/messages",
                 (unsigned long long)dm_id);
    if (n < 0 || (size_t)n >= route_cap) {
        dc_jobq_note_executed(&g_jobs, false);
        return;
    }
    {
        /* Raw text -> Components-V2 container, accent from aux_id
         * (0 = default blurple). Worst case escape growth (6 B/byte on
         * 112 B) plus the template stays under 800 B inside the 1024 B
         * `out`: the escape never truncates. RX stays NULL on the send
         * below: `out` is the request body, and a 5xx retry must re-send
         * it intact (a capture would have overwritten it). */
        uint32_t accent = job->aux_id ? job->aux_id : DC_V2_ACCENT_DEFAULT;

        if (dc_v2_text_body(out, out_cap, DC_V2_FLAG, accent,
                            job->payload, job->payload_len, NULL)
            != DC_V2_OK) {
            ESP_LOGW(DC_TRANSPORT_TAG, "dm body build failed");
            dc_jobq_note_executed(&g_jobs, false);
            return;
        }
    }
    err = dc_rest_request(DC_REST_POST, route, out, NULL, 0u, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(DC_TRANSPORT_TAG, "dm send -> failed (%u)",
                 (unsigned)dc_rest_last_status());
    }
    dc_jobq_note_executed(&g_jobs, err == ESP_OK);
}

static void rest_execute(const DcJob *job, char *payload,
                             size_t payload_cap, char *route,
                             size_t route_cap, char *out,
                             size_t out_cap)
{
    DcRestMethod method;
    bool has_body;
    esp_err_t err;
    const char *body = NULL;
    char *rx = out;
    size_t rx_cap = out_cap;

    /* NUKE is multi-step (history GET, id extract, bulk POST): it bypasses
     * the single-request job_route and owns the shared buffers. */
    if (job->kind == DC_JOB_NUKE) {
        nuke_execute(job, out, out_cap, route, route_cap);
        return;
    }
    if (job->kind == DC_JOB_SEND_DM) {
        dm_execute(job, payload, payload_cap, route, route_cap, out, out_cap);
        return;
    }
    if (job->kind == DC_JOB_TIMEOUT_USER) {
        /* Rendered locally; never carries secrets, only a UTC timestamp. */
        if (timeout_body(job, payload, payload_cap) != ESP_OK) {
            ESP_LOGW(DC_TRANSPORT_TAG, "timeout job without clock/duration");
            dc_jobq_note_executed(&g_jobs, false);
            return;
        }
        body = payload;
    } else if (job->kind == DC_JOB_EDIT_MESSAGE) {
        /* Payload shape "msg_id|json": the route needs the 64-bit id and
         * the body needs the JSON, and 48 B cannot hold both twice. */
        size_t off = 0;
        size_t blen;
        while (off < job->payload_len && off < DC_JOB_PAYLOAD &&
               job->payload[off] != '|') {
            off++;
        }
        if (off >= job->payload_len || off + 1 >= payload_cap) {
            ESP_LOGW(DC_TRANSPORT_TAG, "edit job without id|body shape");
            dc_jobq_note_executed(&g_jobs, false);
            return;
        }
        blen = (size_t)job->payload_len - off - 1;
        if (blen >= payload_cap) blen = payload_cap - 1;
        memcpy(payload, &job->payload[off + 1], blen);
        payload[blen] = '\0';
        body = payload;
    } else if (job->kind == DC_JOB_SEND_MESSAGE) {
        /* Raw text -> Components-V2 body, built here (see dm_execute).
         * Worst case 6*112 escaped plus the template stays under 800 B
         * inside `out` (1024): never truncates. `out` becomes the
         * request body, so RX moves to NULL and a 5xx retry re-sends it
         * intact. */
        uint32_t accent = job->aux_id ? job->aux_id : DC_V2_ACCENT_DEFAULT;

        if (dc_v2_text_body(out, out_cap, DC_V2_FLAG, accent,
                            job->payload, job->payload_len, NULL)
            != DC_V2_OK) {
            ESP_LOGW(DC_TRANSPORT_TAG, "send job body build failed");
            dc_jobq_note_executed(&g_jobs, false);
            return;
        }
        body = out;
        rx = NULL;
        rx_cap = 0u;
    } else {
        job_payload(job, payload, payload_cap);
        body = payload;
    }
    err = job_route(job, body, route, route_cap, &method,
                    &has_body);
    if (err != ESP_OK) {
        ESP_LOGW(DC_TRANSPORT_TAG, "job kind %u route failed",
                 (unsigned)job->kind);
        dc_jobq_note_executed(&g_jobs, false);
        return;
    }
    err = dc_rest_request(method, route,
                          has_body ? body : NULL,
                          rx, rx_cap, NULL);
    dc_jobq_note_executed(&g_jobs, err == ESP_OK);
}

static void rest_task(void *arg)
{
    DcJob job;
    char payload[DC_JOB_PAYLOAD + 1u];
    char route[DC_REST_ROUTE_MAX];
    char out[DC_REST_RX_BUFFER];
    uint32_t idle_ticks = 0u;

    (void)arg;
    for (;;) {
        if (dc_jobq_peek(&g_jobs, &job)) {
            /* peek copies; rest_execute ends in dc_jobq_note_executed,
             * which advances the head AND counts. No pop here: that
             * would advance twice and skip every other job. */
            rest_execute(&job, payload, sizeof(payload), route,
                         sizeof(route), out, sizeof(out));
            idle_ticks = 0u;
        } else {
            idle_ticks++;
            if (idle_ticks >= (15000u / DC_REST_POLL_MS)) {
                idle_ticks = 0u;
                if (g_state == DC_TR_READY) {
                    (void)dc_rest_keepalive();
                }
            }
        }
        dc_watchdog_pet(&g_wd, g_slots[DC_TASK_REST], epoch_ms());
        vTaskDelay(pdMS_TO_TICKS(DC_REST_POLL_MS));
    }
}

static void bot_task(void *arg)
{
    uint32_t elapsed_ms = 0u;
    uint32_t last_log_ms = 0u;

    (void)arg;
    for (;;) {
        dc_watchdog_pet(&g_wd, g_slots[DC_TASK_BOT], epoch_ms());
        dispatch_pump();
        /* Wall-clock accumulator instead of an iteration count: the tick is
         * 20 ms now, so "seconds" would advance fifty times faster. */
        elapsed_ms += DC_BOT_POLL_MS;
        if (elapsed_ms - last_log_ms >= DC_LOG_EVERY_S * 1000u) {
            last_log_ms = elapsed_ms;
            ESP_LOGI(DC_TRANSPORT_TAG,
                     "bot heap %u ackq %u jobs %u intq %u/%u drops %u "
                     "peak %u msg_shed %u",
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)dc_ackq_depth(dc_ws_ackq()),
                     (unsigned)dc_jobq_count(&g_jobs),
                     (unsigned)dc_intq_depth(&g_intq),
                     (unsigned)DC_INTQ_CAPACITY,
                     (unsigned)dc_intq_dropped(&g_intq),
                     (unsigned)dc_intq_peak(&g_intq),
                     (unsigned)g_msg_sheds);
        }
        vTaskDelay(pdMS_TO_TICKS(DC_BOT_POLL_MS));
    }
}

static void monitor_log_heap(const char *milestone)
{
    uint32_t free = esp_get_free_heap_size();
    size_t largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_8BIT);
    uint32_t frag = 0u;

    if (free > 0u) {
        frag = 100u - (uint32_t)((uint64_t)largest * 100u /
                                 (uint64_t)free);
    }
    ESP_LOGI(DC_TRANSPORT_TAG,
             "%s heap %u largest %u frag %u%%",
             milestone, free, (unsigned)largest, frag);
}

static void monitor_log_tasks(void)
{
    for (uint32_t i = 0u; i < DC_TRANSPORT_TASK_COUNT; i++) {
        ESP_LOGI(DC_TRANSPORT_TAG, "%s stack hwm %u bytes",
                 TASK_NAMES[i],
                 (unsigned)(uxTaskGetStackHighWaterMark(
                                g_tasks[i]) *
                            sizeof(StackType_t)));
    }
}

static void monitor_log(void)
{
    monitor_log_heap("telemetry");
    monitor_log_tasks();
}

static void monitor_log_tripped(const DcWatchdogReport *reports,
                                    uint32_t count)
{
    for (uint32_t i = 0u; i < count; i++) {
        ESP_LOGE(DC_TRANSPORT_TAG, "watchdog %s overdue %u ms",
                 reports[i].name, (unsigned)reports[i].overdue_ms);
    }
    if (count == 0u) {
        ESP_LOGE(DC_TRANSPORT_TAG, "watchdog unhealthy");
    }
}

static void monitor_scan(void)
{
    DcWatchdogReport reports[DC_WD_TASK_SLOTS];
    DcGwState gw_state;
    uint32_t found;

    found = dc_watchdog_scan(&g_wd, epoch_ms(), reports,
                             DC_WD_TASK_SLOTS);
    if (!dc_watchdog_healthy(&g_wd)) {
        monitor_log_tripped(reports, found);
        esp_restart();
    }
    gw_state = dc_ws_gateway()->state;
    if (gw_state == DC_GW_READY && g_state != DC_TR_READY) {
        g_state = DC_TR_READY;
        monitor_log_heap("ready");
    } else if (gw_state == DC_GW_FATAL &&
               g_state != DC_TR_FATAL) {
        g_state = DC_TR_FATAL;
        ESP_LOGE(DC_TRANSPORT_TAG, "gateway fatal close %u",
                 (unsigned)dc_ws_gateway()->close_code);
    }
}

static void monitor_task(void *arg)
{
    uint32_t seconds = 0u;

    (void)arg;
    for (;;) {
        monitor_scan();
        if (seconds % DC_LOG_EVERY_S == 0u) {
            monitor_log();
        }
        seconds++;
        dc_watchdog_pet(&g_wd, g_slots[DC_TASK_MONITOR], epoch_ms());
        vTaskDelay(pdMS_TO_TICKS(DC_MONITOR_POLL_MS));
    }
}

static esp_err_t spawn(uint32_t index)
{
    if (xTaskCreate(TASK_FUNCS[index], TASK_NAMES[index],
                    TASK_STACKS[index], NULL, TASK_PRIOS[index],
                    &g_tasks[index]) != pdTRUE) {
        ESP_LOGE(DC_TRANSPORT_TAG, "task %s create failed",
                 TASK_NAMES[index]);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t start_tasks(void)
{
    esp_err_t err;

    /* Before any task runs: the producer is the WebSocket task, which starts
     * decoding frames as soon as spawn() returns. */
    dc_intq_init(&g_intq);
    /* Kept across a stop/start cycle (the provisioning console restarts the
     * transport): created once, never deleted, so no handler can observe a
     * mutex disappearing under it. */
    if (g_dispatch_lock == NULL) {
        g_dispatch_lock = xSemaphoreCreateMutex();
        if (g_dispatch_lock == NULL) {
            ESP_LOGE(DC_TRANSPORT_TAG, "dispatch lock out of memory");
            return ESP_ERR_NO_MEM;
        }
    }
    dc_watchdog_init(&g_wd, DC_WD_TOLERANCE);
    for (uint32_t i = 0u; i < DC_TRANSPORT_TASK_COUNT; i++) {
        err = spawn(i);
        if (err != ESP_OK) {
            return err;
        }
    }
    for (uint32_t i = 0u; i < DC_TRANSPORT_TASK_COUNT; i++) {
        g_slots[i] = dc_watchdog_register(&g_wd, TASK_NAMES[i],
                                              TASK_PERIODS[i]);
        if (g_slots[i] < 0) {
            ESP_LOGE(DC_TRANSPORT_TAG,
                     "watchdog register %s failed",
                     TASK_NAMES[i]);
            return ESP_FAIL;
        }
        dc_watchdog_pet(&g_wd, g_slots[i], epoch_ms());
    }
    return ESP_OK;
}

static void stop_tasks(void)
{
    for (uint32_t i = 0u; i < DC_TRANSPORT_TASK_COUNT; i++) {
        if (g_tasks[i] != NULL) {
            vTaskDelete(g_tasks[i]);
            g_tasks[i] = NULL;
        }
        g_slots[i] = -1;
    }
}

esp_err_t discord_init(void)
{
    DcIdentifyStore store;
    esp_err_t err;

    if (g_initialized) {
        return ESP_OK;
    }
    dc_registry_init(&g_registry);
    dc_ratelimit_init(&g_rl, boot_ms());
    dc_watchdog_init(&g_wd, DC_WD_TOLERANCE);
    dc_jobq_init(&g_jobs, DC_JOB_DROP_OLDEST);
    err = load_token();
    if (err != ESP_OK) {
        ESP_LOGE(DC_TRANSPORT_TAG, "token read failed 0x%x",
                 (unsigned)err);
        return err;
    }
    err = dc_rest_init(g_token, &g_rl);
    if (err != ESP_OK) {
        ESP_LOGE(DC_TRANSPORT_TAG, "rest init failed 0x%x",
                 (unsigned)err);
        return err;
    }
    dc_ws_set_ca_store((const unsigned char *)DC_DISCORD_ROOT_CA_PEM,
                       sizeof(DC_DISCORD_ROOT_CA_PEM));
    dc_ws_set_token_provider(token_provider);
    dc_ws_set_intents(dc_registry_intents(&g_registry));
    dc_ws_set_interaction_handler(on_interaction);
    dc_ws_set_message_handler(on_message);
    dc_ws_set_modal_for(modal_for_command);
    dc_ws_set_ephemeral_for(ephemeral_for_command);
    store = dc_nvs_identify_store();
    dc_ws_set_identify_store(&store);
    g_initialized = true;
    return ESP_OK;
}

esp_err_t discord_start(void)
{
    esp_err_t err;

    if (!g_initialized) {
        ESP_LOGE(DC_TRANSPORT_TAG, "start before init");
        return ESP_FAIL;
    }
    if (g_state == DC_TR_STARTING ||
        g_state == DC_TR_READY) {
        return ESP_OK;
    }
    if (g_state == DC_TR_FATAL) {
        ESP_LOGE(DC_TRANSPORT_TAG, "start after fatal, stop first");
        return ESP_FAIL;
    }
    err = start_tasks();
    if (err != ESP_OK) {
        stop_tasks();
        return err;
    }
    (void)dc_rest_prewarm();
    monitor_log_heap("after tls");
    if (dc_rest_last_status() == DC_TRANSPORT_HTTP_401) {
        ESP_LOGE(DC_TRANSPORT_TAG, "token rejected, fatal");
        stop_tasks();
        return ESP_FAIL;
    }
    {
        /* Boot evidence: the exact mask IDENTIFY will carry. A raw
         * compact-bit 6 here once meant MESSAGE_CREATE never arrived. */
        uint32_t mask = dc_registry_intents(&g_registry);

        dc_ws_set_intents(mask);
        ESP_LOGI(DC_TRANSPORT_TAG, "gateway intents mask %u", (unsigned)mask);
    }
    if (dc_ws_start() != ESP_OK) {
        ESP_LOGE(DC_TRANSPORT_TAG, "gateway start failed");
        stop_tasks();
        return ESP_FAIL;
    }
    g_state = DC_TR_STARTING;
    return ESP_OK;
}

void discord_stop(void)
{
    if (!g_initialized) {
        return;
    }
    dc_ws_stop();
    stop_tasks();
    dc_rest_deinit();
    g_state = DC_TR_IDLE;
    g_initialized = false;
}

DcTransportState discord_state(void)
{
    return g_state;
}

DiscordFeatureRegistry *discord_registry(void)
{
    return &g_registry;
}

DcBotState *discord_bot_state(void)
{
    return &g_bot_state;
}

bool discord_job_push(const DcJob *job)
{
    DcJob copy;

    if (job == NULL || job->kind == DC_JOB_NONE ||
        job->kind > DC_JOB_SEND_DM) {
        return false;
    }
    /* IDs only + aux duration: a DcJob structurally cannot carry the bot
     * token (~70 chars > the 112-byte payload), so queued work never leaks
     * it. */
    copy = *job;
    if (copy.payload_len > DC_JOB_PAYLOAD) {
        return false;
    }
    return dc_jobq_push(&g_jobs, &copy) == DC_JOB_OK;
}

bool discord_set_activity(uint8_t type, const char *name, const char *url,
                          bool *live)
{
    bool pushed;

    if (!dc_ws_set_activity(type, name, url)) {
        return false;
    }
    pushed = dc_ws_push_activity();
    if (live != NULL) {
        *live = pushed;
    }
    return true;
}

void discord_job_stats(uint32_t *depth, uint32_t *peak, uint64_t *dropped)
{
    if (depth != NULL) *depth = dc_jobq_count(&g_jobs);
    if (peak != NULL) *peak = dc_jobq_peak(&g_jobs);
    if (dropped != NULL) *dropped = g_jobs.dropped;
}

void discord_on_member(DcWsMemberHandler handler)
{
    g_member_handler = handler;
    dc_ws_set_member_handler(handler != NULL ? member_event_sink : NULL);
}

void discord_set_modal_builder(DcWsModalBuilder fn)
{
    dc_ws_set_modal_builder(fn);
}

/* discord_interaction_reply lives in discord_reply.c (AGENT.md §1 split:
 * transport.c stays the controller, the followup path owns its buffer,
 * escaping and ACK-ordering there). */
