#include "discord_ws.h"

#include <string.h>
#include <time.h>

#include "bot_config.h"
#include "discord_etf.h"
#include "discord_json.h"
#include "discord_ws_decode.h"

#include "esp_log.h"
#include "esp_tls.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"

#define DC_WS_TAG "discord_ws"

#define DC_WS_URI "wss://gateway.discord.gg/?encoding=etf&v=10"
#define DC_WS_CLIENT_STACK 8192
#define DC_WS_CLIENT_PRIO 5
#define DC_WS_TX_BUFFER 512
#define DC_WS_LOG_EVERY 1024u
#define DC_WS_OP_BINARY 2
#define DC_WS_OP_CLOSE 8

static DcGateway gw;
static DcAckq ackq;
static DcIdentify identify;
static DcIdentifyStore identify_store = { NULL, NULL, NULL };
static DcWsTokenProvider token_provider;
static DcWsInteractionHandler interaction_handler;
static DcWsMessageHandler message_handler;
static DcWsMemberHandler member_handler;
static const char *(*modal_for)(const char *command, const char *sub);
static DcWsModalBuilder modal_build;
static bool (*ephemeral_for)(const char *command);
/* Staged activity ("playing X"): pushed live as an OP3 PRESENCE_UPDATE
 * when the session is READY (discord.js setActivity parity via
 * dc_ws_push_activity, called by dc_bot) and kept staged so every fresh
 * IDENTIFY replays it — RESUME keeps the previous presence server-side. */
static char staged_activity[64];
static char staged_activity_url[96];
static uint8_t staged_activity_type;
static bool activity_staged;
static uint32_t intents;
static const unsigned char *ca_pem;
static esp_websocket_client_handle_t client;
static bool client_started;
static bool fatal_latch;
static bool want_stop;
static uint32_t pending_close_code;
static uint32_t dropped_frames;
static uint8_t rx_buf[DC_GW_RX_BUFFER];
static char tx_event[DC_WS_TX_BUFFER];
static char tx_task[DC_WS_TX_BUFFER];
/* OP3 push frame: dc_bot (dc_ws_push_activity's only caller) owns it. */
static char tx_presence[DC_WS_TX_BUFFER];

static uint64_t clock_ms(void)
{
    return (uint64_t)time(NULL) * 1000u;
}

static void note_drop(void)
{
    dropped_frames++;
    if (dropped_frames == 1u || dropped_frames % DC_WS_LOG_EVERY == 0u) {
        ESP_LOGW(DC_WS_TAG, "frame dropped, total %u", dropped_frames);
    }
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}

/* Frames share one path: the gateway task (heartbeat), the ws task
 * (identify/resume) and dc_bot (OP3 presence push) all land here. The IDF
 * client serializes tx internally (tx_lock), so a push from dc_bot cannot
 * interleave with a heartbeat frame. True when the whole frame left. */
static bool send_frame(const char *data, size_t len)
{
    int sent;

    if (client == NULL || !client_started || len == 0u) {
        return false;
    }
    sent = esp_websocket_client_send_bin(client, data, (int)len,
                                          pdMS_TO_TICKS(2000));
    if (sent != (int)len) {
        ESP_LOGW(DC_WS_TAG, "send returned %d of %u bytes", sent,
                 (unsigned)len);
        return false;
    }
    return true;
}

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t pos;
    bool overflow;
} DcEtfWriter;

static void etf_init(DcEtfWriter *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->pos = 0;
    w->overflow = false;
    if (cap > 0) {
        w->buf[w->pos++] = 131; /* FORMAT_VERSION */
    } else {
        w->overflow = true;
    }
}

static void etf_u8(DcEtfWriter *w, uint8_t v)
{
    if (w->pos < w->cap) {
        w->buf[w->pos++] = v;
    } else {
        w->overflow = true;
    }
}

static void etf_u16(DcEtfWriter *w, uint16_t v)
{
    etf_u8(w, (uint8_t)(v >> 8));
    etf_u8(w, (uint8_t)v);
}

static void etf_u32(DcEtfWriter *w, uint32_t v)
{
    etf_u8(w, (uint8_t)(v >> 24));
    etf_u8(w, (uint8_t)(v >> 16));
    etf_u8(w, (uint8_t)(v >> 8));
    etf_u8(w, (uint8_t)v);
}

static void etf_map_header(DcEtfWriter *w, uint32_t pair_count)
{
    etf_u8(w, 116); /* MAP_EXT */
    etf_u32(w, pair_count);
}

static void etf_binary(DcEtfWriter *w, const char *s, size_t len)
{
    etf_u8(w, 109); /* BINARY_EXT */
    etf_u32(w, (uint32_t)len);
    if (w->pos + len <= w->cap) {
        memcpy(w->buf + w->pos, s, len);
        w->pos += len;
    } else {
        w->overflow = true;
    }
}

static void etf_binary_str(DcEtfWriter *w, const char *s)
{
    etf_binary(w, s, strlen(s));
}

static void etf_small_int(DcEtfWriter *w, uint8_t v)
{
    etf_u8(w, 97); /* SMALL_INTEGER_EXT */
    etf_u8(w, v);
}

static void etf_int(DcEtfWriter *w, int32_t v)
{
    if (v >= 0 && v <= 255) {
        etf_small_int(w, (uint8_t)v);
    } else {
        etf_u8(w, 98); /* INTEGER_EXT */
        etf_u32(w, (uint32_t)v);
    }
}

/* ATOM_EXT with an ASCII name. Discord exchanges atoms for booleans/null
 * in ETF (its own booleans decode from atoms — see read_boolean in
 * discord_ws_decode.c), so "false"/"nil" go out in that form. */
static void etf_atom(DcEtfWriter *w, const char *name)
{
    size_t len = strlen(name);

    etf_u8(w, 100); /* ATOM_EXT */
    etf_u16(w, (uint16_t)len);
    while (*name != '\0') {
        etf_u8(w, (uint8_t)*name);
        name++;
    }
}

/* NIL_EXT: the empty list, and by spec the tail of every proper list
 * (Erlang's term_to_binary always terminates [a, b] with 106). */
static void etf_nil_ext(DcEtfWriter *w)
{
    etf_u8(w, 106);
}

static void etf_list_header(DcEtfWriter *w, uint32_t count)
{
    etf_u8(w, 108); /* LIST_EXT */
    etf_u32(w, count);
    /* Elements follow; the caller closes with etf_nil_ext() as the tail. */
}

/* One-entry activities list for the presence object: name, type, and for
 * streaming (type 1) the url. Shared by IDENTIFY's presence and the OP3
 * push so both always describe the same staged activity. */
static void etf_activity_list(DcEtfWriter *w)
{
    etf_list_header(w, 1);
    etf_map_header(w, (staged_activity_type == 1) ? 3u : 2u);
    etf_binary_str(w, "name");
    etf_binary_str(w, staged_activity);
    etf_binary_str(w, "type");
    etf_small_int(w, staged_activity_type);
    if (staged_activity_type == 1) {
        etf_binary_str(w, "url");
        etf_binary_str(w, staged_activity_url);
    }
    etf_nil_ext(w);
}

static void send_heartbeat(char *tx, size_t cap)
{
    DcEtfWriter w;

    etf_init(&w, (uint8_t *)tx, cap);
    etf_map_header(&w, 2);
    etf_binary_str(&w, "op");
    etf_small_int(&w, DC_GW_OP_HEARTBEAT);
    etf_binary_str(&w, "d");
    if (gw.sequence_valid) {
        etf_int(&w, (int32_t)gw.sequence);
    } else {
        /* JSON null: Discord encodes it as the ATOM nil, not NIL_EXT. */
        etf_atom(&w, "nil");
    }
    if (!w.overflow) {
        send_frame((const char *)w.buf, w.pos);
    }
}

static bool fetch_token(const char **token, size_t *len)
{
    if (token_provider == NULL) {
        return false;
    }
    *token = token_provider(len);
    return *token != NULL && *len > 0u;
}

static void enter_fatal(const char *reason)
{
    ESP_LOGE(DC_WS_TAG, "fatal: %s", reason);
    fatal_latch = true;
    want_stop = true;
}

static void send_identify(uint64_t now, char *tx, size_t cap)
{
    const char *token;
    size_t token_len;
    DcEtfWriter w;

    if (!dc_identify_may_send(&identify, now)) {
        enter_fatal("identify budget exhausted, no retry");
        return;
    }
    if (!dc_identify_record(&identify, now)) {
        enter_fatal("identify store write failed");
        return;
    }
    if (!fetch_token(&token, &token_len)) {
        enter_fatal("no bot token provisioned");
        return;
    }

    etf_init(&w, (uint8_t *)tx, cap);
    etf_map_header(&w, 2);
    etf_binary_str(&w, "op");
    etf_small_int(&w, DC_GW_OP_IDENTIFY);
    etf_binary_str(&w, "d");
    etf_map_header(&w, 4);
    etf_binary_str(&w, "token");
    etf_binary(&w, token, token_len);
    etf_binary_str(&w, "intents");
    etf_int(&w, (int32_t)intents);
    etf_binary_str(&w, "properties");
    etf_map_header(&w, 3);
    etf_binary_str(&w, "$os");
    etf_binary_str(&w, "esp32");
    etf_binary_str(&w, "$browser");
    etf_binary_str(&w, "esp32");
    etf_binary_str(&w, "$device");
    etf_binary_str(&w, "esp32");
    /* Presence always sent so the bot boots as Do Not Disturb even
     * without a staged activity; activities only when staged. Canonical
     * Gateway Presence Update shape: status, since (nil), afk (false),
     * activities (when staged). */
    etf_binary_str(&w, "presence");
    etf_map_header(&w, activity_staged ? 4u : 3u);
    etf_binary_str(&w, "status");
    etf_binary_str(&w, "dnd");
    etf_binary_str(&w, "since");
    etf_atom(&w, "nil");
    etf_binary_str(&w, "afk");
    etf_atom(&w, "false");
    if (activity_staged) {
        etf_binary_str(&w, "activities");
        etf_activity_list(&w);
    }

    if (!w.overflow) {
        send_frame((const char *)w.buf, w.pos);
    }
}

static void send_resume(char *tx, size_t cap)
{
    const char *token;
    size_t token_len;
    DcEtfWriter w;

    if (!fetch_token(&token, &token_len)) {
        enter_fatal("no bot token provisioned");
        return;
    }

    etf_init(&w, (uint8_t *)tx, cap);
    etf_map_header(&w, 2);
    etf_binary_str(&w, "op");
    etf_small_int(&w, DC_GW_OP_RESUME);
    etf_binary_str(&w, "d");
    etf_map_header(&w, 3);
    etf_binary_str(&w, "token");
    etf_binary(&w, token, token_len);
    etf_binary_str(&w, "session_id");
    etf_binary_str(&w, gw.session_id);
    etf_binary_str(&w, "seq");
    etf_int(&w, (int32_t)gw.sequence);

    if (!w.overflow) {
        send_frame((const char *)w.buf, w.pos);
    }
}

static void stop_client(void)
{
    if (client == NULL) {
        return;
    }
    if (client_started) {
        esp_websocket_client_stop(client);
    }
    esp_websocket_client_destroy(client);
    client = NULL;
    client_started = false;
}

static void ws_event_cb(void *handler_args, esp_event_base_t base,
                        int32_t event_id, void *event_data);

static esp_err_t start_client(void)
{
    esp_websocket_client_config_t cfg = {
        .uri = DC_WS_URI,
        .buffer_size = DC_GW_RX_BUFFER,
        .task_stack = DC_WS_CLIENT_STACK,
        .task_prio = DC_WS_CLIENT_PRIO,
        .disable_auto_reconnect = true,
        .user_context = NULL,
        .use_global_ca_store = true,
    };

    client = esp_websocket_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(DC_WS_TAG, "client init failed");
        return ESP_FAIL;
    }
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_event_cb, NULL);
    if (esp_websocket_client_start(client) != ESP_OK) {
        ESP_LOGE(DC_WS_TAG, "client start failed");
        esp_websocket_client_destroy(client);
        client = NULL;
        return ESP_FAIL;
    }
    client_started = true;
    return ESP_OK;
}

static void execute_event_action(DcGwAction *a, uint64_t now, char *tx,
                                 size_t cap)
{
    switch (a->event) {
    case DC_GW_EV_SEND_IDENTIFY:
        send_identify(now, tx, cap);
        break;
    case DC_GW_EV_SEND_RESUME:
        send_resume(tx, cap);
        break;
    case DC_GW_EV_SEND_HEARTBEAT:
        send_heartbeat(tx, cap);
        break;
    case DC_GW_EV_RECONNECT:
        want_stop = true;
        break;
    case DC_GW_EV_FATAL:
        enter_fatal("close code is fatal");
        break;
    case DC_GW_EV_QUOTA_EXHAUSTED:
        enter_fatal("identify quota exhausted");
        break;
    default:
        break;
    }
}

static void transition(DcWsFrame *frame)
{
    uint64_t now = clock_ms();
    DcGwAction a;

    if (frame->have_seq) {
        gw.sequence = frame->seq;
        gw.sequence_valid = true;
    }
    if (frame->op == DC_GW_OP_DISPATCH) {
        if (discord_etf_slice_equals(&frame->type, "READY") ||
            discord_etf_slice_equals(&frame->type, "RESUMED")) {
            gw.state = DC_GW_READY;
            if (frame->have_session_id) {
                memcpy(gw.session_id, frame->session_id,
                       sizeof(frame->session_id));
            }
            if (frame->have_resume_url) {
                memcpy(gw.resume_url, frame->resume_url,
                       sizeof(frame->resume_url));
            }
        }
        dc_gateway_on_op(&gw, DC_GW_OP_DISPATCH, false, now);
        return;
    }
    if (frame->op == DC_GW_OP_HELLO) {
        a = dc_gateway_on_hello(&gw, frame->interval, now);
    } else if (frame->op == DC_GW_OP_INVALID_SESSION) {
        a = dc_gateway_on_op(&gw, DC_GW_OP_INVALID_SESSION,
                             frame->resumable, now);
    } else if (frame->op == DC_GW_OP_HEARTBEAT) {
        a = dc_gateway_on_op(&gw, DC_GW_OP_HEARTBEAT, false, now);
    } else if (frame->op == DC_GW_OP_RECONNECT) {
        a = dc_gateway_on_op(&gw, DC_GW_OP_RECONNECT, false, now);
    } else if (frame->op == DC_GW_OP_HEARTBEAT_ACK) {
        a = dc_gateway_on_heartbeat_ack(&gw, now);
    } else {
        return;
    }
    execute_event_action(&a, now, tx_event, sizeof(tx_event));
}

static void handle_frame(const uint8_t *data, size_t len)
{
    DcWsFrame frame;
    const uint8_t *d_data = NULL;
    size_t d_len = 0u;

    memset(&frame, 0, sizeof(frame));
    if (!discord_ws_decode_frame(data, len, &frame, &d_data, &d_len)) {
        ESP_LOGW(DC_WS_TAG, "drop: decode_frame failed for %u bytes (first byte 0x%02x)",
                 (unsigned)len, len > 0 ? data[0] : 0);
        note_drop();
        return;
    }
    ESP_LOGI(DC_WS_TAG, "rx frame: op %u, type %.*s", (unsigned)frame.op,
             (int)frame.type.len, frame.type.ptr != NULL ? (const char *)frame.type.ptr : "");
    if (d_data != NULL) {
        switch (frame.op) {
        case DC_GW_OP_HELLO:
            frame.interval = discord_ws_decode_hello(d_data, d_len);
            ESP_LOGI(DC_WS_TAG, "HELLO interval %u ms", (unsigned)frame.interval);
            break;
        case DC_GW_OP_INVALID_SESSION:
            frame.resumable =
                discord_ws_decode_invalid_session(d_data, d_len);
            break;
        case DC_GW_OP_DISPATCH:
            discord_ws_decode_dispatch(&frame, d_data, d_len);
            break;
        default:
            break;
        }
    }
    transition(&frame);
}

static void reassemble(const uint8_t *data, int data_len, int payload_len,
                       int payload_offset)
{
    if (payload_len <= 0 || payload_len > (int)sizeof(rx_buf)) {
        ESP_LOGW(DC_WS_TAG, "drop: payload_len %d exceeds rx_buf %d",
                 payload_len, (int)sizeof(rx_buf));
        note_drop();
        return;
    }
    if (data == NULL || data_len <= 0 || payload_offset < 0 ||
        payload_offset + data_len > (int)sizeof(rx_buf)) {
        ESP_LOGW(DC_WS_TAG, "drop: invalid data_len %d offset %d",
                 data_len, payload_offset);
        note_drop();
        return;
    }
    memcpy(rx_buf + payload_offset, data, (size_t)data_len);
    if (payload_offset + data_len < payload_len) {
        return;
    }
    handle_frame(rx_buf, (size_t)payload_len);
}

static void on_data(const esp_websocket_event_data_t *event)
{
    if (event == NULL) {
        return;
    }
    if (event->op_code == DC_WS_OP_CLOSE) {
        if (event->data_len >= 2 && event->data_ptr != NULL) {
            pending_close_code = be16((const uint8_t *)event->data_ptr);
        }
        ESP_LOGW(DC_WS_TAG, "ws close received: code %u", (unsigned)pending_close_code);
        return;
    }
    ESP_LOGI(DC_WS_TAG, "ws event: op_code %u, data_len %d, payload_len %d, offset %d",
             (unsigned)event->op_code, event->data_len, event->payload_len, event->payload_offset);
    if (event->op_code == 0x09 || event->op_code == 0x0A) {
        return;
    }
    if (event->op_code != DC_WS_OP_BINARY && event->op_code != 0x00) {
        ESP_LOGW(DC_WS_TAG, "drop: opcode %u is not BINARY (2)", (unsigned)event->op_code);
        note_drop();
        return;
    }
    reassemble((const uint8_t *)event->data_ptr, event->data_len,
               event->payload_len, event->payload_offset);
}

static void on_disconnected(const esp_websocket_event_data_t *event)
{
    uint64_t now = clock_ms();
    uint32_t code = pending_close_code;
    DcGwAction a;

    pending_close_code = 0u;
    if (code == 0u && event != NULL) {
        if (event->close_status_code > 0) {
            code = (uint32_t)event->close_status_code;
        } else if (event->data_ptr != NULL && event->data_len >= 2) {
            code = be16((const uint8_t *)event->data_ptr);
        }
    }
    a = dc_gateway_on_close(&gw, code, now);
    execute_event_action(&a, now, tx_event, sizeof(tx_event));
}

static void ws_event_cb(void *handler_args, esp_event_base_t base,
                        int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    const esp_websocket_event_data_t *event =
        (const esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        uint64_t now = clock_ms();
        DcGwAction a = dc_gateway_on_connected(&gw, now);

        execute_event_action(&a, now, tx_event, sizeof(tx_event));
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
        on_disconnected(event);
        break;
    case WEBSOCKET_EVENT_DATA:
        on_data(event);
        break;
    case WEBSOCKET_EVENT_ERROR:
        if (event != NULL && event->data_len > 0 && event->data_ptr != NULL) {
            ESP_LOGW(DC_WS_TAG, "client error: %.*s", event->data_len,
                     event->data_ptr);
        } else {
            ESP_LOGW(DC_WS_TAG, "client error");
        }
        break;
    default:
        break;
    }
}

void dc_ws_set_ca_store(const unsigned char *pem, size_t len)
{
    ca_pem = pem;
    if (pem != NULL && len > 0u &&
        esp_tls_set_global_ca_store(pem, len) != ESP_OK) {
        ESP_LOGE(DC_WS_TAG, "global CA store install failed");
    }
}

void dc_ws_set_token_provider(DcWsTokenProvider provider)
{
    token_provider = provider;
}

void dc_ws_set_intents(uint32_t value)
{
    intents = value;
}

void dc_ws_set_interaction_handler(DcWsInteractionHandler handler)
{
    interaction_handler = handler;
}

void dc_ws_set_message_handler(DcWsMessageHandler handler)
{
    message_handler = handler;
}

void dc_ws_set_modal_for(const char *(*fn)(const char *command,
                                           const char *sub))
{
    modal_for = fn;
}

void dc_ws_set_modal_builder(DcWsModalBuilder fn)
{
    modal_build = fn;
}

void dc_ws_set_ephemeral_for(bool (*fn)(const char *command))
{
    ephemeral_for = fn;
}

void dc_ws_set_member_handler(DcWsMemberHandler handler)
{
    member_handler = handler;
}

/* Activity types: 0 playing, 2 listening, 3 watching, 5 competing,
 * 1 streaming (needs a twitch/youtube URL, enforced by Discord).
 * 4 (custom) is user-only: refused. Name is 1..63 chars. */
static bool ci_prefix(const char *s, const char *pfx)
{
    size_t i;

    for (i = 0; pfx[i] != '\0'; i++) {
        char a = s[i];
        char b = pfx[i];

        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

/* Node's setactivity regex is /i, so the host compare is case-insensitive
 * too; the https:// prefix stays REQUIRED (IDENTIFY needs an absolute
 * URL — the command layer normalizes http/schemeless to https). */
static bool url_is_streamable(const char *url)
{
    static const char *const ok[] = {
        "https://twitch.tv/", "https://www.twitch.tv/",
        "https://youtube.com/", "https://www.youtube.com/",
        "https://youtu.be/",
    };
    size_t i;

    if (url == NULL) return false;
    for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        if (ci_prefix(url, ok[i])) return true;
    }
    return false;
}

bool dc_ws_set_activity(uint8_t type, const char *name, const char *url)
{
    size_t len;

    if (name == NULL) return false;
    if (type != 0 && type != 1 && type != 2 && type != 3 && type != 5) {
        return false;
    }
    len = strlen(name);
    if (len == 0 || len >= sizeof(staged_activity)) return false;
    if (type == 1) {
        size_t ulen;
        if (!url_is_streamable(url)) return false;
        ulen = strlen(url);
        if (ulen >= sizeof(staged_activity_url)) return false;
        memcpy(staged_activity_url, url, ulen + 1);
    } else {
        staged_activity_url[0] = '\0';
    }
    memcpy(staged_activity, name, len + 1);
    staged_activity_type = type;
    activity_staged = true;
    return true;
}

/* Pushes the staged activity live as OP3 PRESENCE_UPDATE when the session
 * is READY — discord.js setActivity parity. dc_bot (the interaction
 * handler task) is the only caller, so tx_presence needs no lock; the
 * frame then rides send_frame's shared, client-serialized path. False
 * when not READY or the send failed: the staged value then lands at the
 * next IDENTIFY (reconnect fallback, never a forced reconnect — that
 * would spend IDENTIFY quota). */
bool dc_ws_push_activity(void)
{
    DcEtfWriter w;

    if (!activity_staged || gw.state != DC_GW_READY) {
        return false;
    }
    etf_init(&w, (uint8_t *)tx_presence, sizeof(tx_presence));
    etf_map_header(&w, 2);
    etf_binary_str(&w, "op");
    etf_small_int(&w, DC_GW_OP_PRESENCE_UPDATE);
    etf_binary_str(&w, "d");
    etf_map_header(&w, 4);
    etf_binary_str(&w, "since");
    etf_atom(&w, "nil");
    etf_binary_str(&w, "activities");
    etf_activity_list(&w);
    etf_binary_str(&w, "status");
    etf_binary_str(&w, "dnd");
    etf_binary_str(&w, "afk");
    etf_atom(&w, "false");
    if (w.overflow) {
        return false;
    }
    return send_frame((const char *)w.buf, w.pos);
}

void dc_ws_set_identify_store(const DcIdentifyStore *store)
{
    if (store != NULL) {
        identify_store = *store;
    }
}

esp_err_t dc_ws_start(void)
{
    DcWsDecodeSinks sinks;

    if (fatal_latch) {
        ESP_LOGE(DC_WS_TAG, "fatal latch set, restart required");
        return ESP_FAIL;
    }
    if (client_started) {
        ESP_LOGW(DC_WS_TAG, "already started");
        return ESP_OK;
    }
    if (ca_pem == NULL || token_provider == NULL ||
        identify_store.read == NULL || identify_store.write == NULL) {
        ESP_LOGE(DC_WS_TAG, "ca store, token provider or identify store missing");
        return ESP_FAIL;
    }

    dc_identify_init(&identify, &identify_store);
    if (!dc_identify_load(&identify)) {
        ESP_LOGE(DC_WS_TAG, "identify store unreadable, fatal at boot");
        return ESP_FAIL;
    }

    sinks.ackq = &ackq;
    sinks.interaction_handler = interaction_handler;
    sinks.message_handler = message_handler;
    sinks.member_handler = member_handler;
    sinks.modal_for = modal_for;
    sinks.modal_build = modal_build;
    sinks.ephemeral_for = ephemeral_for;
    discord_ws_decode_wire(&sinks);

    dc_gateway_init(&gw, clock_ms(), DC_GW_IDENTIFY_DAILY_LIMIT);
    dc_ackq_init(&ackq);
    dc_gateway_connect(&gw);
    want_stop = false;
    return start_client();
}

void dc_ws_stop(void)
{
    stop_client();
    want_stop = false;
    fatal_latch = false;
}

void dc_ws_gateway_tick(uint64_t now_ms)
{
    DcGwAction a;

    if (want_stop) {
        if (client != NULL) {
            stop_client();
        }
        want_stop = false;
    }
    a = dc_gateway_tick(&gw, now_ms);
    switch (a.event) {
    case DC_GW_EV_SEND_HEARTBEAT:
        send_heartbeat(tx_task, sizeof(tx_task));
        break;
    case DC_GW_EV_RECONNECT:
        if (start_client() == ESP_OK) {
            dc_gateway_connect(&gw);
        } else {
            gw.state = DC_GW_BACKOFF;
            gw.backoff_ms = DC_GW_BACKOFF_BASE_MS;
            gw.next_retry_ms = now_ms + DC_GW_BACKOFF_BASE_MS;
        }
        break;
    default:
        break;
    }
}

DcGateway *dc_ws_gateway(void)
{
    return &gw;
}

DcAckq *dc_ws_ackq(void)
{
    return &ackq;
}

DcIdentify *dc_ws_identify(void)
{
    return &identify;
}
