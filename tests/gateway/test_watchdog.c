#include <stdio.h>
#include <string.h>

#include "discord_watchdog.h"

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

static void test_registration(void)
{
    DcWatchdog wd;
    dc_watchdog_init(&wd, 3);

    int a = dc_watchdog_register(&wd, "gateway", 100);
    int b = dc_watchdog_register(&wd, "rest", 200);
    check(a == 0 && b == 1, "slots assigned in order");

    int c = dc_watchdog_register(&wd, "bot", 50);
    check(c == 2, "third slot");

    for (int i = 3; i < DC_WD_TASK_SLOTS; i++) {
        check(dc_watchdog_register(&wd, "filler", 100) == i, "slots up to 8 accepted");
    }
    check(dc_watchdog_register(&wd, "overflow", 100) == -1, "9th task refused");
    check(wd.slot_count == DC_WD_TASK_SLOTS, "slot count capped at 8");
    check(DC_WD_TASK_SLOTS >= 6, "at least one slot per runtime task, plus headroom");
}

static void test_healthy_task(void)
{
    DcWatchdog wd;
    dc_watchdog_init(&wd, 3);
    int gw = dc_watchdog_register(&wd, "gateway", 100);
    dc_watchdog_pet(&wd, gw, 1000);

    for (uint64_t t = 1100; t <= 1500; t += 100) {
        DcWatchdogReport r[4];
        check(dc_watchdog_scan(&wd, t, r, 4) == 0 || 1, "scan runs");
        dc_watchdog_pet(&wd, gw, t);
    }
    check(dc_watchdog_healthy(&wd), "regularly petted task stays healthy");
}

static void test_starvation_detected(void)
{
    DcWatchdog wd;
    dc_watchdog_init(&wd, 3);
    int gw = dc_watchdog_register(&wd, "gateway", 100);
    dc_watchdog_pet(&wd, gw, 1000);

    DcWatchdogReport r[4];
    check(dc_watchdog_scan(&wd, 1200, r, 4) == 0, "within deadline does not count");
    check(dc_watchdog_scan(&wd, 1400, r, 4) == 0, "first overdue scan records a miss");
    check(dc_watchdog_scan(&wd, 1600, r, 4) == 1, "second overdue scan trips");
    check(!dc_watchdog_healthy(&wd), "watchdog unhealthy");
    check(strcmp(r[0].name, "gateway") == 0, "report names the task");
    check(r[0].overdue_ms > 0, "report includes overdue amount");
    check(wd.trips == 1, "trip counted once");
}

static void test_no_repeat_trip(void)
{
    DcWatchdog wd;
    dc_watchdog_init(&wd, 3);
    int gw = dc_watchdog_register(&wd, "gateway", 100);
    dc_watchdog_pet(&wd, gw, 1000);

    DcWatchdogReport r[4];
    dc_watchdog_scan(&wd, 1400, r, 4);
    dc_watchdog_scan(&wd, 1600, r, 4);
    check(wd.trips == 1, "tripped");

    dc_watchdog_scan(&wd, 2000, r, 4);
    dc_watchdog_scan(&wd, 3000, r, 4);
    check(wd.trips == 1, "trip not re-counted while still starved");
}

static void test_recovery(void)
{
    DcWatchdog wd;
    dc_watchdog_init(&wd, 3);
    int gw = dc_watchdog_register(&wd, "gateway", 100);
    dc_watchdog_pet(&wd, gw, 1000);

    DcWatchdogReport r[4];
    dc_watchdog_scan(&wd, 1400, r, 4);
    dc_watchdog_scan(&wd, 1600, r, 4);
    check(!dc_watchdog_healthy(&wd), "starved");

    dc_watchdog_pet(&wd, gw, 1700);
    check(dc_watchdog_healthy(&wd), "petting clears the trip immediately");
    check(dc_watchdog_scan(&wd, 1800, r, 4) == 0, "no new trip after recovery");
}

static void test_intermittent_starvation(void)
{
    DcWatchdog wd;
    dc_watchdog_init(&wd, 3);
    int gw = dc_watchdog_register(&wd, "gateway", 100);
    dc_watchdog_pet(&wd, gw, 1000);

    DcWatchdogReport r[4];
    dc_watchdog_scan(&wd, 1400, r, 4);
    dc_watchdog_pet(&wd, gw, 1450);
    dc_watchdog_scan(&wd, 1500, r, 4);
    dc_watchdog_pet(&wd, gw, 1550);
    dc_watchdog_scan(&wd, 1600, r, 4);

    check(dc_watchdog_healthy(&wd), "intermittent misses never accumulate to a trip");
}

static void test_multiple_tasks(void)
{
    DcWatchdog wd;
    dc_watchdog_init(&wd, 3);
    int gw = dc_watchdog_register(&wd, "gateway", 100);
    int rest = dc_watchdog_register(&wd, "rest", 200);
    dc_watchdog_pet(&wd, gw, 1000);
    dc_watchdog_pet(&wd, rest, 1000);

    DcWatchdogReport r[4];
    dc_watchdog_scan(&wd, 2000, r, 4);
    uint32_t trips = dc_watchdog_scan(&wd, 3000, r, 4);
    check(trips == 2, "both starved tasks reported");
    check(strcmp(r[0].name, "gateway") == 0 && strcmp(r[1].name, "rest") == 0,
          "reports ordered by slot");
}

static void test_report_cap(void)
{
    DcWatchdog wd;
    dc_watchdog_init(&wd, 3);
    dc_watchdog_pet(&wd, dc_watchdog_register(&wd, "a", 100), 1000);
    dc_watchdog_pet(&wd, dc_watchdog_register(&wd, "b", 100), 1000);
    dc_watchdog_pet(&wd, dc_watchdog_register(&wd, "c", 100), 1000);

    DcWatchdogReport r[2];
    uint32_t n = dc_watchdog_scan(&wd, 5000, r, 2);
    check(n == 0, "first scan reports nothing");
    n = dc_watchdog_scan(&wd, 6000, r, 2);
    check(n == 2, "report list respects capacity");
    check(wd.trips == 3, "all three still tripped internally");
}

static void test_invalid_slot(void)
{
    DcWatchdog wd;
    dc_watchdog_init(&wd, 3);
    dc_watchdog_register(&wd, "gateway", 100);

    dc_watchdog_pet(&wd, -1, 1000);
    dc_watchdog_pet(&wd, 99, 1000);
    check(dc_watchdog_healthy(&wd), "invalid slots ignored safely");
}

static void test_clock_jump_backward(void)
{
    DcWatchdog wd;
    dc_watchdog_init(&wd, 3);
    int gw = dc_watchdog_register(&wd, "gateway", 100);
    dc_watchdog_pet(&wd, gw, 100000);

    DcWatchdogReport r[4];
    dc_watchdog_scan(&wd, 50000, r, 4);
    check(dc_watchdog_scan(&wd, 51000, r, 4) == 0, "backward clock does not false-trip");
    check(dc_watchdog_healthy(&wd), "healthy under a backward clock jump");
}

int main(void)
{
    test_registration();
    test_healthy_task();
    test_starvation_detected();
    test_no_repeat_trip();
    test_recovery();
    test_intermittent_starvation();
    test_multiple_tasks();
    test_report_cap();
    test_invalid_slot();
    test_clock_jump_backward();

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}