#include "discord_watchdog.h"

#include <string.h>

void dc_watchdog_init(DcWatchdog *wd, uint32_t tolerance)
{
    memset(wd, 0, sizeof(*wd));
    wd->slot_count = 0;
    wd->tolerance = tolerance ? tolerance : 3;
    wd->trips = 0;
}

int dc_watchdog_register(DcWatchdog *wd, const char *name, uint32_t expected_period_ms)
{
    if (wd->slot_count >= DC_WD_TASK_SLOTS) {
        return -1;
    }

    uint32_t idx = wd->slot_count;
    DcWatchdogTask *t = &wd->tasks[idx];
    memset(t, 0, sizeof(*t));
    t->name = name;
    t->expected_period_ms = expected_period_ms;
    t->registered = true;
    t->misses = 0;
    t->tripped = false;
    wd->slot_count++;
    return (int)idx;
}

void dc_watchdog_pet(DcWatchdog *wd, int slot, uint64_t now_ms)
{
    if (slot < 0 || (uint32_t)slot >= wd->slot_count) {
        return;
    }
    DcWatchdogTask *t = &wd->tasks[slot];
    t->last_seen_ms = now_ms;
    t->misses = 0;
    t->tripped = false;
}

uint32_t dc_watchdog_scan(DcWatchdog *wd, uint64_t now_ms, DcWatchdogReport *reports,
                          uint32_t report_cap)
{
    uint32_t found = 0;

    for (uint32_t i = 0; i < wd->slot_count; i++) {
        DcWatchdogTask *t = &wd->tasks[i];
        if (!t->registered) {
            continue;
        }

        if (now_ms < t->last_seen_ms) {
            continue;
        }

        uint64_t elapsed = now_ms - t->last_seen_ms;
        uint64_t deadline = (uint64_t)t->expected_period_ms * wd->tolerance;
        bool overdue = elapsed > deadline;

        if (overdue) {
            t->misses++;
            if (t->misses >= 2 && !t->tripped) {
                t->tripped = true;
                wd->trips++;
                if (reports && found < report_cap) {
                    reports[found].name = t->name;
                    reports[found].overdue_ms = (uint32_t)(elapsed - deadline);
                    found++;
                }
            }
        } else {
            t->misses = 0;
        }
    }

    return found;
}

bool dc_watchdog_healthy(const DcWatchdog *wd)
{
    for (uint32_t i = 0; i < wd->slot_count; i++) {
        if (wd->tasks[i].registered && wd->tasks[i].tripped) {
            return false;
        }
    }
    return true;
}