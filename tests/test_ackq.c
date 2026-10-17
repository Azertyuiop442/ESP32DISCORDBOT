#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "discord_ackq.h"

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

static DiscordAck mk_ack(uint32_t seq, uint8_t kind)
{
    DiscordAck a;

    memset(&a, 0, sizeof(a));
    a.id = (DiscordId)seq;
    a.kind = kind;
    snprintf(a.token, sizeof(a.token), "tok-%u", seq);
    a.deferred = (seq % 2u) == 0u;
    a.ephemeral = (seq % 3u) == 0u;

    return a;
}

static uint8_t kind_for(uint32_t seq)
{
    return (uint8_t)((seq % 6u) + 1u);
}

static size_t payload_floor_bytes(void)
{
    return (size_t)DC_ACKQ_CAPACITY * sizeof(DiscordAck);
}

static bool same_ack(const DiscordAck *a, const DiscordAck *b)
{
    return a->id == b->id && a->kind == b->kind && a->deferred == b->deferred &&
           a->ephemeral == b->ephemeral && strcmp(a->token, b->token) == 0;
}

static uint32_t rng_state = 2463534242u;

static uint32_t rng(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 8;
}

static void test_capacity_and_shedding(void)
{
    DcAckq q;
    dc_ackq_init(&q);

    for (uint32_t i = 0; i < DC_ACKQ_CAPACITY; i++) {
        DiscordAck a = { .id = i + 1, .kind = 4 };
        check(dc_ackq_push(&q, &a) == DC_ACK_OK, "push below capacity");
    }
    check(dc_ackq_depth(&q) == DC_ACKQ_CAPACITY, "queue full");
    check(dc_ackq_dropped(&q) == 0, "nothing dropped yet");

    DiscordAck overflow = { .id = 9999, .kind = 4 };
    check(dc_ackq_push(&q, &overflow) == DC_ACK_OK, "over-capacity push sheds, never blocks");
    check(dc_ackq_depth(&q) == DC_ACKQ_CAPACITY, "depth unchanged after shedding");
    check(dc_ackq_dropped(&q) == 1, "drop counted");

    DiscordAck head;
    check(dc_ackq_pop(&q, &head), "pop succeeds");
    check(head.id == 2, "oldest entry was shed, not the newest");
}

static void test_expired_ack_is_dead(void)
{
    DcAckq q;
    dc_ackq_init(&q);

    DiscordAck fresh = { .id = 1, .kind = 4, .deferred = false };
    dc_ackq_push(&q, &fresh);
    check(dc_ackq_oldest_age_ms(&q, 1000) == 0, "age starts at enqueue");

    check(dc_ackq_oldest_age_ms(&q, 3000) == 2000, "age tracks the clock");

    check(dc_ackq_pop(&q, NULL) == false, "NULL out refused");
    check(dc_ackq_depth(&q) == 1, "refused pop did not consume");
}

static void test_payload_survives_the_ring(void)
{
    DcAckq q;
    DiscordAck out;
    bool order_ok = true;
    bool fields_ok = true;

    dc_ackq_init(&q);

    for (uint32_t i = 1; i <= DC_ACKQ_CAPACITY; i++) {
        DiscordAck a = mk_ack(i, kind_for(i));
        if (dc_ackq_push(&q, &a) != DC_ACK_OK) {
            fields_ok = false;
        }
    }
    check(fields_ok, "a full lap of distinct payloads is accepted");

    for (uint32_t i = 1; i <= DC_ACKQ_CAPACITY; i++) {
        DiscordAck want = mk_ack(i, kind_for(i));

        if (!dc_ackq_pop(&q, &out) || out.id != (DiscordId)i) {
            order_ok = false;
        }
        if (!same_ack(&out, &want)) {
            fields_ok = false;
        }
    }
    check(order_ok, "one full lap drains in FIFO order");
    check(fields_ok, "id, kind, deferred and token all survive the ring");
    check(dc_ackq_depth(&q) == 0, "queue empty after draining every slot");
    check(dc_ackq_dropped(&q) == 0, "draining lost nothing");
}

static void test_wrap_after_shedding(void)
{
    DcAckq q;
    DiscordAck out;
    bool pushed_ok = true;
    bool order_ok = true;

    dc_ackq_init(&q);

    for (uint32_t lap = 0; lap < 2u; lap++) {
        for (uint32_t i = 1; i <= DC_ACKQ_CAPACITY; i++) {
            DiscordAck a = mk_ack((lap + 1u) * 1000u + i, 1);
            if (dc_ackq_push(&q, &a) != DC_ACK_OK) {
                pushed_ok = false;
            }
        }
    }
    check(pushed_ok, "32 over-capacity pushes all shed instead of blocking");
    check(dc_ackq_depth(&q) == DC_ACKQ_CAPACITY, "depth still bounded after 32 sheds");
    check(dc_ackq_dropped(&q) == DC_ACKQ_CAPACITY, "every shed counted");

    for (uint32_t i = 1; i <= DC_ACKQ_CAPACITY; i++) {
        DiscordAck want = mk_ack(2000u + i, 1);
        if (!dc_ackq_pop(&q, &out) || !same_ack(&out, &want)) {
            order_ok = false;
        }
    }
    check(order_ok, "the surviving lap is intact and in order after the wrap");
    check(dc_ackq_depth(&q) == 0, "queue empty after the second lap");
}

static void test_invalid_pushes_are_refused(void)
{
    DcAckq q;
    DiscordAck a = mk_ack(1, 2);
    DiscordAck zero_id = { .id = 0, .kind = 4 };
    DiscordAck zero_kind = { .id = 1, .kind = 0 };

    dc_ackq_init(&q);

    check(dc_ackq_push(NULL, &a) == DC_ACK_INVALID, "NULL queue refused");
    check(dc_ackq_push(&q, NULL) == DC_ACK_INVALID, "NULL ack refused");
    check(dc_ackq_depth(&q) == 0, "refused push stored nothing");

    check(dc_ackq_push(&q, &zero_id) == DC_ACK_INVALID, "zero id refused");
    check(dc_ackq_push(&q, &zero_kind) == DC_ACK_INVALID, "kind zero refused");
    check(dc_ackq_depth(&q) == 0, "both refusals stored nothing");
    check(dc_ackq_dropped(&q) == 0, "a refused argument is not a shed");

    check(dc_ackq_push(&q, &a) == DC_ACK_OK, "valid push accepted after refusals");
    check(dc_ackq_depth(&q) == 1, "only the valid push stored");
}

static void test_age_is_the_oldest_live_entry(void)
{
    DcAckq q;
    DiscordAck out;

    dc_ackq_init(&q);
    check(dc_ackq_oldest_age_ms(&q, 5000) == 0, "empty queue has no age");

    DiscordAck first = mk_ack(1, 1);
    DiscordAck second = mk_ack(2, 1);
    dc_ackq_push(&q, &first);
    check(dc_ackq_oldest_age_ms(&q, 5000) == 0, "first observation stamps the head, age 0");
    check(dc_ackq_oldest_age_ms(&q, 8000) == 3000, "age accumulates from the stamp");

    dc_ackq_push(&q, &second);
    check(dc_ackq_oldest_age_ms(&q, 8000) == 3000, "age reads the head, not the newest push");

    check(dc_ackq_pop(&q, &out) && out.id == first.id, "head popped");
    check(dc_ackq_oldest_age_ms(&q, 8000) == 0, "the new head is re-stamped, age 0");
    check(dc_ackq_oldest_age_ms(&q, 11000) == 3000, "the new head accumulates from its own stamp");
    check(dc_ackq_oldest_age_ms(&q, 250) == 0, "clock going backwards clamps to zero");
    check(dc_ackq_oldest_age_ms(&q, 0) == 0, "zero clock clamps to zero");

    check(dc_ackq_pop(&q, &out) && out.id == second.id, "second head popped");
    check(dc_ackq_oldest_age_ms(&q, 20000) == 0, "an emptied queue has no age");

    DiscordAck third = mk_ack(3, 1);
    dc_ackq_push(&q, &third);
    check(dc_ackq_oldest_age_ms(&q, 20000) == 0, "a refilled head starts unobserved");
    check(dc_ackq_oldest_age_ms(&q, 24500) == 4500, "and accumulates from that observation");
}

static void test_peek_does_not_consume(void)
{
    DcAckq q;
    DiscordAck out;
    DiscordAck peeked = mk_ack(7, 3);

    dc_ackq_init(&q);

    check(!dc_ackq_peek(&q, &out), "peek on empty returns false");
    check(!dc_ackq_peek(&q, NULL), "peek with NULL out on empty returns false");

    dc_ackq_push(&q, &peeked);

    check(dc_ackq_peek(&q, &out), "peek on non-empty succeeds");
    check(out.id == peeked.id, "peek returns the head");
    check(dc_ackq_depth(&q) == 1, "peek did not consume");
    check(dc_ackq_dropped(&q) == 0, "peek did not count as a shed");
    check(dc_ackq_pop(&q, &out) && same_ack(&out, &peeked), "pop after peek yields the same entry");
}

static void test_null_guards(void)
{
    DcAckq q;
    DiscordAck out;
    DiscordAck a = mk_ack(11, 2);

    dc_ackq_init(NULL);
    check(true, "init(NULL) returns instead of writing through a NULL pointer");

    check(!dc_ackq_pop(NULL, &out), "pop(NULL, out) refused");
    check(!dc_ackq_peek(NULL, &out), "peek(NULL, out) refused");
    check(!dc_ackq_peek(NULL, NULL), "peek(NULL, NULL) refused");
    check(dc_ackq_depth(NULL) == 0, "depth(NULL) is 0");
    check(dc_ackq_dropped(NULL) == 0, "dropped(NULL) is 0");
    check(dc_ackq_oldest_age_ms(NULL, 12345) == 0, "oldest_age_ms(NULL, now) is 0");

    dc_ackq_init(&q);
    dc_ackq_push(&q, &a);
    check(dc_ackq_depth(NULL) == 0, "depth(NULL) stays 0 while the real queue holds one entry");
    check(dc_ackq_dropped(NULL) == 0, "dropped(NULL) stays 0 while the real queue is clean");
    check(dc_ackq_oldest_age_ms(NULL, 12345) == 0,
          "oldest_age_ms(NULL) cannot stamp or disturb the real queue");
    check(dc_ackq_depth(&q) == 1, "the refused NULL calls left the real queue untouched");
    check(dc_ackq_pop(&q, &out) && out.id == a.id, "the real queue still pops its own entry");
}

static void test_counters_are_exact(void)
{
    DcAckq q;
    DiscordAck out;
    bool push_ok = true;

    dc_ackq_init(&q);

    for (uint32_t i = 0; i < DC_ACKQ_CAPACITY; i++) {
        DiscordAck a = mk_ack(i + 1, 4);
        if (dc_ackq_push(&q, &a) != DC_ACK_OK) {
            push_ok = false;
        }
    }
    check(push_ok, "32 pushes accepted");
    check(q.peak == 32, "peak is exactly 32 after filling the queue");
    check(q.enqueued == 32, "enqueued is exactly 32 after 32 pushes");

    DiscordAck overflow = mk_ack(9000, 4);
    dc_ackq_push(&q, &overflow);
    check(q.enqueued == 33, "enqueued counts a shed push, it is still an arrival");
    check(q.dropped == 1, "dropped is exactly 1 after one shed");
    check(q.peak == 32, "a shed does not raise peak, the depth never grew");
    check(q.count == 32, "depth is still 32 after the shed");

    dc_ackq_pop(&q, &out);
    check(q.count == 31, "a pop lowers depth");
    check(q.peak == 32, "a pop never lowers peak, the high-water mark is history");

    while (dc_ackq_pop(&q, &out)) {
    }
    check(q.count == 0, "drained");
    check(q.peak == 32, "draining to empty still leaves peak at 32");
    check(q.enqueued == 33, "popping never changes enqueued");

    for (uint32_t i = 0; i < 5u; i++) {
        DiscordAck a = mk_ack(5000u + i, 4);
        dc_ackq_push(&q, &a);
    }
    check(q.peak == 32, "a refill to 5 does not lower the earlier peak of 32");
    check(q.enqueued == 38, "enqueued is exactly 38 after 33 plus 5 pushes");

    for (uint32_t i = 0; i < 40u; i++) {
        DiscordAck a = mk_ack(6000u + i, 4);
        dc_ackq_push(&q, &a);
    }
    check(q.count == 32, "40 pushes onto 5 entries land at depth 32");
    check(q.peak == 32, "peak is still exactly 32 after the burst");
    check(q.enqueued == 78, "enqueued is exactly 78 after 38 plus 40 pushes");
    check(q.dropped == 14, "dropped is exactly 14: 1 earlier plus 13 of the burst");

    while (dc_ackq_pop(&q, &out)) {
    }
    check(q.count == 0, "drained again");
    check(q.enqueued == 78, "enqueued is a lifetime counter, drains do not reset it");
    check(q.dropped == 14, "dropped is a lifetime counter, drains do not reset it");
    check(q.peak == 32, "peak survives a full drain at its high-water mark");
}

static void test_shed_restarts_the_age_clock(void)
{
    DcAckq q;
    DiscordAck out;

    dc_ackq_init(&q);

    DiscordAck a = mk_ack(1, 4);
    dc_ackq_push(&q, &a);
    check(dc_ackq_oldest_age_ms(&q, 1000) == 0, "head A observed at t=1000");
    check(dc_ackq_oldest_age_ms(&q, 2000) == 1000, "head A ages to 1000 ms");

    for (uint32_t i = 0; i < 31u; i++) {
        DiscordAck b = mk_ack(i + 2u, 4);
        dc_ackq_push(&q, &b);
    }
    check(dc_ackq_depth(&q) == DC_ACKQ_CAPACITY, "a burst of 31 more fills the queue");
    check(dc_ackq_oldest_age_ms(&q, 100000) == 99000, "head A is still the head before the shed");

    DiscordAck shedder = mk_ack(9000, 4);
    check(dc_ackq_push(&q, &shedder) == DC_ACK_OK, "the push that sheds A");
    check(dc_ackq_depth(&q) == DC_ACKQ_CAPACITY, "the shed kept depth at capacity");
    check(dc_ackq_dropped(&q) == 1, "the shed was counted");

    check(dc_ackq_oldest_age_ms(&q, 101000) == 0,
          "after a shed the new head has no stale stamp, age restarts at 0");
    check(dc_ackq_oldest_age_ms(&q, 102000) == 1000,
          "the fresh burst ages 1 s from the shed, not 101 s from the shed entry");
    check(dc_ackq_oldest_age_ms(&q, 101000) == 0,
          "the clock going back to the shed instant clamps to zero, not to a huge age");
    check(dc_ackq_oldest_age_ms(&q, 101000 + DC_ACKQ_AGE_TTL_MS) == DC_ACKQ_AGE_TTL_MS,
          "a freshly shed burst reaches the 15 minute expiry window from the shed, not before");
    check(dc_ackq_oldest_age_ms(&q, 101000 + DC_ACKQ_AGE_TTL_MS) <= DC_ACKQ_AGE_TTL_MS,
          "a freshly shed burst is not falsely declared expired");

    DiscordAck again = mk_ack(9100, 4);
    dc_ackq_push(&q, &again);
    check(dc_ackq_oldest_age_ms(&q, 500000) == 0, "a second shed re-stamps the head too");
    check(dc_ackq_oldest_age_ms(&q, 501000) == 1000, "the head is 1 s old after the second shed");

    check(dc_ackq_pop(&q, &out) && out.id == 3, "the second shed advanced the head again");
}

static void test_age_interleaved_with_sheds(void)
{
    DcAckq q;
    DiscordAck out;
    uint64_t now = 0;
    uint64_t head_changed_at = 0;
    uint32_t model_stamp = 0;
    bool model_stamped = false;
    bool age_ok = true;
    bool bound_ok = true;
    bool expiry_ok = true;
    uint32_t ops_ok = 1;
    uint32_t pushed = 0;
    uint32_t sheds = 0;

    dc_ackq_init(&q);

    for (uint32_t step = 0; step < 40000u; step++) {
        now += 1u + (rng() % 4000u);
        uint32_t choice = rng() % 100u;
        uint32_t before = q.count;

        if (choice < 55u) {
            DiscordAck a = mk_ack(pushed + 1u, 4);
            if (dc_ackq_push(&q, &a) != DC_ACK_OK) {
                ops_ok = 0;
            }
            pushed++;
            if (q.count <= before && before == DC_ACKQ_CAPACITY) {
                sheds++;
                head_changed_at = now;
                model_stamped = false;
            }
        } else if (q.count > 0u) {
            if (!dc_ackq_pop(&q, &out)) {
                ops_ok = 0;
            } else {
                head_changed_at = now;
                model_stamped = false;
            }
        }

        uint32_t age = dc_ackq_oldest_age_ms(&q, now);
        uint32_t clock = (uint32_t)(now & DC_ACKQ_AGE_WRAP_MS);
        uint32_t want;

        if (q.count == 0u) {
            want = 0u;
            model_stamped = false;
        } else if (!model_stamped) {
            model_stamp = clock;
            model_stamped = true;
            want = 0u;
        } else {
            want = clock > model_stamp ? clock - model_stamp : 0u;
        }

        if (age != want) {
            age_ok = false;
        }
        if (age > now - head_changed_at) {
            bound_ok = false;
        }
        if (q.count == DC_ACKQ_CAPACITY && age > DC_ACKQ_AGE_TTL_MS) {
            expiry_ok = false;
        }
    }

    check(ops_ok == 1, "40000 interleaved pushes and pops all succeeded");
    check(sheds > 100, "the interleaved driver actually shed hundreds of times");
    check(age_ok, "every observed age matches a reference model of the stamp, through every shed");
    check(bound_ok, "no observed age ever exceeded the time since the head last changed");
    check(expiry_ok, "a full queue never reported a head older than the 15 minute expiry window");
    check(q.count == DC_ACKQ_CAPACITY, "the interleaved driver ended at capacity");
    check(q.dropped == sheds, "every interleaved shed was counted, none was silent");
}

static void test_clock_wraps_without_a_false_expiry(void)
{
    DcAckq q;
    DiscordAck a = mk_ack(1, 4);
    uint64_t base = 46ull * 24ull * 60ull * 60ull * 1000ull;
    uint64_t near = 3940000000ull;

    dc_ackq_init(&q);
    dc_ackq_push(&q, &a);

    check(base + near > (uint64_t)DC_ACKQ_AGE_WRAP_MS,
          "the test clock really does cross the 32 bit wrap");

    check(dc_ackq_oldest_age_ms(&q, base + near) == 0, "the wrap clamps to zero");
    check(dc_ackq_oldest_age_ms(&q, base + near + 1000ull) == 1000,
          "age accumulates correctly on the far side of the wrap");
    check(dc_ackq_oldest_age_ms(&q, base + near + 1000ull - 5000ull) == 0,
          "a small backwards step across the wrap clamps to zero, never to a huge age");

    dc_ackq_push(&q, &a);
    check(dc_ackq_oldest_age_ms(&q, base + near + 1000ull - 5000ull) == 0,
          "the wrap does not hand a pre-wrap entry a 49 day age");
    check(dc_ackq_oldest_age_ms(&q, base + near + 2000ull) == 2000,
          "and the next step forward ages normally past the wrap");
}

static void test_sustained_overload_stays_bounded(void)
{
    DcAckq q;
    DiscordAck out;
    uint32_t refused = 0;
    uint32_t sheds = 0;
    uint32_t popped = 0;
    uint32_t max_depth = 0;
    bool real_entries = true;

    dc_ackq_init(&q);

    for (uint32_t i = 1; i <= 50000u; i++) {
        DiscordAck a = mk_ack(i, 4);
        uint32_t before = dc_ackq_depth(&q);

        if (dc_ackq_push(&q, &a) != DC_ACK_OK) {
            refused++;
        }
        if (dc_ackq_depth(&q) <= before) {
            sheds++;
        }
        if (dc_ackq_depth(&q) > max_depth) {
            max_depth = dc_ackq_depth(&q);
        }
        if ((i % 2u) == 0u && dc_ackq_pop(&q, &out)) {
            popped++;
            if (out.id == 0) {
                real_entries = false;
            }
        }
    }

    check(refused == 0, "50000 pushes, none refused and none blocked");
    check(max_depth <= DC_ACKQ_CAPACITY, "depth never exceeds capacity");
    check(dc_ackq_dropped(&q) == sheds, "every push that did not grow the queue was a shed");
    check(dc_ackq_dropped(&q) + dc_ackq_depth(&q) + popped == 50000u,
          "every push is accounted for as live or shed");
    check(real_entries, "overload pops always yield a real entry, never stale memory");

    uint32_t live_before_drain = dc_ackq_depth(&q);
    uint32_t total_pops = popped;

    while (dc_ackq_pop(&q, &out)) {
        total_pops++;
    }
    check(total_pops == popped + live_before_drain,
          "draining yields exactly the entries still held, no more, no less");
    check(dc_ackq_depth(&q) == 0, "queue empty after the final drain");
    check(dc_ackq_dropped(&q) + total_pops == 50000u,
          "shed plus delivered accounts for every push, with nothing lost");
    check(q.enqueued == 50000u, "enqueued counts every one of the 50000 pushes");
    check(q.peak == DC_ACKQ_CAPACITY, "peak reached capacity and no further");
    check(dc_ackq_oldest_age_ms(&q, 90000) == 0, "an empty queue has no age");
}

static void test_consumer_fields_are_single_words(void)
{
    DcAckq q;

    dc_ackq_init(&q);

    check(sizeof(q.head) == 4, "head is one 32 bit word");
    check(sizeof(q.tail) == 4, "tail is one 32 bit word");
    check(sizeof(q.count) == 4, "count is one 32 bit word");
    check(sizeof(q.dropped) == 4, "dropped is one 32 bit word");
    check(sizeof(q.head_stamp_ms) == 4, "head_stamp_ms is one 32 bit word, not 64");
    check(sizeof(q.head_stamped) == 1, "head_stamped is a single byte");
    check(offsetof(DcAckq, slots) % 4u == 0u, "the payload array starts on a 32 bit boundary");
    check(offsetof(DcAckq, head_stamped) % 4u == 0u,
          "the last consumer-read field starts on a 32 bit boundary");
    check(sizeof(DcAckq) == 8992, "the queue is 8992 B: 32 x 280 B acks (modal pointer) + 32 B index");
    check(sizeof(DcAckq) - payload_floor_bytes() == 32,
          "index and bookkeeping measure 32 B, down from 40 B with a 64 bit stamp");

    DiscordAck a = mk_ack(1, 4);
    dc_ackq_push(&q, &a);
    uint64_t huge = 0xFFFFFFFF00000000ull;
    check(dc_ackq_oldest_age_ms(&q, huge) == 0,
          "a clock whose low word is zero stamps as 0 and reports age 0");
    check(dc_ackq_oldest_age_ms(&q, huge + 70000ull) == 70000,
          "the age difference is computed in the 32 bit domain, so no torn 64 bit compare shows up");
}

static void test_footprint(void)
{
    size_t payload_floor = payload_floor_bytes();
    size_t footprint = dc_ackq_footprint();
    size_t slot_used = offsetof(DiscordAck, ephemeral) + sizeof(bool);
    size_t slot_padding = (size_t)DC_ACKQ_CAPACITY * (sizeof(DiscordAck) - slot_used);
    size_t index = footprint - payload_floor;
    size_t overhead = index + slot_padding;

    printf("\nfootprint %zu B (%.2f KiB), %d slots x %zu B payload = %zu B\n",
           footprint, (double)footprint / 1024.0, DC_ACKQ_CAPACITY, sizeof(DiscordAck),
           payload_floor);
    printf("index and bookkeeping %zu B, alignment padding %zu B (%zu B per slot), "
           "true overhead %zu B, amortised %.2f B per entry\n\n",
           index, slot_padding, sizeof(DiscordAck) - slot_used, overhead,
           (double)footprint / (double)DC_ACKQ_CAPACITY);

    check(sizeof(DiscordAck) == 280, "DiscordAck measures 280 B per entry (one borrowed modal pointer)");
    check(sizeof(DiscordAck) - slot_used == 13, "13 B per slot is alignment padding, not data");
    check(slot_padding == 416, "32 slots carry 416 B of alignment padding");
    check(index == 32, "index and bookkeeping measure 32 B");
    check(overhead == 448, "true non-payload overhead is 448 B: 32 B of index plus 416 B of padding");
    check(footprint == sizeof(DcAckq), "footprint is the whole queue struct");
    check(footprint <= 10240, "ack queue fits the 10 KiB application allowance");
    check(footprint >= payload_floor, "footprint covers all 32 payloads");
    check(footprint > payload_floor, "index and bookkeeping are counted, not hidden");
}

int main(void)
{
    test_capacity_and_shedding();
    test_expired_ack_is_dead();
    test_payload_survives_the_ring();
    test_wrap_after_shedding();
    test_invalid_pushes_are_refused();
    test_age_is_the_oldest_live_entry();
    test_peek_does_not_consume();
    test_null_guards();
    test_counters_are_exact();
    test_shed_restarts_the_age_clock();
    test_age_interleaved_with_sheds();
    test_clock_wraps_without_a_false_expiry();
    test_sustained_overload_stays_bounded();
    test_consumer_fields_are_single_words();
    test_footprint();

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
