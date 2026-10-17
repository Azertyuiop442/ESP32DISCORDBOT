#include <stdio.h>
#include <string.h>

#include "discord_ratelimit.h"

static int failures = 0;

static void check(int cond, const char *name)
{
    if (!cond) {
        printf("FAIL %s\n", name);
        failures++;
    } else {
        printf("ok   %s\n", name);
    }
}

#define R_CHANNELS 0x11111111u
#define R_USERS 0x22222222u
#define W10 (10ULL * 60ULL * 1000ULL)
#define WINDOW_AT (2ULL * W10)
#define PACER_AT (WINDOW_AT - 300ULL * 1000ULL)
#define STRIDE_MS 50ULL
#define SPREAD_FLOOD 12000u
#define SPACED_PER_WINDOW (DC_RATELIMIT_INVALID_MAX / 4u)
#define SPACED_WINDOWS 6u
#define SATURATED_AT (2u * W10)

static void test_rest_burst(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 1000);

    int allowed = 0;
    for (int i = 0; i < 60; i++) {
        if (dc_ratelimit_rest(&rl, R_CHANNELS, 1000) == DC_WAIT_OK) {
            allowed++;
        }
    }
    printf("     allowed %d of 60 in one second\n", allowed);
    check(allowed == (int)DC_GLOBAL_LIMIT_PER_SEC, "exactly 50 requests per second");
    check(dc_ratelimit_rest(&rl, R_CHANNELS, 1000) == DC_WAIT_THROTTLED,
          "51st request throttled");
}

static void test_second_rollover(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 1000);

    for (int i = 0; i < 50; i++) {
        dc_ratelimit_rest(&rl, R_CHANNELS, 1000);
    }
    check(dc_ratelimit_rest(&rl, R_CHANNELS, 1500) == DC_WAIT_THROTTLED,
          "still throttled mid-second");
    check(dc_ratelimit_rest(&rl, R_CHANNELS, 2000) == DC_WAIT_OK,
          "bucket refills on second boundary");
}

static void test_gateway_limit(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 0);

    int allowed = 0;
    for (int i = 0; i < 130; i++) {
        if (dc_ratelimit_gateway(&rl, 0) == DC_WAIT_OK) {
            allowed++;
        }
    }
    printf("     gateway allowed %d of 130 per minute\n", allowed);
    check(allowed == (int)DC_GATEWAY_LIMIT_PER_MIN, "exactly 120 gateway events per minute");
    check(dc_ratelimit_gateway(&rl, 1000) == DC_WAIT_THROTTLED, "121st gateway event blocked");
    check(dc_ratelimit_gateway(&rl, 60000) == DC_WAIT_OK, "gateway window rolls over at 60s");
}

static void test_route_header(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 1000);

    DcRateLimitInfo info = {
        .remaining = 0,
        .reset_after_ms = 2500,
        .global = false,
        .shared = false,
        .present = true,
    };
    dc_ratelimit_on_headers(&rl, R_CHANNELS, &info, 1000);

    check(dc_ratelimit_rest(&rl, R_CHANNELS, 1000) == DC_WAIT_THROTTLED,
          "exhausted route blocks");
    check(dc_ratelimit_rest(&rl, R_USERS, 1000) == DC_WAIT_OK,
          "other route unaffected");
    check(dc_ratelimit_wait_ms(&rl, R_CHANNELS, 1000) == 2500, "reports header reset delay");
    check(dc_ratelimit_rest(&rl, R_CHANNELS, 2000) == DC_WAIT_THROTTLED,
          "still blocked before reset");
    check(dc_ratelimit_rest(&rl, R_CHANNELS, 3500) == DC_WAIT_OK,
          "unblocked after reset elapsed");
}

static void test_unknown_remaining(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 1000);

    DcRateLimitInfo info = {
        .remaining = UINT32_MAX,
        .reset_after_ms = 0,
        .global = false,
        .shared = false,
        .present = true,
    };
    dc_ratelimit_on_headers(&rl, R_CHANNELS, &info, 1000);

    int allowed = 0;
    for (int i = 0; i < 60; i++) {
        if (dc_ratelimit_rest(&rl, R_CHANNELS, 1000) == DC_WAIT_OK) {
            allowed++;
        }
    }
    check(allowed == (int)DC_GLOBAL_LIMIT_PER_SEC,
          "unknown remaining falls back to global limit only");
}

static void test_global_block(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 1000);

    DcRateLimitInfo info = {
        .remaining = 0,
        .reset_after_ms = 5000,
        .global = true,
        .shared = false,
        .present = true,
    };
    dc_ratelimit_on_headers(&rl, R_CHANNELS, &info, 1000);

    check(dc_ratelimit_global_blocked(&rl, 1000), "global block active");
    check(dc_ratelimit_rest(&rl, R_USERS, 1000) == DC_WAIT_THROTTLED,
          "global block hits every route");
    check(dc_ratelimit_wait_ms(&rl, R_USERS, 1000) == 5000, "reports global wait");
    check(dc_ratelimit_rest(&rl, R_USERS, 6000) == DC_WAIT_OK, "global block expires");
    check(!dc_ratelimit_global_blocked(&rl, 6000), "block cleared after expiry");
}

static void test_429(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 1000);

    dc_ratelimit_on_429(&rl, R_CHANNELS, 1200, 1000);

    check(dc_ratelimit_rest(&rl, R_CHANNELS, 1000) == DC_WAIT_THROTTLED, "429 blocks route");
    check(dc_ratelimit_wait_ms(&rl, R_CHANNELS, 1000) == 1200, "429 retry-after honoured");
    check(dc_ratelimit_rest(&rl, R_CHANNELS, 2200) == DC_WAIT_OK, "retry allowed after delay");
}

static void test_wait_is_max(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 1000);

    DcRateLimitInfo route = {
        .remaining = 0, .reset_after_ms = 3000, .global = false, .shared = false, .present = true,
    };
    DcRateLimitInfo glob = {
        .remaining = 0, .reset_after_ms = 9000, .global = true, .shared = false, .present = true,
    };
    dc_ratelimit_on_headers(&rl, R_CHANNELS, &route, 1000);
    dc_ratelimit_on_headers(&rl, R_USERS, &glob, 1000);

    uint32_t w = dc_ratelimit_wait_ms(&rl, R_CHANNELS, 1000);
    check(w == 9000, "global wait dominates route wait");
}

static void test_independent_counters(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 0);

    for (int i = 0; i < 120; i++) {
        dc_ratelimit_gateway(&rl, 0);
    }
    check(dc_ratelimit_gateway(&rl, 0) == DC_WAIT_THROTTLED, "gateway exhausted");
    check(dc_ratelimit_rest(&rl, R_CHANNELS, 0) == DC_WAIT_OK,
          "gateway exhaustion does not block REST");
}

static void test_interaction_ignores_global_cap(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 0);

    for (int i = 0; i < 60; i++) {
        dc_ratelimit_rest(&rl, R_CHANNELS, 0);
    }
    check(dc_ratelimit_rest(&rl, R_CHANNELS, 0) == DC_WAIT_THROTTLED, "REST is capped");

    uint32_t global_before = rl.global_until_ms;
    uint32_t global_flag_before = rl.global_blocked ? 1u : 0u;
    uint32_t window_before = rl.second_window_start_ms;
    uint32_t budget_before = rl.second_count;

    int allowed = 0;
    for (int i = 0; i < 60; i++) {
        if (dc_ratelimit_interaction(&rl, R_CHANNELS, 0) == DC_WAIT_OK) allowed++;
    }
    check(allowed == 60, "interaction path ignores the exhausted global cap");
    check(rl.second_count == budget_before,
          "interaction path did not consume the REST budget");
    check(rl.second_window_start_ms == window_before,
          "interaction path did not roll the REST second window");
    check((rl.global_blocked ? 1u : 0u) == global_flag_before &&
              rl.global_until_ms == global_before,
          "interaction path did not touch the global block state");

    int rest_after = 0;
    for (int i = 0; i < 60; i++) {
        if (dc_ratelimit_rest(&rl, R_CHANNELS, 0) == DC_WAIT_OK) rest_after++;
    }
    check(rest_after == 0, "60 interactions spent none of the REST budget");
}

static void test_interaction_respects_route(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 0);

    DcRateLimitInfo info = { 0, 3000, false, false, true };
    dc_ratelimit_on_headers(&rl, R_CHANNELS, &info, 0);

    check(dc_ratelimit_interaction(&rl, R_CHANNELS, 0) == DC_WAIT_THROTTLED,
          "exemption from the global cap is not exemption from the route bucket");
    check(dc_ratelimit_interaction(&rl, R_USERS, 0) == DC_WAIT_OK,
          "interaction throttled route does not block another route");
    check(dc_ratelimit_interaction(&rl, R_CHANNELS, 3000) == DC_WAIT_OK,
          "interaction route unblocks when the bucket resets");
    check(rl.second_count == 0, "route-throttled interactions cost no REST budget");
}

static void test_interaction_skips_global_429(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 0);

    DcRateLimitInfo info = { 0, 4000, true, false, true };
    dc_ratelimit_on_headers(&rl, R_USERS, &info, 0);
    check(dc_ratelimit_global_blocked(&rl, 0), "a global 429 is armed");
    check(dc_ratelimit_rest(&rl, R_CHANNELS, 0) == DC_WAIT_THROTTLED, "REST answers the 429");
    check(dc_ratelimit_interaction(&rl, R_CHANNELS, 0) == DC_WAIT_OK,
          "interaction ACKs are not held behind a global 429");
    check(dc_ratelimit_wait_ms(&rl, R_CHANNELS, 0) == 4000, "the 429 still gates REST");
}

static void test_interaction_drains_route_budget(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 0);

    DcRateLimitInfo info = { 3, 3000, false, false, true };
    dc_ratelimit_on_headers(&rl, R_CHANNELS, &info, 0);

    int allowed = 0;
    for (int i = 0; i < 10; i++) {
        if (dc_ratelimit_interaction(&rl, R_CHANNELS, 0) == DC_WAIT_OK) allowed++;
    }
    check(allowed == 3, "interaction spends the advertised route remaining");
    check(dc_ratelimit_interaction(&rl, R_CHANNELS, 0) == DC_WAIT_THROTTLED,
          "interaction stops once the route budget is gone");
}

static void test_invalid_request_trips(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 0);

    check(!dc_ratelimit_invalid_tripped(&rl, 0), "breaker starts closed");
    check(!dc_ratelimit_note_invalid(&rl, 0), "the first invalid is far from the threshold");

    DcRateLimiter near;
    dc_ratelimit_init(&near, 0);
    for (uint32_t i = 0; i + 1u < DC_RATELIMIT_INVALID_MAX; i++) {
        dc_ratelimit_note_invalid(&near, 0);
    }
    check(!dc_ratelimit_invalid_tripped(&near, 0),
          "one short of the threshold leaves the breaker closed");
    check(dc_ratelimit_note_invalid(&near, 0),
          "the function returns true on the call that reaches the threshold");

    DcRateLimiter rl2;
    dc_ratelimit_init(&rl2, 0);
    for (uint32_t i = 0; i < DC_RATELIMIT_INVALID_MAX; i++) {
        dc_ratelimit_note_invalid(&rl2, 0);
    }
    check(dc_ratelimit_invalid_tripped(&rl2, 0), "10 000 invalid requests trips the breaker");

    check(!dc_ratelimit_invalid_tripped(&rl2, 10ULL * 60ULL * 1000ULL),
          "breaker clears after the 10 minute window");
}

static void test_invalid_window_rolls_the_count(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, WINDOW_AT);

    for (uint32_t i = 0; i < DC_RATELIMIT_INVALID_MAX; i++) {
        dc_ratelimit_note_invalid(&rl, WINDOW_AT);
    }
    check(dc_ratelimit_invalid_tripped(&rl, WINDOW_AT), "threshold reached in the first window");
    check(dc_ratelimit_invalid_tripped(&rl, WINDOW_AT + W10 - 1u),
          "breaker stays shut one millisecond before the window ends");
    check(!dc_ratelimit_note_invalid(&rl, WINDOW_AT + W10),
          "the window rollover zeroes the count, so one new invalid is back under");
    check(!dc_ratelimit_invalid_tripped(&rl, WINDOW_AT + W10),
          "cleared window leaves the breaker open");
    check(dc_ratelimit_note_invalid(&rl, WINDOW_AT + W10) == false,
          "note_invalid on a rolled window starts from one, not from the old count");

    DcRateLimiter refill;
    dc_ratelimit_init(&refill, WINDOW_AT);
    for (uint32_t i = 0; i + 1u < DC_RATELIMIT_INVALID_MAX; i++) {
        dc_ratelimit_note_invalid(&refill, WINDOW_AT);
    }
    check(!dc_ratelimit_invalid_tripped(&refill, WINDOW_AT), "one short is not a trip");
    for (uint32_t i = 0; i + 1u < DC_RATELIMIT_INVALID_MAX; i++) {
        dc_ratelimit_note_invalid(&refill, WINDOW_AT + W10);
    }
    check(!dc_ratelimit_invalid_tripped(&refill, WINDOW_AT + W10),
          "the 9 999th of the next window is not a trip either");
    check(dc_ratelimit_note_invalid(&refill, WINDOW_AT + W10),
          "the 10 000th of the next window trips on its own");
}

static void test_invalid_window_spans_multiple_rollovers(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 0);

    for (uint32_t i = 0; i < DC_RATELIMIT_INVALID_MAX; i++) {
        dc_ratelimit_note_invalid(&rl, 2ULL * W10 - 1u);
    }
    check(dc_ratelimit_invalid_tripped(&rl, 2ULL * W10 - 1u),
          "a window inside an unrelated window count is still a window");
    check(!dc_ratelimit_note_invalid(&rl, 3ULL * W10),
          "one boundary later is already a different fixed window");
}

/* The shape that separates a fixed window from an anchored one. One pacer at
 * PACER_AT pins an anchored window; the flood then runs at STRIDE_MS from
 * WINDOW_AT, inside a single fixed window, and the pacer sits 300 s before it,
 * so an anchored window started at the pacer expires 600 s later, half way
 * through the flood. The anchored count is therefore split in two and never
 * reaches the threshold, while the fixed window counts the whole flood and
 * trips. No gap anywhere is as long as the window, so nothing else resets it. */
static void test_invalid_spread_flood_trips(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 0);

    dc_ratelimit_note_invalid(&rl, PACER_AT);

    uint32_t tripped_at = 0;
    for (uint32_t i = 0; i < SPREAD_FLOOD; i++) {
        if (dc_ratelimit_note_invalid(&rl, WINDOW_AT + (uint64_t)i * STRIDE_MS)) {
            tripped_at = i + 1u;
            break;
        }
    }

    printf("     %u invalids at %llu ms = %.1f/s, pacer %llu ms earlier\n",
           (unsigned)SPREAD_FLOOD, (unsigned long long)STRIDE_MS, 1000.0 / (double)STRIDE_MS,
           (unsigned long long)(WINDOW_AT - PACER_AT));
    check(tripped_at == DC_RATELIMIT_INVALID_MAX,
          "a flood spread over the window trips at the threshold, not at the flood's end");
}

static void test_invalid_spread_flood_is_one_window(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 0);

    uint64_t first = WINDOW_AT;
    uint64_t last = WINDOW_AT + (uint64_t)(SPACED_PER_WINDOW - 1u) * STRIDE_MS;

    check(first / W10 == last / W10,
          "the paced flood really does sit inside one fixed window");
    check(WINDOW_AT - PACER_AT < W10,
          "the pacer is inside the window too, so no silence resets an anchored window");
    check(last - first < W10,
          "the paced flood is shorter than a window, so it cannot span two by accident");

    for (uint32_t i = 0; i < SPACED_PER_WINDOW; i++) {
        dc_ratelimit_note_invalid(&rl, first + (uint64_t)i * STRIDE_MS);
    }
    check(dc_ratelimit_invalid_count(&rl, last) == SPACED_PER_WINDOW,
          "the fixed window holds every event of the flood");
    check(!dc_ratelimit_invalid_tripped(&rl, last),
          "a quarter of the flood leaves the breaker closed");
    check(!dc_ratelimit_invalid_tripped(&rl, last + 1u),
          "one millisecond later is still the same fixed window");
    check(!dc_ratelimit_invalid_tripped(&rl, first + W10),
          "the next fixed window starts from zero, not from the paced flood");
    check(dc_ratelimit_invalid_count(&rl, first + W10) == 0,
          "the count reads zero in a window nothing has been counted in yet");
}

static void test_invalid_counter_saturates(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 0);

    check(dc_ratelimit_invalid_count(&rl, 0) == 0, "no invalids counted yet");
    for (uint32_t i = 0; i < DC_RATELIMIT_INVALID_MAX; i++) {
        dc_ratelimit_note_invalid(&rl, 0);
    }
    check(dc_ratelimit_invalid_count(&rl, 0) == DC_RATELIMIT_INVALID_MAX,
          "exactly the threshold invalids counted");

    for (uint32_t i = 0; i < DC_RATELIMIT_INVALID_MAX; i++) {
        dc_ratelimit_note_invalid(&rl, 0);
    }
    check(dc_ratelimit_invalid_count(&rl, 0) == DC_RATELIMIT_INVALID_MAX,
          "20 000 invalids in one window leave the count at the threshold");
    check(dc_ratelimit_note_invalid(&rl, 0),
          "the counter saturates at the threshold instead of wrapping to 0");
check(dc_ratelimit_invalid_tripped(&rl, 0),
          "one window of 20 000 invalids never clears the breaker");
}

/* One flood of SPACED_PER_WINDOW spread invalids per fixed window, each window
 * driven on its own limiter so the assertion reads that window and nothing
 * else. Saturation is therefore observed across a window rollover, not
 * assumed from a single timestamp. */
static void test_invalid_spread_saturation_across_windows(void)
{
    for (uint32_t w = 0; w < SPACED_WINDOWS; w++) {
        DcRateLimiter rl;
        uint64_t base = (uint64_t)w * W10;
        dc_ratelimit_init(&rl, base);

        for (uint32_t i = 0; i < SPACED_PER_WINDOW; i++) {
            dc_ratelimit_note_invalid(&rl, base + (uint64_t)i * STRIDE_MS);
        }
        uint64_t last = base + (uint64_t)(SPACED_PER_WINDOW - 1u) * STRIDE_MS;
        uint64_t window_end = base + W10 - 1u;
        uint64_t next = base + W10;
        uint32_t after = SPACED_PER_WINDOW + DC_RATELIMIT_INVALID_MAX;

        for (uint32_t i = 0; i < after; i++) {
            dc_ratelimit_note_invalid(&rl, last);
        }
        check(dc_ratelimit_invalid_count(&rl, last) == DC_RATELIMIT_INVALID_MAX,
              "a flood plus 25 000 more still pins the count at the threshold");
        check(dc_ratelimit_invalid_tripped(&rl, last),
              "and the pinned count still trips the breaker");
        check(dc_ratelimit_invalid_tripped(&rl, window_end),
              "the pinned window is still tripped at its own last millisecond");
        check(!dc_ratelimit_invalid_tripped(&rl, next),
              "the first millisecond of the next window is already clear");
        check(dc_ratelimit_invalid_count(&rl, next) == 0,
              "and reads a count of zero");
        check(dc_ratelimit_note_invalid(&rl, next) == false,
              "the first invalid of the window after a pinned one is back under");
        check(dc_ratelimit_invalid_count(&rl, next) == 1,
              "and that window counts from one, not from the pinned total");
    }
}

static void test_invalid_clock_restarts_at_zero(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 0);

    for (uint32_t i = 0; i < DC_RATELIMIT_INVALID_MAX; i++) {
        dc_ratelimit_note_invalid(&rl, 0);
    }
    check(dc_ratelimit_invalid_tripped(&rl, 0), "a trip survives the clock's own epoch");

    uint64_t same_bucket = W10 - 1u;
    check(dc_ratelimit_invalid_tripped(&rl, same_bucket),
          "a trip is still live one millisecond before its window ends");
    check(dc_ratelimit_invalid_count(&rl, same_bucket) == DC_RATELIMIT_INVALID_MAX,
          "and the count reads the threshold, not zero");

    uint64_t after = SPACED_WINDOWS * W10 + 1u;
    check(!dc_ratelimit_invalid_tripped(&rl, after),
          "a window with nothing counted in it reads clear");
    check(dc_ratelimit_invalid_count(&rl, after) == 0,
          "and reads zero, without clearing the stored count");
    check(dc_ratelimit_invalid_tripped(&rl, 0),
          "the stored trip survives a read from another window");
}

static void test_invalid_independent_of_rest(void)
{
    DcRateLimiter rl;
    dc_ratelimit_init(&rl, 0);

    for (uint32_t i = 0; i < DC_RATELIMIT_INVALID_MAX; i++) {
        dc_ratelimit_note_invalid(&rl, 0);
    }
    check(dc_ratelimit_rest(&rl, R_CHANNELS, 0) == DC_WAIT_OK,
          "the invalid breaker does not throttle REST by itself");
    check(dc_ratelimit_interaction(&rl, R_CHANNELS, 0) == DC_WAIT_OK,
          "the invalid breaker does not throttle ACKs by itself");
    check(rl.second_count == 1, "one REST call, one unit of the REST budget");
}

int main(void)
{
    test_rest_burst();
    test_second_rollover();
    test_gateway_limit();
    test_route_header();
    test_unknown_remaining();
    test_global_block();
    test_429();
    test_wait_is_max();
    test_independent_counters();
    test_interaction_ignores_global_cap();
    test_interaction_respects_route();
    test_interaction_skips_global_429();
    test_interaction_drains_route_budget();
    test_invalid_request_trips();
    test_invalid_window_rolls_the_count();
    test_invalid_window_spans_multiple_rollovers();
    test_invalid_spread_flood_trips();
    test_invalid_spread_flood_is_one_window();
    test_invalid_counter_saturates();
    test_invalid_spread_saturation_across_windows();
    test_invalid_clock_restarts_at_zero();
    test_invalid_independent_of_rest();

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}