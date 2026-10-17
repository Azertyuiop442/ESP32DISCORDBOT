#ifndef DISCORD_GATEWAY_H
#define DISCORD_GATEWAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DC_GW_HEARTBEAT_MIN_MS 41250u
#define DC_GW_HEARTBEAT_JITTER_MS 5000u
#define DC_GW_BACKOFF_BASE_MS 1000u
#define DC_GW_BACKOFF_MAX_MS 60000u
#define DC_GW_IDENTIFY_DAILY_LIMIT 1000u
#define DC_GW_SESSION_TTL_MS 300000u

typedef enum {
    DC_GW_IDLE = 0,
    DC_GW_CONNECTING,
    DC_GW_HANDSHAKING,
    DC_GW_IDENTIFYING,
    DC_GW_RESUMING,
    DC_GW_READY,
    DC_GW_BACKOFF,
    DC_GW_FATAL
} DcGwState;

typedef enum {
    DC_GW_CLOSE_NONE = 0,
    DC_GW_CLOSE_NORMAL = 1000,
    DC_GW_CLOSE_GOING_AWAY = 1001,
    DC_GW_CLOSE_PROTOCOL_ERROR = 1002,
    DC_GW_CLOSE_AUTH_FAILED = 4004,
    DC_GW_CLOSE_INVALID_INTENTS = 4013,
    DC_GW_CLOSE_DISALLOWED_INTENTS = 4014
} DcGwCloseCode;

typedef enum {
    DC_GW_OP_DISPATCH = 0,
    DC_GW_OP_HEARTBEAT = 1,
    DC_GW_OP_IDENTIFY = 2,
    DC_GW_OP_PRESENCE_UPDATE = 3,
    DC_GW_OP_VOICE_STATE = 4,
    DC_GW_OP_RESUME = 6,
    DC_GW_OP_RECONNECT = 7,
    DC_GW_OP_REQUEST_MEMBERS = 8,
    DC_GW_OP_INVALID_SESSION = 9,
    DC_GW_OP_HELLO = 10,
    DC_GW_OP_HEARTBEAT_ACK = 11
} DcGwOpcode;

typedef enum {
    DC_GW_EV_NONE = 0,
    DC_GW_EV_CONNECTED,
    DC_GW_EV_SEND_HEARTBEAT,
    DC_GW_EV_SEND_IDENTIFY,
    DC_GW_EV_SEND_RESUME,
    DC_GW_EV_RECONNECT,
    DC_GW_EV_READY,
    DC_GW_EV_DISPATCH,
    DC_GW_EV_RESUMED,
    DC_GW_EV_INVALID_SESSION,
    DC_GW_EV_FATAL,
    DC_GW_EV_QUOTA_EXHAUSTED
} DcGwEvent;

typedef struct {
    DcGwState state;
    DcGwCloseCode close_code;

    uint32_t heartbeat_interval_ms;
    uint64_t last_heartbeat_ms;
    uint64_t heartbeat_sent_ms;
    bool     heartbeat_acked;
    bool     heartbeat_pending;

    uint32_t sequence;
    bool     sequence_valid;

    uint32_t reconnect_attempts;
    uint64_t next_retry_ms;
    uint32_t backoff_ms;

    char     session_id[64];
    char     resume_url[128];
    uint64_t session_started_ms;

    uint32_t identify_count;
    uint32_t identify_limit;
    uint64_t identify_window_start_ms;

    uint32_t resume_count;
    uint32_t dispatch_count;
    uint32_t fatal_count;
} DcGateway;

typedef struct {
    DcGwEvent event;
    uint32_t  heartbeat_interval_ms;
    bool     can_resume;
    bool     resumed;
    DcGwCloseCode close_code;
} DcGwAction;

void dc_gateway_init(DcGateway *gw, uint64_t now_ms, uint32_t identify_limit);
void dc_gateway_connect(DcGateway *gw);
DcGwAction dc_gateway_on_connected(DcGateway *gw, uint64_t now_ms);
DcGwAction dc_gateway_on_hello(DcGateway *gw, uint32_t interval_ms, uint64_t now_ms);
DcGwAction dc_gateway_on_op(DcGateway *gw, uint32_t opcode, bool resumable,
                            uint64_t now_ms);
DcGwAction dc_gateway_on_heartbeat_ack(DcGateway *gw, uint64_t now_ms);
DcGwAction dc_gateway_on_close(DcGateway *gw, uint32_t close_code, uint64_t now_ms);
DcGwAction dc_gateway_tick(DcGateway *gw, uint64_t now_ms);
bool dc_gateway_can_identify(const DcGateway *gw, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif