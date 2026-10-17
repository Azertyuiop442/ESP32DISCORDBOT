#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "discord_gateway.h"
#include "discord_identify.h"

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

typedef struct { uint32_t count; uint64_t start; int writes; int fail_read; int fail_write; } FakeStore;

static int32_t fake_read(void *ctx, uint32_t *c, uint64_t *w)
{
    FakeStore *f = ctx;
    if (f->fail_read) return -1;
    *c = f->count; *w = f->start; return 0;
}
static int32_t fake_write(void *ctx, uint32_t c, uint64_t w)
{
    FakeStore *f = ctx;
    f->writes++;
    if (f->fail_write) return -1;
    f->count = c; f->start = w; return 0;
}

static const uint64_t DAY_MS = 24ULL * 60ULL * 60ULL * 1000ULL;

static void bind(FakeStore *fs, DcIdentify *id)
{
    DcIdentifyStore store;

    memset(&store, 0, sizeof(store));
    store.read = fake_read;
    store.write = fake_write;
    store.ctx = fs;
    dc_identify_init(id, &store);
}

static void reboot(FakeStore *fs, DcIdentify *id)
{
    dc_identify_init(id, NULL);
    bind(fs, id);
    dc_identify_load(id);
}

static void test_quota_blocks_at_limit(void)
{
    FakeStore fs = { DC_GW_IDENTIFY_DAILY_LIMIT, 1000, 0, 0, 0 };
    DcIdentify id;
    bind(&fs, &id);

    check(dc_identify_load(&id), "store loads");
    check(!dc_identify_may_send(&id, 2000), "at the limit, IDENTIFY refused");
    check(dc_identify_used(&id) == DC_GW_IDENTIFY_DAILY_LIMIT, "count read back");
}

static void test_window_expiry(void)
{
    FakeStore fs = { DC_GW_IDENTIFY_DAILY_LIMIT, 1000, 0, 0, 0 };
    DcIdentify id;
    bind(&fs, &id);
    dc_identify_load(&id);

    check(!dc_identify_may_send(&id, 1000 + DAY_MS - 1), "still blocked just before 24 h");
    check(dc_identify_may_send(&id, 1000 + DAY_MS), "window reopens at 24 h");
}

static void test_unreadable_is_fatal_signal(void)
{
    FakeStore fs = { 0, 0, 0, 1, 0 };
    DcIdentify id;
    bind(&fs, &id);
    check(!dc_identify_load(&id), "unreadable store reports failure to the caller");
}

static void test_record_persists(void)
{
    FakeStore fs = { 0, 0, 0, 0, 0 };
    DcIdentify id;
    bind(&fs, &id);
    dc_identify_load(&id);

    check(dc_identify_record(&id, 5000), "a landed write reports success");
    check(dc_identify_used(&id) == 1, "count incremented");
    check(fs.writes == 1, "persisted immediately, RAM alone would lose it on reboot");
    check(dc_identify_window_start(&id) == 5000, "window opened at first IDENTIFY");
}

static void test_failed_load_refuses_identify(void)
{
    FakeStore fs = { 0, 0, 0, 1, 0 };
    DcIdentify id;
    bind(&fs, &id);

    check(!dc_identify_load(&id), "the load failed");
    check(dc_identify_used(&id) == 0, "a failed load reports no usage");
    check(!dc_identify_may_send(&id, 1000), "an unreadable store refuses IDENTIFY, it is FATAL");
}

static void test_window_is_fixed_from_the_first_identify(void)
{
    FakeStore fs = { 0, 0, 0, 0, 0 };
    DcIdentify id;
    int writes_before;
    bind(&fs, &id);
    dc_identify_load(&id);

    dc_identify_record(&id, 5000);
    dc_identify_record(&id, 9000);
    dc_identify_record(&id, 9000000);
    check(dc_identify_used(&id) == 3, "three IDENTIFY recorded");
    check(dc_identify_window_start(&id) == 5000,
          "the window still starts at the first IDENTIFY, later ones do not slide it");
    check(fs.start == 5000, "the persisted window start is the first IDENTIFY, not the newest");

    writes_before = fs.writes;
    while (dc_identify_may_send(&id, 9000000)) {
        dc_identify_record(&id, 9000000);
    }
    check(dc_identify_used(&id) == DC_GW_IDENTIFY_DAILY_LIMIT, "the window filled to the limit");
    check(writes_before == 3, "each of the three IDENTIFY wrote exactly once, no batching");

    check(!dc_identify_may_send(&id, 5000 + DAY_MS - 1),
          "a sliding window would have reopened long before 24 h from the first IDENTIFY");
    check(dc_identify_may_send(&id, 5000 + DAY_MS),
          "the fixed window reopens exactly 24 h after the first IDENTIFY");

    dc_identify_record(&id, 5000 + DAY_MS);
    check(dc_identify_used(&id) == 1, "the first IDENTIFY of the next window restarts the count");
    check(dc_identify_window_start(&id) == 5000 + DAY_MS, "and opens a new window");
}

static void test_may_send_is_read_only(void)
{
    FakeStore fs = { DC_GW_IDENTIFY_DAILY_LIMIT, 1000, 0, 0, 0 };
    DcIdentify id;
    bind(&fs, &id);
    dc_identify_load(&id);

    check(dc_identify_may_send(&id, 1000 + DAY_MS), "the expired window reads as open");
    check(fs.writes == 0, "asking the question does not write to the store");
    check(dc_identify_used(&id) == DC_GW_IDENTIFY_DAILY_LIMIT,
          "asking the question does not clear the count in RAM either");
    check(dc_identify_window_start(&id) == 1000, "nor does it move the window start");

    dc_identify_record(&id, 1000 + DAY_MS);
    check(dc_identify_used(&id) == 1, "only the record opens the new window");
    check(fs.writes == 1, "and only the record wrote");
}

static void test_record_clamped_and_window_never_recedes(void)
{
    FakeStore fs = { 0, 0, 0, 0, 0 };
    DcIdentify id;
    bind(&fs, &id);
    dc_identify_load(&id);

    dc_identify_record(&id, 10000);
    check(dc_identify_may_send(&id, 20000), "one IDENTIFY leaves room in a fresh window");

    dc_identify_record(&id, 5000);
    check(dc_identify_used(&id) == 2, "a clock that steps backwards still counts the IDENTIFY");
    check(dc_identify_window_start(&id) == 10000,
          "a backwards clock cannot slide the window start into the future");

    while (dc_identify_may_send(&id, 10000)) {
        dc_identify_record(&id, 10000);
    }
    check(dc_identify_used(&id) == DC_GW_IDENTIFY_DAILY_LIMIT,
          "sending until refused stops at exactly the limit");
    dc_identify_record(&id, 10000);
    check(dc_identify_used(&id) == DC_GW_IDENTIFY_DAILY_LIMIT,
          "one IDENTIFY past the limit saturates instead of wrapping");
    check(!dc_identify_may_send(&id, 10000), "a wrapped count would falsely reopen the window");
}

static void test_unknown_budget_is_refused(void)
{
    FakeStore fs = { 0, 0, 0, 0, 0 };
    DcIdentify id;
    bind(&fs, &id);

    check(!dc_identify_may_send(&id, 0), "before any load the budget is unknown, so IDENTIFY is refused");
    check(!dc_identify_may_send(&id, 5000), "and it stays refused however far the clock runs");
    check(!dc_identify_may_send(&id, 5000 + DAY_MS), "a clock past 24 h does not invent a budget");

    dc_identify_load(&id);
    check(dc_identify_used(&id) == 0, "an all zero store loads as no usage");
    check(dc_identify_window_start(&id) == 0, "with a window start of zero");
    check(dc_identify_may_send(&id, 5000), "a fresh device whose store reads zero may IDENTIFY");
    check(dc_identify_window_start(&id) == 0, "and reading it does not open a window");

    dc_identify_record(&id, 5000);
    check(dc_identify_window_start(&id) == 5000, "the first record on a fresh device opens at now");
    check(dc_identify_used(&id) == 1, "and counts as the first IDENTIFY");
}

static void test_null_guards(void)
{
    FakeStore fs = { 0, 0, 0, 0, 0 };
    DcIdentify id;

    dc_identify_init(NULL, NULL);
    check(true, "init(NULL) returns instead of writing through a NULL pointer");
    check(!dc_identify_load(NULL), "load(NULL) refuses");
    check(dc_identify_used(NULL) == 0, "used(NULL) is 0");
    check(dc_identify_window_start(NULL) == 0, "window_start(NULL) is 0");
    check(!dc_identify_may_send(NULL, 1000), "may_send(NULL) refuses instead of crashing");
    check(!dc_identify_record(NULL, 1000), "record(NULL) refuses instead of crashing");

    dc_identify_init(&id, NULL);
    check(!dc_identify_load(&id), "a NULL store is unreadable, so the caller goes FATAL");
    check(dc_identify_used(&id) == 0, "a NULL store leaves the counter at 0");
    check(!dc_identify_record(&id, 1000),
          "an unloaded record refuses, which is how a NULL store cannot spend anything");
    check(dc_identify_used(&id) == 0, "so the counter stays at 0");
    check(dc_identify_window_start(&id) == 0, "and no window is opened");

    bind(&fs, &id);
    check(dc_identify_used(&id) == 0, "init with a store clears the RAM state left by the last test");
    check(!dc_identify_may_send(&id, 5000), "and clears the knowledge that made it sendable");
    check(dc_identify_load(&id), "then the real store loads");
    check(dc_identify_may_send(&id, 5000), "and a zero store reads as a fresh device again");
    check(fs.writes == 0, "a refused record reached no store, so it never wrote");
}

static void test_record_before_load_cannot_destroy_a_persisted_budget(void)
{
    FakeStore fs = { DC_GW_IDENTIFY_DAILY_LIMIT, 1700000000000ull, 0, 0, 0 };
    DcIdentify id;
    bind(&fs, &id);

    check(!dc_identify_record(&id, 1700000001000ull),
          "a record before any load is refused, not acted on");
    check(dc_identify_used(&id) == 0, "so RAM count is untouched");
    check(fs.writes == 0, "and the persisted budget was never written to");
    check(fs.count == DC_GW_IDENTIFY_DAILY_LIMIT, "the exhausted count is still 1000 in the store");
    check(fs.start == 1700000000000ull, "and the window start is still the persisted one");

    reboot(&fs, &id);
    check(dc_identify_used(&id) == DC_GW_IDENTIFY_DAILY_LIMIT,
          "after a reboot the 1000 IDENTIFYs already spent are still counted");
    check(!dc_identify_may_send(&id, 1700000001000ull),
          "so the exhausted window still refuses, 999 more would have cost the token");
}

static void test_backwards_clock_is_refused_within_a_live_window(void)
{
    FakeStore fs = { 0, 10000, 0, 0, 0 };
    DcIdentify id;
    bind(&fs, &id);
    dc_identify_load(&id);

    check(dc_identify_used(&id) == 0, "a window is open at 10000 with nothing spent in it");
    check(dc_identify_may_send(&id, 11000), "a clock slightly ahead is allowed to spend");
    check(!dc_identify_may_send(&id, 5000),
          "a clock behind the window start is refused, that guard is not reachable elsewhere");
    check(!dc_identify_may_send(&id, 9999),
          "one millisecond behind the start is refused too");
    check(dc_identify_may_send(&id, 10000), "the window start itself is not a backwards clock");

    fs.count = DC_GW_IDENTIFY_DAILY_LIMIT;
    dc_identify_load(&id);
    check(dc_identify_may_send(&id, 10000 + DAY_MS), "the window is genuinely expired at 24 h");
    check(!dc_identify_may_send(&id, 5000), "a clock still behind the start refuses an expired window too");
}

static void test_write_failure_is_surfaced_and_stays_conservative(void)
{
    FakeStore fs = { 0, 0, 0, 0, 1 };
    DcIdentify id;
    bind(&fs, &id);
    dc_identify_load(&id);

    check(!dc_identify_record(&id, 5000), "a store write that did not land is reported to the caller");
    check(dc_identify_used(&id) == 1, "the RAM count is advanced anyway, so the hold is conservative");
    check(fs.writes == 1, "the store was attempted exactly once, not silently skipped");
    check(fs.count == 0 && fs.start == 0, "and nothing was persisted");

    fs.fail_write = 0;
    check(dc_identify_record(&id, 9000), "the next write succeeds once the store recovers");
    check(fs.count == 2, "and writes through the count the failed write had advanced in RAM");
    check(fs.start == 5000, "against the window start the failed write also opened");

    fs.fail_write = 1;
    check(!dc_identify_record(&id, 13000), "a failure mid window is reported too");
    check(dc_identify_used(&id) == 3, "and RAM is advanced so no IDENTIFY is sent twice");
    check(dc_identify_window_start(&id) == 5000, "without moving the window start");

    reboot(&fs, &id);
    check(dc_identify_used(&id) == 2, "a reboot loses only the counts that were never written");
}

static void test_zero_window_start_costs_one_record(void)
{
    FakeStore fs = { 0, 0, 0, 0, 0 };
    DcIdentify id;
    bind(&fs, &id);
    dc_identify_load(&id);

    check(dc_identify_record(&id, 0), "a record at ms 0 is taken");
    check(fs.count == 1 && fs.start == 0,
          "and persisted, which is the zero sentinel collision: a count of 1 against no window");

    reboot(&fs, &id);
    check(dc_identify_window_start(&id) == 0, "a reboot cannot tell that a window was open at 0");
    check(dc_identify_record(&id, 5000), "so the next record opens a second window instead");
    check(fs.count == 1, "and the IDENTIFY recorded before the reboot is not counted against it");
    check(dc_identify_used(&id) == 1, "the window holds only what has been sent since the reboot");

    fs.start = 1;
    dc_identify_load(&id);
    check(dc_identify_window_start(&id) == 1, "a window at ms 1 is an ordinary window");
    check(dc_identify_record(&id, 2), "and a record inside it is taken");
    check(fs.count == 2, "the zero sentinel costs nothing once the clock has left 0");
}

static void test_high_count_then_failed_reload_holds(void)
{
    FakeStore fs = { DC_GW_IDENTIFY_DAILY_LIMIT, 1700000000000ull, 0, 0, 0 };
    DcIdentify id;
    bind(&fs, &id);
    dc_identify_load(&id);

    fs.count = DC_GW_IDENTIFY_DAILY_LIMIT - 10;
    check(dc_identify_load(&id), "a near exhausted window loads");
    check(dc_identify_used(&id) == DC_GW_IDENTIFY_DAILY_LIMIT - 10, "with the persisted count");
    check(dc_identify_may_send(&id, 1700000001000ull), "so the last 10 IDENTIFY are still allowed");

    fs.fail_read = 1;
    check(!dc_identify_load(&id), "a reload now fails");
    check(!dc_identify_may_send(&id, 1700000001000ull),
          "a failed reload after a high count does not spend, the budget is unknown again");
    check(dc_identify_used(&id) == 0, "and the RAM count is cleared, not left half known");
    check(!dc_identify_record(&id, 1700000001000ull), "record refuses too, so nothing can re-open it");
    check(fs.count == DC_GW_IDENTIFY_DAILY_LIMIT - 10, "the store keeps the count it already had");
    check(fs.writes == 0, "and no write was attempted, the refused record never reached it");

    fs.fail_read = 0;
    check(dc_identify_load(&id), "a successful reload restores the truth");
    check(dc_identify_used(&id) == DC_GW_IDENTIFY_DAILY_LIMIT - 10, "count and");
    check(dc_identify_may_send(&id, 1700000001000ull), "budget are the persisted ones");
}

static void test_constant_is_discords(void)
{
    check(DC_GW_IDENTIFY_DAILY_LIMIT == 1000u, "the limit is Discord's 1000, not a config mirror");
}

int main(void)
{
    test_quota_blocks_at_limit();
    test_window_expiry();
    test_unreadable_is_fatal_signal();
    test_record_persists();
    test_failed_load_refuses_identify();
    test_window_is_fixed_from_the_first_identify();
    test_may_send_is_read_only();
    test_record_clamped_and_window_never_recedes();
    test_unknown_budget_is_refused();
    test_null_guards();
    test_record_before_load_cannot_destroy_a_persisted_budget();
    test_backwards_clock_is_refused_within_a_live_window();
    test_write_failure_is_surfaced_and_stays_conservative();
    test_zero_window_start_costs_one_record();
    test_high_count_then_failed_reload_holds();
    test_constant_is_discords();

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
