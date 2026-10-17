#ifndef DISCORD_INTQ_H
#define DISCORD_INTQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "discord_feature.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bounded interaction ring: 4 slots, newest shed on overload.
 *
 * CAPACITY 4 IS DELIBERATE. A queued entry costs sizeof(DiscordInteraction)
 * = 1688 B of DRAM, so the ring is 6.8 KiB before its index. Four is a burst
 * absorber (a raid's first frames) — not a backlog. Growing it trades RAM for
 * latency Discord has already cancelled: an interaction ACKed but answered 30 s
 * later is worse than one shed immediately, and the shed is counted.
 *
 * SHED = DROP NEWEST, the opposite of dc_ackq (drop oldest). An ACK already in
 * the queue is a promise to Discord; an interaction still in this ring is a
 * handler a user is waiting on. Evicting the oldest would break that promise
 * for the entry that has waited longest, so the arrival that does not fit is
 * the one that goes: the user sees "This interaction failed", nothing else is
 * touched, and dc_intq_dropped() says how often.
 *
 * THREADING CONTRACT: single producer, single consumer, no synchronisation in
 * this module and no RTOS header, so the whole ring tests on the host. The
 * transport owns the happens-before edge, exactly as in discord_ackq.h:
 *
 *   - producer (WebSocket task): dc_intq_init() then dc_intq_push() only;
 *   - consumer (bot task): dc_intq_pop(), dc_intq_peek(), dc_intq_depth(),
 *     dc_intq_dropped(), dc_intq_peak() only;
 *   - the consumer may act on an entry only after the producer yielded or the
 *     transport wrapped both calls in one mutex;
 *   - dc_intq_init() returns before either task runs.
 *
 * WIDTH: every field the consumer reads is one naturally aligned 32-bit word,
 * so a racing access is atomic on Xtensa and cannot tear. count is the one
 * field both sides write (producer ++, consumer --) — the same accepted
 * trade-off as dc_ackq; see its header for why the races are expected and why
 * a mutex here would be the wrong answer.
 */
#define DC_INTQ_CAPACITY 4

typedef enum {
    DC_INT_OK = 0,
    DC_INTQ_FULL,
    DC_INTQ_INVALID
} DcIntStatus;

typedef struct {
    /* Largest members first, then the six book-keeping words: 24 B of index
     * and slots[] landing on the struct's 8-byte boundary (AGENT.md §2). */
    DiscordInteraction slots[DC_INTQ_CAPACITY];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t peak;
    uint32_t enqueued;
    uint32_t dropped;
} DcIntq;

void dc_intq_init(DcIntq *q);

/** Copy the whole struct into the ring. Never blocks. */
DcIntStatus dc_intq_push(DcIntq *q, const DiscordInteraction *it);

/** Consume the oldest entry. false when empty; nothing is consumed on false. */
bool dc_intq_pop(DcIntq *q, DiscordInteraction *out);

/** Read the oldest entry without consuming it. out may be NULL to test emptiness. */
bool dc_intq_peek(DcIntq *q, DiscordInteraction *out);

uint32_t dc_intq_depth(const DcIntq *q);
uint32_t dc_intq_free(const DcIntq *q);
uint32_t dc_intq_dropped(const DcIntq *q);
uint32_t dc_intq_peak(const DcIntq *q);
bool dc_intq_full(const DcIntq *q);

/** Static DRAM the ring occupies, measured rather than estimated. */
size_t dc_intq_footprint(void);

#ifdef __cplusplus
}
#endif

#endif
