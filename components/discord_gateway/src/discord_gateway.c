#include "discord_gateway.h"

#include <string.h>

static DcGwAction action(DcGwEvent ev)
{
    DcGwAction a;
    memset(&a, 0, sizeof(a));
    a.event = ev;
    return a;
}

static void clear_session(DcGateway *gw)
{
    gw->session_id[0] = '\0';
    gw->resume_url[0] = '\0';
    gw->session_started_ms = 0;
    gw->sequence_valid = false;
}

static uint32_t backoff_for(uint32_t attempts)
{
    if (attempts == 0) {
        return 0;
    }
    uint32_t shift = attempts - 1;
    if (shift > 16) {
        shift = 16;
    }
    uint32_t delay = DC_GW_BACKOFF_BASE_MS << shift;
    return delay > DC_GW_BACKOFF_MAX_MS ? DC_GW_BACKOFF_MAX_MS : delay;
}

static uint32_t identify_used(const DcGateway *gw, uint64_t now_ms)
{
    if (now_ms < gw->identify_window_start_ms) {
        return 0;
    }
    return gw->identify_count;
}

void dc_gateway_init(DcGateway *gw, uint64_t now_ms, uint32_t identify_limit)
{
    memset(gw, 0, sizeof(*gw));
    gw->state = DC_GW_IDLE;
    gw->close_code = DC_GW_CLOSE_NONE;
    gw->heartbeat_interval_ms = DC_GW_HEARTBEAT_MIN_MS;
    gw->identify_limit = identify_limit ? identify_limit : DC_GW_IDENTIFY_DAILY_LIMIT;
    gw->identify_window_start_ms = now_ms;
    gw->identify_count = 0;
    clear_session(gw);
}

void dc_gateway_connect(DcGateway *gw)
{
    if (gw->state == DC_GW_FATAL) {
        return;
    }
    gw->state = DC_GW_CONNECTING;
    gw->heartbeat_acked = true;
    gw->heartbeat_pending = false;
}

bool dc_gateway_can_identify(const DcGateway *gw, uint64_t now_ms)
{
    return identify_used(gw, now_ms) < gw->identify_limit;
}

DcGwAction dc_gateway_on_connected(DcGateway *gw, uint64_t now_ms)
{
    (void)now_ms;
    gw->state = DC_GW_HANDSHAKING;
    return action(DC_GW_EV_CONNECTED);
}

DcGwAction dc_gateway_on_hello(DcGateway *gw, uint32_t interval_ms, uint64_t now_ms)
{
    gw->heartbeat_interval_ms = interval_ms ? interval_ms : DC_GW_HEARTBEAT_MIN_MS;
    gw->last_heartbeat_ms = now_ms;
    gw->heartbeat_acked = true;
    gw->heartbeat_pending = false;

    if (gw->state == DC_GW_HANDSHAKING) {
        bool can_resume = gw->session_id[0] != '\0';
        if (can_resume) {
            gw->state = DC_GW_RESUMING;
            return action(DC_GW_EV_SEND_RESUME);
        }
        if (!dc_gateway_can_identify(gw, now_ms)) {
            gw->state = DC_GW_FATAL;
            gw->close_code = DC_GW_CLOSE_NONE;
            return action(DC_GW_EV_QUOTA_EXHAUSTED);
        }
        gw->state = DC_GW_IDENTIFYING;
        return action(DC_GW_EV_SEND_IDENTIFY);
    }

    return action(DC_GW_EV_SEND_HEARTBEAT);
}

DcGwAction dc_gateway_on_op(DcGateway *gw, uint32_t opcode, bool resumable, uint64_t now_ms)
{
    switch (opcode) {
    case DC_GW_OP_HEARTBEAT:
        gw->last_heartbeat_ms = now_ms;
        gw->heartbeat_pending = true;
        gw->heartbeat_acked = true;
        return action(DC_GW_EV_SEND_HEARTBEAT);

    case DC_GW_OP_HELLO:
        return dc_gateway_on_hello(gw, gw->heartbeat_interval_ms, now_ms);

    case DC_GW_OP_INVALID_SESSION: {
        bool can_resume = resumable && gw->session_id[0] != '\0';
        if (can_resume) {
            gw->resume_count++;
            gw->state = DC_GW_RESUMING;
            DcGwAction a = action(DC_GW_EV_SEND_RESUME);
            a.can_resume = true;
            return a;
        }
        clear_session(gw);
        gw->resume_count = 0;
        if (!dc_gateway_can_identify(gw, now_ms)) {
            gw->state = DC_GW_FATAL;
            return action(DC_GW_EV_QUOTA_EXHAUSTED);
        }
        gw->state = DC_GW_IDENTIFYING;
        DcGwAction a = action(DC_GW_EV_SEND_IDENTIFY);
        a.can_resume = false;
        return a;
    }

    case DC_GW_OP_DISPATCH:
        gw->dispatch_count++;
        DcGwAction a = action(DC_GW_EV_DISPATCH);
        a.resumed = (gw->state == DC_GW_READY);
        return a;

    case DC_GW_OP_RECONNECT:
        gw->reconnect_attempts++;
        gw->backoff_ms = backoff_for(gw->reconnect_attempts);
        gw->next_retry_ms = now_ms + gw->backoff_ms;
        gw->state = DC_GW_BACKOFF;
        return action(DC_GW_EV_RECONNECT);

    default:
        return action(DC_GW_EV_NONE);
    }
}

DcGwAction dc_gateway_on_heartbeat_ack(DcGateway *gw, uint64_t now_ms)
{
    gw->heartbeat_acked = true;
    gw->last_heartbeat_ms = now_ms;
    gw->heartbeat_pending = false;
    return action(DC_GW_EV_NONE);
}

static bool is_fatal_close(uint32_t code)
{
    switch (code) {
    /* 4004 is Discord rejecting the bot token. Reconnecting would issue a new
     * IDENTIFY on every attempt, and the quota is 1000 per 24 h before the
     * token is revoked for good. Failing fast is the only safe response. */
    case DC_GW_CLOSE_AUTH_FAILED:
    case DC_GW_CLOSE_INVALID_INTENTS:
    case DC_GW_CLOSE_DISALLOWED_INTENTS:
        return true;
    default:
        return false;
    }
}

DcGwAction dc_gateway_on_close(DcGateway *gw, uint32_t close_code, uint64_t now_ms)
{
    gw->close_code = (DcGwCloseCode)close_code;

    if (is_fatal_close(close_code)) {
        gw->state = DC_GW_FATAL;
        gw->fatal_count++;
        return action(DC_GW_EV_FATAL);
    }

    if (close_code == DC_GW_CLOSE_NORMAL || close_code == DC_GW_CLOSE_GOING_AWAY) {
        gw->reconnect_attempts++;
        gw->backoff_ms = backoff_for(gw->reconnect_attempts);
        gw->next_retry_ms = now_ms + gw->backoff_ms;
        gw->state = DC_GW_BACKOFF;
        return action(DC_GW_EV_RECONNECT);
    }

    if (close_code == 4009 || close_code == 4010) {
        gw->state = DC_GW_FATAL;
        gw->fatal_count++;
        return action(DC_GW_EV_FATAL);
    }

    gw->reconnect_attempts++;
    gw->backoff_ms = backoff_for(gw->reconnect_attempts);
    gw->next_retry_ms = now_ms + gw->backoff_ms;
    gw->state = DC_GW_BACKOFF;
    return action(DC_GW_EV_RECONNECT);
}

DcGwAction dc_gateway_tick(DcGateway *gw, uint64_t now_ms)
{
    if (gw->state == DC_GW_BACKOFF) {
        if (now_ms >= gw->next_retry_ms) {
            gw->reconnect_attempts = 0;
            gw->backoff_ms = 0;
            gw->state = DC_GW_CONNECTING;
            return action(DC_GW_EV_RECONNECT);
        }
        return action(DC_GW_EV_NONE);
    }

    if (gw->state != DC_GW_READY) {
        return action(DC_GW_EV_NONE);
    }

    if (gw->heartbeat_pending && !gw->heartbeat_acked) {
        uint64_t deadline = gw->heartbeat_sent_ms + gw->heartbeat_interval_ms;
        if (now_ms >= deadline) {
            gw->reconnect_attempts++;
            gw->backoff_ms = backoff_for(gw->reconnect_attempts);
            gw->next_retry_ms = now_ms + gw->backoff_ms;
            gw->state = DC_GW_BACKOFF;
            return action(DC_GW_EV_RECONNECT);
        }
        return action(DC_GW_EV_NONE);
    }

    if (gw->last_heartbeat_ms + gw->heartbeat_interval_ms <= now_ms) {
        gw->heartbeat_pending = true;
        gw->heartbeat_acked = false;
        gw->heartbeat_sent_ms = now_ms;
        gw->last_heartbeat_ms = now_ms;
        return action(DC_GW_EV_SEND_HEARTBEAT);
    }

    return action(DC_GW_EV_NONE);
}