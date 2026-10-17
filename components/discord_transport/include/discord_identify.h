#ifndef DISCORD_IDENTIFY_H
#define DISCORD_IDENTIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * IDENTIFY budget: the guard against Discord revoking the bot token.
 *
 * Discord allows DC_GW_IDENTIFY_DAILY_LIMIT IDENTIFY per 24 h and revokes the
 * token for good above that, with no warning and no appeal. A reboot loop on
 * flaky wifi sends one IDENTIFY per cycle, which exhausts the quota in hours,
 * so the count must outlive the process. A RAM counter cannot do that: it reads
 * 0 after every restart, which is precisely the case it exists to detect. The
 * count and the window start therefore live behind DcIdentifyStore, and
 * dc_identify_record() writes through on every call rather than batching,
 * because the reboot that loses an unwritten count is the reboot that burns the
 * quota.
 *
 * WINDOW: DC_IDENTIFY_WINDOW_MS measured from the FIRST IDENTIFY of the window,
 * never from the last. A sliding window would let a bot reconnecting every hour
 * send IDENTIFY forever and never once be refused. The window opens at the
 * first record and rolls over at the first record after its expiry, so a count
 * of 0 with a window start in the past means "this window already rolled over",
 * and every IDENTIFY counted against it must still be refused.
 *
 * RESET: dc_identify_record() is the single place a window rolls over. It does
 * so when the window it finds has expired, which sets the count to 1 and stamps
 * now_ms as the new window start. dc_identify_may_send() is a pure predicate:
 * it takes a const DcIdentify, writes nothing to the store and changes no
 * field, because an expired window is not a spent budget until something is
 * actually sent. Resetting inside the predicate would mean the read spends the
 * quota, and the read happens on every reconnect attempt from several call
 * sites, so its write would need its own failure handling for no benefit.
 *
 * PRECONDITION, THE CALLER MUST CALL dc_identify_load() FIRST: dc_identify_init()
 * alone leaves the module unable to speak, because the persisted store is the
 * only evidence of what has already been spent. The caller MUST therefore call
 * dc_identify_load() once after dc_identify_init() and before any other call,
 * and MUST treat a false return as FATAL for the gateway rather than retrying
 * blind. dc_identify_may_send() refuses until a load has succeeded, and
 * dc_identify_record() refuses too, for the same reason and with the same
 * consequence:
 *
 *   LOAD FIRST, THEN RECORD: a record taken before a load is refused rather
 *   than acted on. dc_identify_record() cannot tell an unloaded DcIdentify,
 *   whose !window_open means "never asked the store", from one the store has
 *   just told it is empty, and if it opens a window on the former it overwrites
 *   the persisted truth. With a store holding count 1000, window start
 *   1700000000000, an unloaded record at 1700000001000 leaves the store holding
 *   count 1 and start 1700000001000, and the next boot then permits 999 further
 *   IDENTIFY on top of the 1000 already spent, which is exactly the overrun
 *   that costs the token. The refusal has no false ALLOW in it, and the caller
 *   loses nothing by obeying the order, because a record without a load has no
 *   meaning to preserve.
 *
 * UNKNOWN IS REFUSED: dc_identify_may_send() is false until the budget is
 * positively known, that is until dc_identify_load() has succeeded. Before the
 * first successful load the module has no evidence that any budget remains, and
 * spending quota on an assumption is the one error Discord punishes
 * permanently. A load that fails is therefore the module's own refusal, not
 * only a signal for the caller to act on. A successful load of an all-zero
 * store is different: that is a real answer meaning no IDENTIFY has ever been
 * sent, and the first one is allowed.
 *
 * PRECONDITION, now_ms MUST BE EPOCH MILLISECONDS: every now_ms this module is
 * given, and every window start it persists, is milliseconds since the Unix
 * epoch, in one clock domain, never a count from boot and never a millisecond
 * timer that restarts at zero on power-up. A boot-relative clock satisfies the
 * arithmetic but silently bricks the device, because the persisted start from a
 * boot with a synced clock is an epoch value a boot-relative counter can never
 * exceed: now_ms stays below window_start_ms, which the CLOCK rule below
 * refuses, so dc_identify_may_send() returns false forever. Not until SNTP
 * syncs, and not at 24 h either, because a clock reading 86400000 is still a
 * far smaller number than 1700000000000. If the clock never syncs at all, with
 * no RTC, a captive portal, blocked UDP/123 or a dead NTP server, the bot is
 * bricked indefinitely with no log line and no escape. There is deliberately no
 * third state for it: treating disagreeing clocks as "no window" would hand out
 * a fresh budget over a spent one, which is the false ALLOW that costs the
 * token. The caller owns that diagnosis, which is why the epoch requirement is
 * stated here rather than inferred.
 *
 * FRESH DEVICE: a stored window start of zero means the window has never been
 * opened, because a real one comes from a clock reading in milliseconds and is
 * non-zero. dc_identify_load() therefore reads a zero start as "no window yet"
 * rather than as a window that opened at the epoch and has been stuck ever
 * since, which is what treating 0 as a real start would do to every device that
 * has never sent an IDENTIFY: the window could not be opened and the budget
 * spent. A window start of zero is also not a rolling expiry, so a count of 0
 * with a zero start simply reads as a device that has spent nothing.
 *
 * COST OF THE ZERO SENTINEL: zero is the only reserved window start, so a clock
 * that is genuinely at epoch 0 when the first IDENTIFY is sent cannot record it.
 * The record persists a count of 1 against a start of 0, and the next
 * dc_identify_load() reads that back as no window at all, so the next
 * dc_identify_record() opens a fresh window with a full budget and the IDENTIFY
 * just spent is not counted against anything.
 *
 * The loss is per WINDOW REOPEN, not per record. A first record at zero followed
 * by a record under a real clock loses that first IDENTIFY, and the reopened
 * window then permits a further 1000 in the following 24 h. Only the next
 * real-clock record closes the reopened window, so a device that syncs once and
 * then behaves can spend 1000 more than the limit intends.
 *
 * The PRECONDITION above is the only thing bounding this. Under an
 * epoch-millisecond clock that is already past zero when the first IDENTIFY is
 * sent, the case cannot arise, which is the other reason the epoch requirement is
 * not negotiable. If you cannot guarantee that, do not use a zero start: seed
 * the window with a non-zero value before the first record.
 *
 * CLOCK: expiry is evaluated only after a backwards step has been excluded, so
 * a clock that steps back, including a reboot with no RTC, is refused rather
 * than handed a wrapped subtraction that would look like a 585 million year old
 * window. dc_identify_record() never moves the window start: an IDENTIFY taken
 * behind the recorded start is counted, but it must not be able to slide the
 * window backwards, because a backwards start both shortens the window and
 * re-arms it as unexpired, handing out a second full budget before 24 h had
 * passed. Those hours are exactly the ones a reboot loop spends them in. The
 * refusal is silent by design: it is the only direction that cannot spend
 * quota, and a log line per reconnect would be noise on the path that produces
 * most of them.
 *
 * SATURATION: the count saturates at DC_GW_IDENTIFY_DAILY_LIMIT. A wrap would
 * turn a full window into an empty one and reopen the quota, so the refusal has
 * to be the thing that survives.
 *
 * WRITE FAILURE: dc_identify_record() advances the in-RAM count whether or not
 * the store write lands, and returns false when it does not. The RAM count is
 * advanced anyway on purpose, because a write that failed still consumed an
 * IDENTIFY on the wire and the hold must stay conservative: dropping the count
 * would hand the same send twice. The false return is the caller's only signal
 * that the count may be lost across a reboot, so it must be logged and the
 * budget reloaded from the store before the next IDENTIFY is considered.
 *
 * ERROR TYPE: the store callbacks return DcErr, which is ESP-IDF's esp_err_t
 * widened to int32_t so this module builds with no ESP-IDF header. Verified
 * against ESP-IDF v5.1 and against ESP-IDF master, both of which declare
 * `typedef int esp_err_t;`, so a function returning esp_err_t is already
 * return-compatible with int32_t and discord_nvs.c passes esp_err_t through
 * these callbacks unchanged; the tests pass 0 and -1. This is a verified fact
 * about those two trees and not a claim about every ESP-IDF target: a fork that
 * widens esp_err_t to a struct is not covered and would not satisfy the
 * signature. Only non-zero is examined, so no ESP_OK or ESP_FAIL constant
 * travels through this interface and none has to be defined for it to compile.
 *
 * THREADING: none. Single task, no synchronisation, no RTOS primitive, no
 * allocation, in keeping with the rest of the transport core.
 */

#define DC_IDENTIFY_WINDOW_MS 86400000ull

/**
 * ESP-IDF's esp_err_t widened to a fixed-width type.
 *
 * Widened rather than replaced so the callbacks are satisfied by esp_err_t
 * itself on the device, with no adapter and no cast, on the ESP-IDF trees
 * verified above and nowhere else.
 */
typedef int32_t DcErr;

typedef struct {
    DcErr (*read)(void *ctx, uint32_t *count, uint64_t *window_start_ms);
    DcErr (*write)(void *ctx, uint32_t count, uint64_t window_start_ms);
    void  *ctx;
} DcIdentifyStore;

typedef struct {
    DcIdentifyStore store;
    uint32_t        count;
    uint64_t        window_start_ms;
    bool            loaded;
    bool            window_open;
} DcIdentify;

/**
 * Binds a store and clears the budget to zero.
 *
 * The result is unusable on purpose: dc_identify_load() MUST be the next call,
 * before dc_identify_may_send() and before dc_identify_record(), or both will
 * refuse. A DcIdentify that has only been initialised knows nothing about what
 * has already been spent, and guessing spends the token.
 */
void dc_identify_init(DcIdentify *id, const DcIdentifyStore *store);

/**
 * Reads the persisted count and window start into RAM.
 *
 * MUST be called after dc_identify_init() and before any other use of the
 * module. Returns false when the store is absent or unreadable. The transport
 * must treat that as fatal rather than reconnecting on an unknown budget:
 * running unidentified is how a quota gets burned.
 */
bool dc_identify_load(DcIdentify *id);

/**
 * False when the budget is unknown, when the window is exhausted, when the
 * clock is behind the window start, and true when the window has expired or
 * never opened. Reopening the window spends nothing: only
 * dc_identify_record() clears the count.
 */
bool dc_identify_may_send(const DcIdentify *id, uint64_t now_ms);

/**
 * Records one IDENTIFY and writes it through to the store.
 *
 * Opens the window at now_ms when none is open, rolls the window over first
 * when the open one has expired, saturates at the limit, and writes exactly
 * once. The window start is never moved once open. RESUME must not call this:
 * it does not count against the quota.
 *
 * Returns false, and writes nothing, when no load has succeeded, so a caller
 * that skipped dc_identify_load() cannot overwrite the persisted budget with a
 * count of 1. Returns false when the store write did not land, while leaving
 * the in-RAM count advanced so the hold stays conservative. Either false is a
 * real loss of accounting that the caller must log and reload for.
 */
bool dc_identify_record(DcIdentify *id, uint64_t now_ms);

uint32_t dc_identify_used(const DcIdentify *id);
uint64_t dc_identify_window_start(const DcIdentify *id);

/** Bytes of RAM one DcIdentify occupies, store included. */
size_t dc_identify_footprint(void);

#ifdef __cplusplus
}
#endif

#endif
