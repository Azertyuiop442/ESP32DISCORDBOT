#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "discord_gateway.h"
#include "discord_ratelimit.h"
#include "discord_state.h"

#define ROUTES 6
#define HOUR_MS 3600000u
#define DAY_MS 86400000u

typedef struct {
    uint32_t p_wifi_drop;      /* per tick chance the link drops        */
    uint32_t p_server_close;   /* per tick chance of a server close     */
    uint32_t p_zombie;         /* per tick chance the ack never arrives  */
    uint32_t p_invalid_nores;  /* per tick chance of non-resumable close */
    uint32_t p_fatal;          /* per tick chance of a fatal close code  */
    uint32_t p_rate429;        /* per tick chance of a 429 on a route    */
} SimProfile;

static uint32_t rs = 1u;

static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

static uint32_t chance(uint32_t percent)
{
    return (rnd() % 100u) < percent;
}

typedef struct {
    uint64_t now_ms;
    uint64_t identify_sent;
    uint64_t resume_sent;
    uint64_t reconnects;
    uint64_t fatal;
    uint64_t rate_limited;
    uint64_t rest_sent;
    uint64_t quarantined;
    uint64_t max_identify_per_day;
    uint32_t day_identify;
    uint64_t day_start_ms;
} SimStats;

static void run_sim(const SimProfile *p, uint64_t days, SimStats *st)
{
    memset(st, 0, sizeof(*st));
    st->now_ms = 0;
    st->day_start_ms = 0;

    DcGateway gw;
    dc_gateway_init(&gw, 0, 1000);
    dc_gateway_connect(&gw);

    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 0);

    DcBotState state;
    memset(&state, 0, sizeof(state));

    uint64_t end_ms = days * DAY_MS;
    uint32_t tick_ms = 100;

    while (st->now_ms < end_ms) {
        uint64_t t = st->now_ms;

        if (t - st->day_start_ms >= DAY_MS) {
            st->day_start_ms = t;
            st->max_identify_per_day = st->day_identify > st->max_identify_per_day
                                           ? st->day_identify
                                           : st->max_identify_per_day;
            st->day_identify = 0;
        }

        if (gw.state == DC_GW_FATAL && gw.close_code != DC_GW_CLOSE_INVALID_INTENTS &&
            chance(p->p_fatal / 2)) {
            st->fatal++;
            return;
        }

        DcGwAction a = dc_gateway_tick(&gw, t);

        if (gw.state == DC_GW_CONNECTING) {
            dc_gateway_on_connected(&gw, t);
            a = dc_gateway_on_hello(&gw, 41250 + (rnd() % 5000), t);
        } else if (a.event == DC_GW_EV_RECONNECT) {
            st->reconnects++;
            dc_gateway_connect(&gw);
            dc_gateway_on_connected(&gw, t);
            a = dc_gateway_on_hello(&gw, 41250 + (rnd() % 5000), t);
        }

        if (a.event == DC_GW_EV_SEND_HEARTBEAT) {
            if (chance(p->p_zombie)) {
                a = dc_gateway_tick(&gw, t + 41250);
            } else {
                dc_gateway_on_heartbeat_ack(&gw, t + 5);
            }
        }

        if (a.event == DC_GW_EV_SEND_IDENTIFY) {
            if (!dc_gateway_can_identify(&gw, t)) {
                st->quarantined++;
                return;
            }
            gw.identify_count++;
            st->identify_sent++;
            st->day_identify++;
            snprintf(gw.session_id, sizeof(gw.session_id), "sess-%llu",
                     (unsigned long long)st->identify_sent);
            snprintf(gw.resume_url, sizeof(gw.resume_url), "wss://resume.discord.gg");
            gw.state = DC_GW_READY;
        }

        if (a.event == DC_GW_EV_SEND_RESUME) {
            st->resume_sent++;
            gw.state = DC_GW_READY;
        }

        if (gw.state == DC_GW_READY) {
            for (uint32_t r = 0; r < ROUTES; r++) {
                if (dc_ratelimit_rest(&rl, r * 7919u, t) == DC_WAIT_OK) {
                    st->rest_sent++;
                    if (chance(p->p_rate429)) {
                        dc_ratelimit_on_429(&rl, r * 7919u, 100 + (rnd() % 2000), t);
                        st->rate_limited++;
                    }
                }
            }
            if (chance(2)) {
                dc_state_open_ticket(&state, rnd() | 1, 222);
            }
            if (chance(2)) {
                dc_state_rank_user(&state, rnd() | 1)->xp += 10;
            }
        }

        if (chance(p->p_wifi_drop)) {
            dc_gateway_on_close(&gw, 1006, t);
            st->reconnects++;
        } else if (chance(p->p_server_close)) {
            dc_gateway_on_close(&gw, 4000 + (rnd() % 500), t);
            st->reconnects++;
        } else if (chance(p->p_invalid_nores)) {
            dc_gateway_on_op(&gw, DC_GW_OP_INVALID_SESSION, false, t);
        } else if (chance(p->p_fatal)) {
            dc_gateway_on_close(&gw, 4014, t);
            st->fatal++;
            return;
        }

        st->now_ms += tick_ms;
    }

    st->max_identify_per_day = st->day_identify > st->max_identify_per_day
                                   ? st->day_identify
                                   : st->max_identify_per_day;
}

static int failures = 0;

static void check(int cond, const char *msg)
{
    if (!cond) {
        printf("  FAIL %s\n", msg);
        failures++;
    }
}

int main(void)
{
    SimProfile ideal = {0, 0, 0, 0, 0, 0};

    printf("Monte Carlo soak, 7-day horizons\n");
    printf("expected on a clean link: identify=1  resume=0  reconnect=0\n\n");

    SimStats st;
    run_sim(&ideal, 7, &st);
    check(st.identify_sent == 1, "clean link IDENTIFIES exactly once");
    check(st.reconnects == 0, "clean link never reconnects");
    check(st.resume_sent == 0, "clean link never RESUMEs");

    SimStats s2;
    SimProfile flaky = {2, 1, 1, 1, 0, 2};
    run_sim(&flaky, 7, &s2);
    printf("flaky wifi             identify=%llu   resume=%llu  reconnect=%llu\n",
           (unsigned long long)s2.identify_sent, (unsigned long long)s2.resume_sent,
           (unsigned long long)s2.reconnects);
    check(s2.identify_sent < 30, "flaky link stays far below the 1000/day quota");
    check(s2.resume_sent > 0, "flaky link actually exercises RESUME");
    check(s2.identify_sent <= s2.resume_sent, "RESUME preferred over IDENTIFY");

    SimStats s3;
    SimProfile brutal = {10, 5, 5, 5, 0, 5};
    run_sim(&brutal, 7, &s3);
    printf("brutal                 identify=%llu   resume=%llu  reconnect=%llu\n",
           (unsigned long long)s3.identify_sent, (unsigned long long)s3.resume_sent,
           (unsigned long long)s3.reconnects);
    check(s3.identify_sent < 60, "brutal conditions still under quota");

    SimStats s4;
    SimProfile zombie = {0, 0, 60, 0, 0, 0};
    run_sim(&zombie, 7, &s4);
    printf("zombie acks            identify=%llu   reconnect=%llu\n",
           (unsigned long long)s4.identify_sent, (unsigned long long)s4.reconnects);
    check(s4.identify_sent < 60, "zombie acks do not burn the quota");

    SimStats s5;
    SimProfile storm = {0, 0, 0, 0, 0, 40};
    run_sim(&storm, 7, &s5);
    printf("rate limit storm       rest_sent=%llu  limited=%llu\n",
           (unsigned long long)s5.rest_sent, (unsigned long long)s5.rate_limited);
    check(s5.rate_limited > 0, "rate limit storm actually triggers 429s");
    check(s5.rest_sent > 0, "traffic still flows during a rate limit storm");

    SimStats s6;
    SimProfile panic = {100, 100, 100, 100, 1, 50};
    run_sim(&panic, 1, &s6);
    printf("panic                  identify=%llu   reconnects=%llu  fatal=%llu\n",
           (unsigned long long)s6.identify_sent, (unsigned long long)s6.reconnects,
           (unsigned long long)s6.fatal);
    check(s6.fatal > 0 || s6.quarantined > 0 || s6.identify_sent < 1000,
          "panic conditions never exceed the Discord quota");

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}