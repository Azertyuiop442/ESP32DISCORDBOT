#ifndef DISCORD_WATCHDOG_H
#define DISCORD_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Six tasks exist at runtime (five of ours plus the WebSocket client's own),
 * so six slots leaves no headroom: adding a task, or the provisioning
 * console, would fail registration at runtime with a bare -1. Eight gives
 * room at a cost of 64 bytes. */
#define DC_WD_TASK_SLOTS 8

typedef struct {
    const char *name;
    uint32_t    expected_period_ms;
    uint64_t    last_seen_ms;
    uint32_t    misses;
    bool        registered;
    bool        tripped;
} DcWatchdogTask;

typedef struct {
    DcWatchdogTask tasks[DC_WD_TASK_SLOTS];
    uint32_t       slot_count;
    uint32_t       tolerance;
    uint32_t       trips;
} DcWatchdog;

typedef struct {
    const char *name;
    uint32_t    overdue_ms;
} DcWatchdogReport;

void dc_watchdog_init(DcWatchdog *wd, uint32_t tolerance);
int dc_watchdog_register(DcWatchdog *wd, const char *name, uint32_t expected_period_ms);
void dc_watchdog_pet(DcWatchdog *wd, int slot, uint64_t now_ms);
uint32_t dc_watchdog_scan(DcWatchdog *wd, uint64_t now_ms, DcWatchdogReport *reports,
                          uint32_t report_cap);
bool dc_watchdog_healthy(const DcWatchdog *wd);

#ifdef __cplusplus
}
#endif

#endif