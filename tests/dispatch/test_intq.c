/* test_intq.c — host suite for the bounded interaction dispatch ring.
 *
 * Pure C, no FreeRTOS, no ESP-IDF headers: the queue under test carries no
 * synchronisation of its own (AGENT.md §3 SPSC contract, same as dc_ackq), so
 * the whole ring is testable here.
 *
 * What is pinned down, in order of the design's promises:
 *   1. FIFO order, including across ring wrap (head/tail are modular).
 *   2. Depth is never above capacity; push never blocks.
 *   3. Shedding policy is DROP NEWEST: an arrival over capacity is refused and
 *      counted, and no entry already queued is ever evicted. This is the
 *      opposite of dc_ackq (drop oldest) on purpose — a queued interaction is
 *      a handler the user is waiting on, so the extra one is what goes.
 *   4. Counters (enqueued / dropped / peak) are exact, and the book-keeping
 *      invariant enqueued == delivered + dropped + live holds under a
 *      randomised interleaved driver.
 *   5. The ring is reusable after a full drain, for many laps.
 *   6. Payload fidelity: DiscordInteraction is fixed-array only, so a whole
 *      struct copy is memcpy-safe — every field must survive the ring.
 *   7. Static RAM cost, measured not estimated.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "discord_intq.h"

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

static DiscordInteraction mk_int(uint32_t seq)
{
    DiscordInteraction it;
    uint32_t i;

    memset(&it, 0, sizeof(it));
    it.id = (DiscordId)seq;
    it.application_id = (DiscordId)(900000u + seq);
    it.user_id = (DiscordId)(1000u + seq);
    it.channel_id = (DiscordId)(2000u + seq);
    it.guild_id = (DiscordId)(3000u + seq);
    it.message_id = (DiscordId)(4000u + seq);
    it.type = (uint32_t)((seq % 3u) + 2u);
    snprintf(it.command, sizeof(it.command), "cmd-%u", seq);
    if ((seq % 2u) == 0u) {
        snprintf(it.sub, sizeof(it.sub), "sub-%u", seq);
    }
    snprintf(it.custom_id, sizeof(it.custom_id), "cid-%u", seq);
    it.value_count = (uint32_t)(seq % DC_INTERACTION_VALUES);
    for (i = 0; i < it.value_count; i++) {
        snprintf(it.values[i], DC_INTERACTION_VALUE_LEN,
                 "v-%u-%u multi-word \xc3\xa9 value", seq, i);
    }
    snprintf(it.token, sizeof(it.token), "tok-%u", seq);
    return it;
}

static bool same_int(const DiscordInteraction *a, const DiscordInteraction *b)
{
    uint32_t i;

    if (a->id != b->id || a->application_id != b->application_id ||
        a->user_id != b->user_id || a->channel_id != b->channel_id ||
        a->guild_id != b->guild_id || a->message_id != b->message_id ||
        a->type != b->type || a->value_count != b->value_count) {
        return false;
    }
    if (memcmp(a->command, b->command, sizeof(a->command)) != 0 ||
        memcmp(a->sub, b->sub, sizeof(a->sub)) != 0 ||
        memcmp(a->custom_id, b->custom_id, sizeof(a->custom_id)) != 0 ||
        memcmp(a->token, b->token, sizeof(a->token)) != 0) {
        return false;
    }
    for (i = 0; i < DC_INTERACTION_VALUES; i++) {
        if (memcmp(a->values[i], b->values[i],
                   DC_INTERACTION_VALUE_LEN) != 0) {
            return false;
        }
    }
    return true;
}

static size_t payload_floor_bytes(void)
{
    return (size_t)DC_INTQ_CAPACITY * sizeof(DiscordInteraction);
}

/* ------------------------------------------------------------------ */

static void test_push_pop_fifo(void)
{
    DcIntq q;
    DiscordInteraction out;
    bool ok = true;
    uint32_t i;

    dc_intq_init(&q);
    check(dc_intq_depth(&q) == 0u, "a fresh queue is empty");
    check(!dc_intq_pop(&q, &out), "pop on empty returns false");

    for (i = 1u; i <= DC_INTQ_CAPACITY; i++) {
        DiscordInteraction in = mk_int(i);
        ok = ok && (dc_intq_push(&q, &in) == DC_INT_OK);
    }
    check(ok, "a queue-full of pushes are all accepted");
    check(dc_intq_depth(&q) == DC_INTQ_CAPACITY, "depth is capacity");
    check(dc_intq_free(&q) == 0u, "free slots are 0 at capacity");
    check(dc_intq_full(&q), "full reports true at capacity");
    check(!dc_intq_full(NULL), "full(NULL) is false, never a crash");
    check(dc_intq_dropped(&q) == 0u, "nothing dropped while under capacity");

    for (i = 1u; i <= DC_INTQ_CAPACITY; i++) {
        DiscordInteraction want = mk_int(i);
        if (!dc_intq_pop(&q, &out)) {
            ok = false;
            break;
        }
        if (out.id != (DiscordId)i || !same_int(&out, &want)) {
            ok = false;
        }
    }
    check(ok, "drain yields the pushes in FIFO order, every field intact");
    check(dc_intq_depth(&q) == 0u, "queue empty after the drain");
    check(dc_intq_free(&q) == DC_INTQ_CAPACITY, "all slots free again after the "
                                                "drain");
}

static void test_fifo_across_wrap(void)
{
    DcIntq q;
    DiscordInteraction out;
    bool order_ok = true;
    uint32_t lap;
    uint32_t i;

    dc_intq_init(&q);

    /* Six laps of capacity, popping everything each lap: head and tail walk
     * the whole ring, so every slot serves as head and as tail. */
    for (lap = 0u; lap < 6u; lap++) {
        for (i = 1u; i <= DC_INTQ_CAPACITY; i++) {
            DiscordInteraction in = mk_int(lap * 100u + i);
            if (dc_intq_push(&q, &in) != DC_INT_OK) {
                order_ok = false;
            }
        }
        for (i = 1u; i <= DC_INTQ_CAPACITY; i++) {
            DiscordInteraction want = mk_int(lap * 100u + i);
            if (!dc_intq_pop(&q, &out) || !same_int(&out, &want)) {
                order_ok = false;
            }
        }
    }
    check(order_ok, "FIFO holds across six laps of ring wrap");
    check(dc_intq_depth(&q) == 0u, "empty after the wrapped laps");
    check(q.peak == DC_INTQ_CAPACITY, "peak reached capacity and no further");

    /* Partial drain then refill: the classic wrap boundary. */
    for (i = 1u; i <= DC_INTQ_CAPACITY; i++) {
        DiscordInteraction in = mk_int(900u + i);
        dc_intq_push(&q, &in);
    }
    for (i = 0u; i < 3u; i++) {
        dc_intq_pop(&q, &out);
    }
    for (i = 1u; i <= 3u; i++) {
        DiscordInteraction in = mk_int(950u + i);
        dc_intq_push(&q, &in);
    }
    order_ok = true;
    {
        static const uint32_t want_ids[] = { 904u, 951u, 952u, 953u };
        uint32_t k;

        for (k = 0u; k < 4u; k++) {
            if (!dc_intq_pop(&q, &out) || out.id != (DiscordId)want_ids[k]) {
                order_ok = false;
            }
        }
    }
    check(order_ok, "refill after a partial drain appends behind the tail, "
                    "across the ring boundary");
    check(dc_intq_depth(&q) == 0u, "empty after the boundary test");
}

static void test_peek_does_not_consume(void)
{
    DcIntq q;
    DiscordInteraction out;
    DiscordInteraction head = mk_int(7u);
    DiscordInteraction second = mk_int(8u);

    dc_intq_init(&q);
    check(!dc_intq_peek(&q, &out), "peek on empty returns false");
    check(!dc_intq_peek(&q, NULL), "peek(NULL out) on empty returns false");

    dc_intq_push(&q, &head);
    check(dc_intq_peek(&q, NULL), "peek(NULL out) on live head returns true "
                                  "without writing anywhere");
    check(dc_intq_peek(&q, &out), "peek on non-empty succeeds");
    check(same_int(&out, &head), "peek returns the head payload");
    check(dc_intq_depth(&q) == 1u, "peek did not consume");
    check(dc_intq_dropped(&q) == 0u, "peek is not a shed");

    dc_intq_push(&q, &second);
    check(dc_intq_peek(&q, &out) && out.id == 7u, "peek reads the head, "
                                                  "not the newest push");
    check(dc_intq_pop(&q, &out) && out.id == 7u, "pop after peek yields the "
                                                 "same entry");
    check(dc_intq_peek(&q, &out) && out.id == 8u, "the next entry becomes head");
    check(dc_intq_depth(&q) == 1u, "only the popped entry left the queue");
}

static void test_drop_newest_at_capacity(void)
{
    DcIntq q;
    DiscordInteraction out;
    uint32_t i;
    bool kept_ok = true;

    dc_intq_init(&q);
    for (i = 1u; i <= DC_INTQ_CAPACITY; i++) {
        DiscordInteraction in = mk_int(i);
        dc_intq_push(&q, &in);
    }
    check(dc_intq_depth(&q) == DC_INTQ_CAPACITY, "queue is at capacity");

    for (i = 100u; i < 200u; i++) {
        DiscordInteraction in = mk_int(i);
        if (dc_intq_push(&q, &in) != DC_INTQ_FULL) {
            kept_ok = false;
        }
    }
    check(kept_ok, "every over-capacity push is refused, none blocks");
    check(dc_intq_depth(&q) == DC_INTQ_CAPACITY, "refusals did not grow depth");
    check(dc_intq_dropped(&q) == 100u, "every refusal counted as a drop");
    check(q.enqueued == 104u, "enqueued counts arrivals, refused ones too");
    check(q.peak == DC_INTQ_CAPACITY, "a shed never raises peak, depth did not "
                                      "grow");

    /* The heart of the policy: the 100 refused arrivals left the queued four
     * untouched, so draining yields the ORIGINAL interactions, in order. A
     * drop-oldest ring would have evicted them. */
    for (i = 1u; i <= DC_INTQ_CAPACITY; i++) {
        DiscordInteraction want = mk_int(i);
        if (!dc_intq_pop(&q, &out) || out.id != (DiscordId)i ||
            !same_int(&out, &want)) {
            kept_ok = false;
        }
    }
    check(kept_ok, "the four queued interactions survived 100 over-capacity "
                   "arrivals untouched and in order");
    check(dc_intq_depth(&q) == 0u, "drained after the shed storm");
}

static void test_space_freed_is_reusable(void)
{
    DcIntq q;
    DiscordInteraction out;
    bool ok = true;
    uint32_t i;

    dc_intq_init(&q);
    for (i = 1u; i <= DC_INTQ_CAPACITY; i++) {
        DiscordInteraction in = mk_int(i);
        dc_intq_push(&q, &in);
    }
    /* Drain one to make room, then the very arrival that was refused fits. */
    check(dc_intq_pop(&q, &out) && out.id == 1u, "one entry drained");
    {
        DiscordInteraction late = mk_int(500u);
        check(dc_intq_push(&q, &late) == DC_INT_OK,
              "a slot freed by a pop is immediately reusable");
    }
    check(dc_intq_depth(&q) == DC_INTQ_CAPACITY, "depth back at capacity");

    /* And after a total drain the ring restarts clean: same ids, same order. */
    while (dc_intq_pop(&q, &out)) {
    }
    check(dc_intq_depth(&q) == 0u, "fully drained");
    for (i = 1u; i <= DC_INTQ_CAPACITY; i++) {
        DiscordInteraction in = mk_int(i);
        ok = ok && (dc_intq_push(&q, &in) == DC_INT_OK);
    }
    for (i = 1u; i <= DC_INTQ_CAPACITY; i++) {
        if (!dc_intq_pop(&q, &out) || out.id != (DiscordId)i) {
            ok = false;
        }
    }
    check(ok, "after a full drain the ring accepts and replays a whole lap");
    check(q.peak == DC_INTQ_CAPACITY, "peak is a lifetime high-water mark");
    check(dc_intq_dropped(&q) == 0u, "no drops were incurred by the reuse test");
}

static void test_counters_exact(void)
{
    DcIntq q;
    DiscordInteraction out;
    uint32_t i;
    uint32_t popped = 0u;

    dc_intq_init(&q);
    check(q.enqueued == 0u && q.dropped == 0u && q.peak == 0u,
          "init zeroes every counter");

    for (i = 0u; i < 10u; i++) {
        DiscordInteraction in = mk_int(i + 1u);
        dc_intq_push(&q, &in);
        if (i >= 2u) {
            dc_intq_pop(&q, &out);
            popped++;
        }
    }
    check(dc_intq_depth(&q) == 2u, "10 pushes and 8 pops leave depth 2");
    check(q.enqueued == 10u, "enqueued counts exactly the 10 pushes");
    check(q.dropped == 0u, "dropped stays 0 through an unbounded run");
    check(q.peak == 3u, "peak is exactly the 3-deep high-water mark, not the "
                        "final 2");
    check(q.enqueued == popped + q.dropped + dc_intq_depth(&q),
          "book-keeping invariant: arrivals == delivered + dropped + live");

    /* Pops must not disturb the lifetime counters. */
    while (dc_intq_pop(&q, &out)) {
        popped++;
    }
    check(q.enqueued == 10u && q.dropped == 0u && q.peak == 3u,
          "draining never rewrites enqueued, dropped or peak");
    check(q.enqueued == popped + q.dropped + dc_intq_depth(&q),
          "the invariant still holds at zero depth");
}

static uint32_t rng_state = 2463534242u;

static uint32_t rng(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 8;
}

static void test_interleaved_counters(void)
{
    DcIntq q;
    DiscordInteraction out;
    uint32_t pushed = 0u;
    uint32_t popped = 0u;
    uint32_t sheds = 0u;
    uint32_t max_depth = 0u;
    bool ops_ok = true;
    bool bounded = true;
    bool real_entries = true;
    uint32_t step;

    dc_intq_init(&q);
    for (step = 0u; step < 20000u; step++) {
        uint32_t before = dc_intq_depth(&q);
        uint32_t choice = rng() % 100u;

        if (choice < 70u) {
            DiscordInteraction in = mk_int(pushed + 1u);
            DcIntStatus st = dc_intq_push(&q, &in);

            pushed++;
            if (st == DC_INTQ_FULL) {
                sheds++;
                if (dc_intq_depth(&q) != before) {
                    bounded = false;
                }
            } else if (st != DC_INT_OK) {
                ops_ok = false;
            }
        } else if (dc_intq_pop(&q, &out)) {
            popped++;
            if (out.id == 0u) {
                real_entries = false;
            }
        }
        if (dc_intq_depth(&q) > max_depth) {
            max_depth = dc_intq_depth(&q);
        }
    }

    check(ops_ok, "20000 interleaved calls only ever returned OK or FULL");
    check(sheds > 1000u, "the interleaved driver really did hit the ceiling "
                         "hundreds of times");
    check(bounded, "a shed never changed the depth");
    check(max_depth == DC_INTQ_CAPACITY, "depth peaked exactly at capacity, "
                                         "never above");
    check(real_entries, "a pop always yielded a real payload, never stale "
                        "memory");
    check(q.dropped == sheds, "every shed was counted, none silent");
    check(q.enqueued == pushed, "enqueued counts all 20000 arrivals");
    check(pushed == popped + q.dropped + dc_intq_depth(&q),
          "arrivals == delivered + dropped + live after the storm");
    check(q.peak == DC_INTQ_CAPACITY, "peak is exactly capacity");

    while (dc_intq_pop(&q, &out)) {
        popped++;
    }
    check(pushed == popped + q.dropped,
          "final drain: every arrival is either delivered or dropped, nothing "
          "lost twice");
    check(dc_intq_depth(&q) == 0u, "queue empty at the very end");
}

static void test_invalid_pushes_refused(void)
{
    DcIntq q;
    DiscordInteraction a = mk_int(11u);
    DiscordInteraction no_id;
    DiscordInteraction out;

    dc_intq_init(&q);
    no_id = mk_int(12u);
    no_id.id = 0u;

    check(dc_intq_push(NULL, &a) == DC_INTQ_INVALID, "NULL queue refused");
    check(dc_intq_push(&q, NULL) == DC_INTQ_INVALID, "NULL interaction refused");
    check(dc_intq_push(&q, &no_id) == DC_INTQ_INVALID,
          "zero-id interaction refused");
    check(dc_intq_depth(&q) == 0u, "the three refusals stored nothing");
    check(dc_intq_dropped(&q) == 0u, "a refused argument is not a shed");
    check(q.enqueued == 0u, "a refused argument is not an arrival");

    check(dc_intq_push(&q, &a) == DC_INT_OK,
          "a valid push is accepted after the refusals");
    check(dc_intq_depth(&q) == 1u, "only the valid push stored");
    check(dc_intq_pop(&q, &out) && same_int(&out, &a),
          "the real queue still pops its own entry");

    check(dc_intq_pop(NULL, &out) == false, "pop(NULL, out) refused");
    check(dc_intq_pop(&q, NULL) == false, "pop with NULL out refuses to "
                                          "consume anything");
    check(dc_intq_depth(&q) == 0u, "a refused pop did not consume");
    check(dc_intq_peek(NULL, &out) == false, "peek(NULL, out) refused");
    check(dc_intq_depth(NULL) == 0u, "depth(NULL) is 0");
    check(dc_intq_dropped(NULL) == 0u, "dropped(NULL) is 0");
    check(dc_intq_peak(NULL) == 0u, "peak(NULL) is 0");
    check(DC_INTQ_CAPACITY == 4, "capacity is exactly 4 slots: the ring is a "
                                 "burst absorber, not a backlog");
}

static void test_init_null_and_zeroed(void)
{
    DcIntq q;

    dc_intq_init(NULL);
    check(true, "init(NULL) returns instead of writing through a NULL pointer");

    memset(&q, 0xA5, sizeof(q));
    dc_intq_init(&q);
    check(q.head == 0u && q.tail == 0u && q.count == 0u,
          "init resets head, tail and count on dirty memory");
    check(q.peak == 0u && q.enqueued == 0u && q.dropped == 0u,
          "init resets every telemetry counter on dirty memory");
    check(dc_intq_depth(&q) == 0u, "a re-init over dirty memory reads empty");
    {
        DiscordInteraction out;
        check(!dc_intq_pop(&q, &out), "a re-init queue pops nothing, it is not "
                                      "fooled by 0xA5 residue");
    }
}

static void test_static_sizes(void)
{
    DcIntq q;
    size_t footprint = dc_intq_footprint();
    size_t floor = payload_floor_bytes();
    size_t bookkeeping = footprint - floor;

    memset(&q, 0, sizeof(q));

    /* Printed, not estimated: the orchestrator needs the real DRAM number. */
    printf("\nSTATIC RAM (measured with sizeof on the host, same types as the "
           "target)\n");
    printf("  DiscordInteraction  %6zu B  (fixed arrays only, memcpy-safe)\n",
           sizeof(DiscordInteraction));
    printf("  DcIntq              %6zu B  (%d slots x %zu B + %zu B index)\n",
           footprint, DC_INTQ_CAPACITY, sizeof(DiscordInteraction),
           bookkeeping);
    printf("  payload floor       %6zu B\n", floor);
    printf("  per slot amortised  %8.2f B\n", (double)footprint /
           (double)DC_INTQ_CAPACITY);

    check(sizeof(DiscordInteraction) == 1720u,
          "DiscordInteraction measures 1720 B (six 64-bit ids, type, command "
          "32, sub 32, custom_id 64, value_count, values[5][256], token 256)");
    check(footprint == sizeof(DcIntq), "footprint is the whole queue struct");
    check(bookkeeping == 24u, "index and telemetry measure 24 B: six 32-bit "
                             "words, no alignment padding");
    check(footprint >= floor, "footprint covers all four payloads");
    check(footprint > floor, "the index is counted, not hidden");
    check(footprint <= 8192u, "the dispatch ring fits 8 KiB of DRAM");
    check(offsetof(DcIntq, slots) % 8u == 0u,
          "the payload array starts on the struct's natural boundary");

    /* SPSC width rule (AGENT.md §3 / discord_ackq.h): everything the reader
     * touches is one naturally aligned 32-bit word, so no racing access can
     * tear on Xtensa. */
    check(sizeof(q.head) == 4u, "head is one 32-bit word");
    check(sizeof(q.tail) == 4u, "tail is one 32-bit word");
    check(sizeof(q.count) == 4u, "count is one 32-bit word");
    check(sizeof(q.peak) == 4u, "peak is one 32-bit word");
    check(sizeof(q.enqueued) == 4u, "enqueued is one 32-bit word");
    check(sizeof(q.dropped) == 4u, "dropped is one 32-bit word");
    check((offsetof(DcIntq, head) % 4u) == 0u &&
          (offsetof(DcIntq, dropped) % 4u) == 0u,
          "every book-keeping word is naturally aligned");
}

int main(void)
{
    test_push_pop_fifo();
    test_fifo_across_wrap();
    test_peek_does_not_consume();
    test_drop_newest_at_capacity();
    test_space_freed_is_reusable();
    test_counters_exact();
    test_interleaved_counters();
    test_invalid_pushes_refused();
    test_init_null_and_zeroed();
    test_static_sizes();

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
