#ifndef DISCORD_ACKQ_H
#define DISCORD_ACKQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "discord_feature.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bounded ACK queue: 32 slots, oldest shed on overload.
 *
 * THREADING CONTRACT: single producer, single consumer. This module carries no
 * synchronisation of its own and uses no RTOS primitive, so it stays free of
 * ESP-IDF headers and testable on the host. The caller owns the happens-before
 * edge between the two tasks:
 *
 *   - one producer task calls dc_ackq_init() and dc_ackq_push(), nothing else;
 *   - one consumer task calls dc_ackq_pop(), dc_ackq_peek(), dc_ackq_depth(),
 *     dc_ackq_dropped() and dc_ackq_oldest_age_ms(), nothing else;
 *   - the consumer may only act on an entry after the producer has published it,
 *     which means the producer yields (vTaskDelay(0) or a port yield at the end
 *     of the gateway task) or the transport wraps both calls in one mutex;
 *   - dc_ackq_init() must have returned before either task starts running.
 *
 * "Only one consumer" is not what makes this safe. Without the edge above the
 * producer writing head, tail, count, dropped and head_stamp_ms races the
 * consumer reading them. DRAM on the ESP32 is cache-coherent across both
 * cores, so hardware coherence is not the hazard. The hazards are that a
 * C11 data race is undefined behaviour, so the compiler may in principle
 * reorder or tear even word-sized accesses, and that only the width rule
 * below saves this design: every field the consumer reads is a single
 * naturally aligned 32-bit word, which is atomic on Xtensa, so no racing
 * access can tear.
 *
 * WIDTH: every field the consumer reads is a single 32-bit word, so no load can
 * tear. The one exception is the payload DiscordAck, which the consumer copies
 * as a whole after the happens-before edge. The queue book-keeping itself never
 * crosses the edge in two pieces.
 *
 * EXPECTED RACES: a ThreadSanitizer run over the two-task topology still reports
 * races, and that is by design. Lock-free SPSC has no happens-before edge, so TSan
 * reports the C11 data races that persist under the contract above. What the
 * width rule buys is that every racing access is at most 32 bits and naturally
 * aligned, so each is atomic on Xtensa and cannot tear. A TSan run before the
 * width fix showed two 8-byte tear-capable accesses; after it, none. Do not try
 * to make TSan clean here, and do not add a mutex to chase it.
 *
 * TWO WRITERS, ONE FIELD: head_stamp_ms and head_stamped are written by the
 * producer, through mark_entry_live() on the shed path in dc_ackq_push(), and by
 * the consumer, through dc_ackq_oldest_age_ms(). That is why oldest_age_ms takes
 * a non-const DcAckq: it is not a pure read. Under the SPSC edge each side
 * writes at a time, so it stays correct, but a reader expecting a read-only
 * accessor will be surprised.
 *
 * CLOCK: dc_ackq_oldest_age_ms() compares uint32_t milliseconds since boot. The
 * counter wraps at 2^32 ms, about 49.7 days. A backwards step, including that
 * wrap, clamps the age to 0 rather than reporting a huge value, and an ACK
 * expires after 15 minutes, so the wrap is harmless for this queue. Callers that
 * need exact sub-wrap deltas must pass the clock in already truncated to 32 bits.
 *
 * AGE MEASUREMENT: dc_ackq_oldest_age_ms() stamps the head lazily, on
 * the first call that observes a live head, so the age it reports for
 * an entry is measured from its first OBSERVATION, never from its
 * enqueue. The reported age is therefore a lower bound: an entry
 * that sat in the queue before that first observation is older than
 * the value returned, and a 15 minute expiry check built on it can
 * only fire late, never early. The lazy stamp is also why the
 * parameter is a non-const DcAckq *: a const pointer cannot store
 * the stamp, which is the settled Task 3 ruling, so neither the
 * signature nor the behavior changes. Callers must not read the
 * return value as an enqueue age, and the consumer must poll at a
 * rate far below the TTL for the bound to stay tight.
 */

#define DC_ACKQ_CAPACITY 32

#define DC_ACKQ_AGE_WRAP_MS 4294967295u
#define DC_ACKQ_AGE_TTL_MS  900000u

typedef enum {
    DC_ACK_OK = 0,
    DC_ACK_INVALID = -1
} DcAckStatus;

typedef struct {
    DiscordAck slots[DC_ACKQ_CAPACITY];
    uint32_t   head;
    uint32_t   tail;
    uint32_t   count;
    uint32_t   peak;
    uint32_t   enqueued;
    uint32_t   dropped;
    uint32_t   head_stamp_ms;
    bool       head_stamped;
} DcAckq;

void dc_ackq_init(DcAckq *q);
DcAckStatus dc_ackq_push(DcAckq *q, const DiscordAck *ack);
bool dc_ackq_pop(DcAckq *q, DiscordAck *out);
bool dc_ackq_peek(const DcAckq *q, DiscordAck *out);
uint32_t dc_ackq_depth(const DcAckq *q);
uint32_t dc_ackq_dropped(const DcAckq *q);
/**
 * Age of the oldest queued ACK, in milliseconds, measured from the
 * first call that observes a live head, not from enqueue. The value
 * is a lower bound: an entry can be older than reported, so a
 * 15 minute expiry check built on it can only fire late. Takes a
 * non-const DcAckq * because that first call lazily stamps the head;
 * a const pointer cannot store the stamp.
 */
uint32_t dc_ackq_oldest_age_ms(DcAckq *q, uint64_t now_ms);
size_t dc_ackq_footprint(void);

#ifdef __cplusplus
}
#endif

#endif
