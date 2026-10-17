#include <stdio.h>
#include <string.h>

#include "discord_gateway.h"

static int failures = 0;
static int checks = 0;
static int quiet = 0;

static void check(int cond, const char *name)
{
    checks++;
    if (!cond) {
        printf("FAIL %s\n", name);
        failures++;
    } else if (!quiet) {
        printf("ok   %s\n", name);
    }
}

static void note(const char *text)
{
    if (!quiet) {
        printf("     %s\n", text);
    }
}

static void set_session(DcGateway *gw, const char *id)
{
    snprintf(gw->session_id, sizeof(gw->session_id), "%s", id);
    snprintf(gw->resume_url, sizeof(gw->resume_url), "wss://resume.discord.gg");
}

static void test_happy_path(void)
{
    DcGateway gw;
    dc_gateway_init(&gw, 0, 1000);
    dc_gateway_connect(&gw);
    check(gw.state == DC_GW_CONNECTING, "connect -> CONNECTING");

    DcGwAction a = dc_gateway_on_connected(&gw, 100);
    check(a.event == DC_GW_EV_CONNECTED && gw.state == DC_GW_HANDSHAKING,
          "connected -> HANDSHAKING");

    a = dc_gateway_on_hello(&gw, 41250, 200);
    check(a.event == DC_GW_EV_SEND_IDENTIFY && gw.state == DC_GW_IDENTIFYING,
          "no session -> IDENTIFY");

    gw.state = DC_GW_READY;
    set_session(&gw, "sess1");
    a = dc_gateway_tick(&gw, 200);
    check(a.event == DC_GW_EV_NONE, "no heartbeat before interval");

    a = dc_gateway_tick(&gw, 200 + 41250);
    check(a.event == DC_GW_EV_SEND_HEARTBEAT, "heartbeat after interval");

    a = dc_gateway_on_heartbeat_ack(&gw, 200 + 41300);
    check(a.event == DC_GW_EV_NONE && gw.heartbeat_acked, "heartbeat acked");
}

static void test_resume_preferred(void)
{
    DcGateway gw;
    dc_gateway_init(&gw, 0, 1000);
    set_session(&gw, "sess1");
    gw.state = DC_GW_READY;

    dc_gateway_on_connected(&gw, 1000);
    DcGwAction a = dc_gateway_on_hello(&gw, 41250, 1100);
    check(a.event == DC_GW_EV_SEND_RESUME && gw.state == DC_GW_RESUMING,
          "existing session -> RESUME not IDENTIFY");
}

static void test_invalid_session_resumable(void)
{
    DcGateway gw;
    dc_gateway_init(&gw, 0, 1000);
    set_session(&gw, "sess1");
    gw.state = DC_GW_READY;

    DcGwAction a = dc_gateway_on_op(&gw, DC_GW_OP_INVALID_SESSION, true, 5000);
    check(a.event == DC_GW_EV_SEND_RESUME && a.can_resume, "resumable -> RESUME");
    check(gw.resume_count == 1, "resume counted");
}

static void test_invalid_session_not_resumable(void)
{
    DcGateway gw;
    dc_gateway_init(&gw, 0, 1000);
    set_session(&gw, "sess1");
    gw.state = DC_GW_READY;

    DcGwAction a = dc_gateway_on_op(&gw, DC_GW_OP_INVALID_SESSION, false, 5000);
    check(a.event == DC_GW_EV_SEND_IDENTIFY && !a.can_resume, "non-resumable -> IDENTIFY");
    check(gw.session_id[0] == '\0', "session cleared before IDENTIFY");
}

static void test_identify_quota_protects_token(void)
{
    DcGateway gw;
    dc_gateway_init(&gw, 0, 5);

    quiet = 1;
    for (int i = 0; i < 5; i++) {
        dc_gateway_connect(&gw);
        dc_gateway_on_connected(&gw, 100);
        DcGwAction a = dc_gateway_on_hello(&gw, 41250, 200);
        check(a.event == DC_GW_EV_SEND_IDENTIFY, "IDENTIFY allowed below quota");
        gw.identify_count++;
    }
    quiet = 0;
    note("5 IDENTIFY attempts, all below quota");

    dc_gateway_connect(&gw);
    dc_gateway_on_connected(&gw, 100);
    DcGwAction a = dc_gateway_on_hello(&gw, 41250, 200);
    check(a.event == DC_GW_EV_QUOTA_EXHAUSTED, "IDENTIFY refused at quota");
    check(gw.state == DC_GW_FATAL, "state FATAL at quota");
    check(!dc_gateway_can_identify(&gw, 200), "can_identify false at quota");
}

static void test_resume_free(void)
{
    DcGateway gw;
    dc_gateway_init(&gw, 0, 2);

    quiet = 1;
    for (int i = 0; i < 50; i++) {
        set_session(&gw, "sess");
        gw.state = DC_GW_HANDSHAKING;
        DcGwAction a = dc_gateway_on_hello(&gw, 41250, 1000);
        check(a.event == DC_GW_EV_SEND_RESUME, "reconnect uses RESUME, not IDENTIFY");
        gw.state = DC_GW_READY;
    }
    quiet = 0;
    note("50 reconnects all used RESUME, none used IDENTIFY");
    check(gw.identify_count == 0, "50 reconnects consumed zero IDENTIFY");
}

static void test_backoff_growth(void)
{
    DcGateway gw;
    dc_gateway_init(&gw, 0, 1000);
    gw.state = DC_GW_READY;
    set_session(&gw, "s");

    quiet = 1;
    uint32_t prev = 0;
    for (int i = 0; i < 10; i++) {
        dc_gateway_on_close(&gw, 4000, 1000);
        check(gw.backoff_ms >= prev, "backoff grows monotonically");
        check(gw.backoff_ms <= DC_GW_BACKOFF_MAX_MS, "backoff capped at 60s");
        prev = gw.backoff_ms;
    }
    quiet = 0;
    note("backoff grew monotonically across 10 closes and capped at 60s");
    check(gw.backoff_ms == DC_GW_BACKOFF_MAX_MS, "backoff reaches cap");
}

static void test_backoff_retry(void)
{
    DcGateway gw;
    dc_gateway_init(&gw, 0, 1000);
    gw.state = DC_GW_READY;

    dc_gateway_on_close(&gw, 4000, 1000);
    check(gw.state == DC_GW_BACKOFF, "close -> BACKOFF");

    DcGwAction a = dc_gateway_tick(&gw, 1000 + gw.backoff_ms - 1);
    check(a.event == DC_GW_EV_NONE, "no reconnect before backoff expires");

    a = dc_gateway_tick(&gw, 1000 + gw.backoff_ms);
    check(a.event == DC_GW_EV_RECONNECT && gw.state == DC_GW_CONNECTING,
          "reconnect fires when backoff expires");
    check(gw.backoff_ms == 0, "backoff reset after firing");
}

static void test_fatal_close_codes(void)
{
    uint32_t fatal_codes[] = {4013, 4014, 4004, 4009, 4010};
    quiet = 1;
    for (size_t i = 0; i < sizeof(fatal_codes) / sizeof(fatal_codes[0]); i++) {
        DcGateway gw;
        dc_gateway_init(&gw, 0, 1000);
        gw.state = DC_GW_READY;

        DcGwAction a = dc_gateway_on_close(&gw, fatal_codes[i], 1000);
        check(a.event == DC_GW_EV_FATAL, "fatal close goes fatal");
        check(gw.state == DC_GW_FATAL, "state FATAL on fatal close");

        dc_gateway_connect(&gw);
        check(gw.state == DC_GW_FATAL, "FATAL blocks reconnect");
    }
    quiet = 0;
    note("close codes 4013/4014/4004/4009/4010 all fatal");
}

/* 4004 means Discord rejected the token. Reconnecting would burn the
 * 1000-per-24h IDENTIFY quota and revoke it permanently, so the guard must
 * hold even after many reconnects. */
static void test_auth_failure_does_not_loop(void)
{
    DcGateway gw;
    dc_gateway_init(&gw, 0, 1000);
    set_session(&gw, "sess");
    gw.state = DC_GW_READY;

    DcGwAction a = dc_gateway_on_close(&gw, DC_GW_CLOSE_AUTH_FAILED, 1000);
    check(a.event == DC_GW_EV_FATAL, "4004 is fatal on first close");
    check(gw.state == DC_GW_FATAL, "state FATAL");
    check(gw.backoff_ms == 0, "no backoff scheduled for a fatal auth failure");
    check(gw.identify_count == 0, "no IDENTIFY consumed by the rejection");

    for (int i = 0; i < 100; i++) {
        dc_gateway_connect(&gw);
        dc_gateway_on_close(&gw, DC_GW_CLOSE_AUTH_FAILED, 1000 + (uint64_t)i);
    }
    check(gw.identify_count == 0, "2000 further closes still consume zero IDENTIFY");
    check(gw.state == DC_GW_FATAL, "stays fatal");
}

static void test_zombie_detection(void)
{
    DcGateway gw;
    dc_gateway_init(&gw, 0, 1000);
    gw.state = DC_GW_READY;
    gw.heartbeat_interval_ms = 41250;

    dc_gateway_tick(&gw, 50000);
    check(gw.heartbeat_pending, "heartbeat sent, awaiting ack");

    DcGwAction a = dc_gateway_tick(&gw, 50000 + 41250 - 1);
    check(a.event == DC_GW_EV_NONE, "waits for ack within the interval");

    a = dc_gateway_tick(&gw, 50000 + 41250);
    check(a.event == DC_GW_EV_RECONNECT, "zombie detected once interval elapses");
    check(gw.state == DC_GW_BACKOFF, "zombie -> BACKOFF");
}

static void test_ack_resets_zombie(void)
{
    DcGateway gw;
    dc_gateway_init(&gw, 0, 1000);
    gw.state = DC_GW_READY;
    gw.heartbeat_interval_ms = 41250;

    dc_gateway_tick(&gw, 50000);
    dc_gateway_on_heartbeat_ack(&gw, 50100);

    DcGwAction a = dc_gateway_tick(&gw, 50000 + 41250);
    check(a.event != DC_GW_EV_RECONNECT, "acked heartbeat does not trip zombie");
}

static void test_server_heartbeat_op(void)
{
    DcGateway gw;
    dc_gateway_init(&gw, 0, 1000);
    gw.state = DC_GW_READY;
    gw.heartbeat_interval_ms = 41250;

    dc_gateway_tick(&gw, 50000);
    check(gw.heartbeat_pending, "heartbeat pending");

    DcGwAction a = dc_gateway_on_op(&gw, DC_GW_OP_HEARTBEAT, false, 51000);
    check(a.event == DC_GW_EV_SEND_HEARTBEAT, "server-requested heartbeat honoured");

    a = dc_gateway_tick(&gw, 51000 + 41250 * 3);
    check(a.event != DC_GW_EV_RECONNECT, "immediate heartbeat clears zombie state");
}

static void test_clock_jump_backward(void)
{
    DcGateway gw;
    dc_gateway_init(&gw, 100000, 1000);
    gw.state = DC_GW_READY;
    gw.heartbeat_interval_ms = 41250;

    dc_gateway_on_close(&gw, 4000, 100000);
    uint64_t next = gw.next_retry_ms;

    DcGwAction a = dc_gateway_tick(&gw, 50000);
    check(a.event == DC_GW_EV_NONE, "clock jump back does not fire reconnect early");

    a = dc_gateway_tick(&gw, next);
    check(a.event == DC_GW_EV_RECONNECT, "reconnect still fires at deadline");
}

static void test_identify_window_reset(void)
{
    DcGateway gw;
    dc_gateway_init(&gw, 0, 3);
    gw.identify_count = 3;
    check(!dc_gateway_can_identify(&gw, 1000), "quota exhausted");

    gw.identify_window_start_ms = 1000;
    gw.identify_count = 0;
    check(dc_gateway_can_identify(&gw, 86400000), "new day window resets quota");
}

int main(void)
{
    test_happy_path();
    test_resume_preferred();
    test_invalid_session_resumable();
    test_invalid_session_not_resumable();
    test_identify_quota_protects_token();
    test_resume_free();
    test_backoff_growth();
    test_backoff_retry();
    test_fatal_close_codes();
    test_auth_failure_does_not_loop();
    test_zombie_detection();
    test_ack_resets_zombie();
    test_server_heartbeat_op();
    test_clock_jump_backward();
    test_identify_window_reset();

    printf("\n%s (%d checks, %d failure(s))\n", failures ? "FAILED" : "PASSED",
           checks, failures);
    return failures ? 1 : 0;
}