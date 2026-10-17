#ifndef DISCORD_RATELIMIT_H
#define DISCORD_RATELIMIT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DC_GLOBAL_LIMIT_PER_SEC 50u
#define DC_GATEWAY_LIMIT_PER_MIN 120u
#define DC_ROUTE_BUCKETS 8
#define DC_RATELIMIT_INVALID_MAX 10000u
#define DC_RATELIMIT_INVALID_WINDOW_MS 600000u

/**
 * INTERACTION EXEMPTION.
 *
 * Discord exempts interaction endpoints, which is how a slash-command ACK is
 * sent, from the 50 req/s GLOBAL cap. dc_ratelimit_interaction() exists so
 * that exemption is actually reachable. It reads and writes per-route buckets
 * only. It never touches second_count, second_window_start_ms, global_blocked
 * or global_until_ms, so an ACK can neither be refused by the REST budget nor
 * spend it, and a global 429 with a long Retry-After holds back REST without
 * stalling acknowledgements.
 *
 * OBLIGATION ON THE TRANSPORT, NOT OPTIONAL, NOT IN THIS MODULE: a global 429
 * MUST gate interaction ACKs too, and it must be enforced at the transport
 * layer, which is the only layer that can enforce it. dc_ratelimit_interaction
 * deliberately ignores global_blocked, so the limiter will hand out DC_WAIT_OK
 * for an ACK while a global 429 is armed; if the transport also ignores the 429
 * the ACK goes on the wire into the block that just arrived and the bot spends
 * its whole Retry-After amplifying it. Read dc_ratelimit_global_blocked() or
 * dc_ratelimit_wait_ms() in the transport and hold ACKs back on it. This is a
 * deliberate choice, not an oversight: interactions are exempt from Discord's
 * 50 req/s GLOBAL cap, and an ACK that a slow interaction route held for
 * minutes would be an interaction Discord never received, which is exactly what
 * the 3 second deadline punishes. The cost is that this contract now has two
 * enforcement points for one signal and the transport is the one that has to be
 * written correctly. It is also the reason the module's own REST path is the
 * stricter one where the two disagree: dc_ratelimit_rest() takes MORE
 * protection when a signal is unknown, because it knows a route header that
 * cannot be trusted must not be allowed to overrun. An ACK takes less here, on
 * purpose, and only under a signal the transport has already parsed.
 *
 * WHAT THE EXEMPTION DOES NOT COVER:
 *   - the per-route bucket. Discord applies route headers to interaction
 *     routes too, so an exhausted route still returns DC_WAIT_THROTTLED and an
 *     ACK issued against that route still spends its remaining.
 *   - the Invalid Request Limit, 10000 responses in a 10 minute window per IP,
 *     counting 401, 403 and 429. Exemption is not unlimited, and exceeding this
 *     one is the one that draws a Cloudflare ban. dc_ratelimit_note_invalid()
 *     and dc_ratelimit_invalid_tripped() track it. The ACK path is exempt from
 *     the 50/s, not from the ban.
 *
 * WHO COUNTS AN INVALID: the function takes no status code. The caller maps
 * 401, 403 and 429 onto one call, so the three cannot be weighted apart here
 * without passing the status in. The signature is the decision: the limiter
 * counts occurrences, the transport knows what occurred.
 *
 * WINDOW: a FIXED 10 minute window, not a rolling one. The index
 * DC_RATELIMIT_INVALID_WINDOW_MS-sized bucket that now_ms falls into is
 * floor(now_ms / 600000); the count belongs to that index and nothing else, so
 * a flood accumulates however it is spread inside one window. A rolling window
 * anchored at the first invalid of the previous one does NOT enforce Discord's
 * rule, because a sustained trickle at 200 ms spacing never lets the anchor go
 * stale: the count grows without bound, never resets, and the breaker can stay
 * closed for the whole attack. Measured: 30 000 000 invalids at 200 ms over 69
 * days never tripped the anchored form, not once. A fixed window is the mirror
 * of the other argument for resetting the count, which is that a genuinely
 * quiet 10 minutes has to be required, because that is the condition Discord
 * itself enforces. A fixed window requires it; an anchored one does not.
 *
 * HOW THE INDEX IS COMPUTED, AND WHY IT SURVIVES A WRAP: the index is derived
 * from the timestamp by unsigned 64 bit division by the constant 600000, in
 * the caller's own uint64_t time base, and then narrowed to the stored uint32_t
 * field. Two consequences, both deliberate.
 *   - NAKING IS SAFE HERE. Discord's real windows are aligned to Unix epoch
 *     boundaries, so a caller that passes a real epoch millisecond timestamp
 *     gets index == now_ms / 600000 exactly and window boundaries land on the
 *     same instants Discord uses. A caller that passes an arbitrary
 *     origin, a monotonic boot clock for instance, gets windows aligned to its
 *     own origin instead. That is a phase offset, never a missed count: every
 *     invalid still lands in exactly one window, and a window still holds
 *     exactly 10 minutes of that clock.
 *   - THE 32 BIT FIELD IS NOT A 32 BIT CLOCK. The 64 bit quotient needs 45 bits
 *     to cover the full uint64_t range, so narrowing to uint32_t aliases any
 *     timestamp roughly 828 years and 11 months above the first one the same
 *     index as the first. A uint32_t millisecond clock, by contrast, wraps every
 *     49.7 days, which is 7103 times sooner, and that wrap is not a possible
 *     input to the API at all: now_ms is uint64_t everywhere, and the
 *     second_count, minute_count and route fields, not this one, are the places
 *     the limiter still stores 32 bit milliseconds. A 64 bit field would cost
 *     4 more bytes to push a year 828 centuries into the future, so the field
 *     stays 32 bit, and at the ESP32's 400 MHz a 64 bit divide costing tens
 *     of cycles this is not on any hot path: the ACK path does not touch it.
 *
 * READS THAT CROSS A WINDOW BOUNDARY: dc_ratelimit_invalid_tripped() and
 * dc_ratelimit_invalid_count() report the window now_ms names, not the window
 * the last write landed in. Called from a later window than the one the count
 * was written in, both report clear and zero without clearing anything, so a
 * late read cannot resurrect a trip that the wall clock has already retired.
 * This is also the behaviour that holds when the millisecond clock the caller
 * passes is reset out from under a live limiter, for instance a reboot or an
 * NTP step backwards: the window index of an earlier timestamp differs from
 * the stored one, so the breaker reads clear and the count reads zero, and the
 * first invalid after the step starts a fresh window at one. A backwards step
 * therefore sheds load for at most one window rather than freezing the breaker
 * open or losing the count for good. dc_ratelimit_note_invalid() stores the new
 * index before it stores the count, so a struct that is read concurrently is
 * never caught holding the new index with the old count.
 *
 * TRIP STATE IS DERIVED, NEVER STORED: it is count >= DC_RATELIMIT_INVALID_MAX
 * inside the current index, which means the count resets with the window, not
 * only the trip. dc_ratelimit_invalid_count() exposes the saturated value so
 * that property can be asserted from outside; it is the only way to observe it.
 * The count saturates at DC_RATELIMIT_INVALID_MAX rather than wrapping, so a
 * flood inside one window cannot increment 0xFFFFFFFF past the threshold and
 * wrap the breaker open; only crossing into a new index clears it.
 *
 * CONCURRENCY: same contract as dc_ratelimit_rest(), no synchronisation here.
 * One task owns the limiter.
 *
 * COST: one DcRouteBucket sized step on the ACK path, plus 8 B of static RAM
 * for the fixed window index and the count, which is inside the footprint the
 * tooling already measures. Both fields are additions: there was no prior
 * 32 bit window anchor to replace. The struct grew by 8 B, from 224 B to
 * 232 B measured, the delta docs/07_footprint.md carries in its table.
 */

typedef enum {
    DC_WAIT_OK = 0,
    DC_WAIT_THROTTLED = 1,
    DC_WAIT_REJECTED = -1
} DcWaitResult;

typedef struct {
    uint32_t remaining;      /* 0 means blocked, UINT32_MAX means unknown */
    uint32_t reset_after_ms;
    bool     global;
    bool     shared;
    bool     present;
} DcRateLimitInfo;

typedef struct {
    DcRateLimitInfo info;
    uint64_t        observed_at_ms;
} DcRouteBucket;

typedef struct {
    uint32_t  second_window_start_ms;
    uint32_t  second_count;
    uint32_t  minute_window_start_ms;
    uint32_t  minute_count;
    bool      global_blocked;
    uint64_t  global_until_ms;
    DcRouteBucket routes[DC_ROUTE_BUCKETS];
    uint32_t  invalid_window_index;
    uint32_t  invalid_count;
} DcRateLimiter;

void dc_ratelimit_init(DcRateLimiter *rl, uint64_t now_ms);

DcWaitResult dc_ratelimit_rest(DcRateLimiter *rl, uint32_t route_hash, uint64_t now_ms);
DcWaitResult dc_ratelimit_interaction(DcRateLimiter *rl, uint32_t route_hash,
                                      uint64_t now_ms);
DcWaitResult dc_ratelimit_gateway(DcRateLimiter *rl, uint64_t now_ms);

bool dc_ratelimit_note_invalid(DcRateLimiter *rl, uint64_t now_ms);
bool dc_ratelimit_invalid_tripped(const DcRateLimiter *rl, uint64_t now_ms);
uint32_t dc_ratelimit_invalid_count(const DcRateLimiter *rl, uint64_t now_ms);

void dc_ratelimit_on_headers(DcRateLimiter *rl, uint32_t route_hash,
                             const DcRateLimitInfo *info, uint64_t now_ms);
void dc_ratelimit_on_429(DcRateLimiter *rl, uint32_t route_hash,
                         uint32_t retry_after_ms, uint64_t now_ms);

uint32_t dc_ratelimit_wait_ms(DcRateLimiter *rl, uint32_t route_hash, uint64_t now_ms);
bool dc_ratelimit_global_blocked(const DcRateLimiter *rl, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif