#include <stdio.h>
#include <string.h>

#include "discord_jobq.h"

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

static DcJob mk(DiscordId user, uint8_t kind)
{
    DcJob j;
    memset(&j, 0, sizeof(j));
    j.kind = kind;
    j.user_id = user;
    j.channel_id = 222;
    return j;
}

static void test_fifo_order(void)
{
    DcJobQueue q;
    dc_jobq_init(&q, DC_JOB_REJECT);

    for (uint32_t i = 1; i <= 5; i++) {
        DcJob j = mk(i, DC_JOB_SEND_MESSAGE);
        check(dc_jobq_push(&q, &j) == DC_JOB_OK, "push accepted");
    }
    check(dc_jobq_count(&q) == 5, "count is 5");

    for (uint32_t i = 1; i <= 5; i++) {
        DcJob out;
        check(dc_jobq_pop(&q, &out), "pop succeeds");
        check(out.user_id == i, "FIFO order preserved");
    }
    check(dc_jobq_count(&q) == 0, "queue empty after draining");
}

static void test_empty(void)
{
    DcJobQueue q;
    dc_jobq_init(&q, DC_JOB_REJECT);

    DcJob out;
    check(!dc_jobq_pop(&q, &out), "pop on empty returns false");
    check(!dc_jobq_peek(&q, &out), "peek on empty returns false");
    check(dc_jobq_count(&q) == 0, "count stays 0");
    check(dc_jobq_free(&q) == DC_JOBQ_CAPACITY, "all slots free");
}

static void test_reject_policy(void)
{
    DcJobQueue q;
    dc_jobq_init(&q, DC_JOB_REJECT);

    for (uint32_t i = 0; i < DC_JOBQ_CAPACITY; i++) {
        DcJob j = mk(i, DC_JOB_SEND_MESSAGE);
        dc_jobq_push(&q, &j);
    }
    check(dc_jobq_full(&q), "queue full");
    DcJob overflow = mk(999, DC_JOB_SEND_MESSAGE);
    check(dc_jobq_push(&q, &overflow) == DC_JOBQ_FULL, "full queue rejects");
    check(q.dropped == 1, "drop counted");

    DcJob out;
    dc_jobq_pop(&q, &out);
    check(out.user_id == 0, "existing jobs untouched by rejection");
}

static void test_drop_newest(void)
{
    DcJobQueue q;
    dc_jobq_init(&q, DC_JOB_DROP_NEWEST);

    for (uint32_t i = 0; i < DC_JOBQ_CAPACITY; i++) {
        DcJob j = mk(i, DC_JOB_SEND_MESSAGE);
        dc_jobq_push(&q, &j);
    }
    DcJob newest = mk(999, DC_JOB_SEND_MESSAGE);
    check(dc_jobq_push(&q, &newest) == DC_JOBQ_FULL, "newest job dropped");

    DcJob out;
    dc_jobq_pop(&q, &out);
    check(out.user_id == 0, "oldest job still at the head");
    check(q.count == DC_JOBQ_CAPACITY - 1, "count decremented");
}

static void test_drop_oldest(void)
{
    DcJobQueue q;
    dc_jobq_init(&q, DC_JOB_DROP_OLDEST);

    for (uint32_t i = 0; i < DC_JOBQ_CAPACITY; i++) {
        DcJob j = mk(i, DC_JOB_SEND_MESSAGE);
        dc_jobq_push(&q, &j);
    }
    DcJob newest = mk(999, DC_JOB_SEND_MESSAGE);
    check(dc_jobq_push(&q, &newest) == DC_JOB_OK, "push succeeds by evicting");
    check(q.count == DC_JOBQ_CAPACITY, "queue still full");
    check(q.dropped == 1, "eviction counted");

    DcJob out;
    dc_jobq_pop(&q, &out);
    check(out.user_id == 1, "oldest job was evicted, not the newest");
}

static void test_wraparound(void)
{
    DcJobQueue q;
    dc_jobq_init(&q, DC_JOB_REJECT);

    for (uint32_t cycle = 0; cycle < 100; cycle++) {
        DcJob j = mk(cycle, DC_JOB_SEND_MESSAGE);
        dc_jobq_push(&q, &j);
        DcJob out;
        check(dc_jobq_pop(&q, &out), "push/pop cycle");
        check(out.user_id == cycle, "value survives wraparound");
    }
    check(q.enqueued == 100, "enqueue counter accurate");
    check(q.peak == 1, "peak never exceeded 1");
}

static void test_no_growth_under_flood(void)
{
    DcJobQueue q;
    dc_jobq_init(&q, DC_JOB_DROP_NEWEST);

    for (uint32_t i = 0; i < 100000; i++) {
        DcJob j = mk(i, DC_JOB_SEND_MESSAGE);
        dc_jobq_push(&q, &j);
        if (i % 3 == 0) {
            DcJob out;
            dc_jobq_pop(&q, &out);
        }
    }
    check(q.count <= DC_JOBQ_CAPACITY, "count never exceeds capacity");
    check(q.peak <= DC_JOBQ_CAPACITY, "peak never exceeds capacity");
    check(q.dropped > 0, "overload actually occurred and was counted");
}

static void test_bad_kind(void)
{
    DcJobQueue q;
    dc_jobq_init(&q, DC_JOB_REJECT);

    DcJob j = mk(1, 0);
    check(dc_jobq_push(&q, &j) == DC_JOBQ_BAD_KIND, "kind NONE rejected");
    j.kind = 200;
    check(dc_jobq_push(&q, &j) == DC_JOBQ_BAD_KIND, "out-of-range kind rejected");
    check(dc_jobq_push(&q, NULL) == DC_JOBQ_BAD_KIND, "NULL job rejected");
    check(q.count == 0, "nothing enqueued on rejection");
}

static void test_load_metric(void)
{
    DcJobQueue q;
    dc_jobq_init(&q, DC_JOB_REJECT);

    check(dc_jobq_load_permille(&q) == 0, "empty queue is 0 per mille");
    for (uint32_t i = 0; i < DC_JOBQ_CAPACITY / 2; i++) {
        DcJob j = mk(i, DC_JOB_SEND_MESSAGE);
        dc_jobq_push(&q, &j);
    }
    check(dc_jobq_load_permille(&q) == 500, "half full is 500 per mille");
}

static void test_note_executed(void)
{
    DcJobQueue q;
    dc_jobq_init(&q, DC_JOB_REJECT);

    DcJob first = mk(1, DC_JOB_SEND_MESSAGE);
    DcJob second = mk(2, DC_JOB_SEND_MESSAGE);
    dc_jobq_push(&q, &first);
    dc_jobq_push(&q, &second);

    dc_jobq_note_executed(&q, true);
    check(q.count == 1, "success consumes a slot");
    check(q.executed == 1, "success counted");

    dc_jobq_note_executed(&q, false);
    check(q.count == 0, "failure consumes a slot");
    check(q.failed == 1, "failure counted separately");
}

static void test_footprint(void)
{
    /* Payload 48 -> 112 B (captcha welcome DM text, 78 B worst case):
     * the ring is now 16 x 144 B + 56 B of state, ~2.3 KiB. */
    printf("     DcJobQueue = %zu B\n", sizeof(DcJobQueue));
    printf("     DcJob      = %zu B\n", sizeof(DcJob));
    check(sizeof(DcJobQueue) < 2560, "queue under 2.5 KiB");
    check(sizeof(DcJob) <= 160, "job stays compact");
}

static void test_burst_200_interleaved_zero_lost(void)
{
    /* "200 commands in 2 s, 0 lost": producer and consumer interleave
     * (the rest task polls every 10 ms), so every push drains. */
    DcJobQueue q;
    DcJob out;
    uint32_t last = 0;
    dc_jobq_init(&q, DC_JOB_DROP_OLDEST);

    for (uint32_t i = 1; i <= 200; i++) {
        DcJob j = mk(i, DC_JOB_SEND_MESSAGE);
        check(dc_jobq_push(&q, &j) == DC_JOB_OK, "burst push accepted");
        check(dc_jobq_pop(&q, &out), "consumer keeps up");
        check(out.user_id == i, "FIFO order under burst");
        last = i;
    }
    check(last == 200, "all 200 burst jobs drained in order");
    check(q.dropped == 0, "zero shed when the consumer keeps up");
    check(q.enqueued == 200 && q.executed == 0, "counters consistent");
    check(q.peak == 1, "depth never exceeded 1");
}

static void test_burst_200_stalled_counts_every_shed(void)
{
    /* Stalled consumer: 200 pushes into 16 slots. Losses are COUNTED
     * (dropped), never silent — /stats exposes the counter. */
    DcJobQueue q;
    DcJob out;
    dc_jobq_init(&q, DC_JOB_DROP_OLDEST);

    for (uint32_t i = 1; i <= 200; i++) {
        DcJob j = mk(i, DC_JOB_SEND_MESSAGE);
        dc_jobq_push(&q, &j);
    }
    check(q.dropped == 200 - DC_JOBQ_CAPACITY, "every shed counted");
    check(q.count == DC_JOBQ_CAPACITY, "depth capped at 16");
    check(dc_jobq_pop(&q, &out), "drain works after overload");
    check(out.user_id == (200 - DC_JOBQ_CAPACITY + 1),
          "oldest shed first, newest kept");
}

int main(void)
{
    test_fifo_order();
    test_empty();
    test_reject_policy();
    test_drop_newest();
    test_drop_oldest();
    test_wraparound();
    test_no_growth_under_flood();
    test_bad_kind();
    test_load_metric();
    test_note_executed();
    test_footprint();
    test_burst_200_interleaved_zero_lost();
    test_burst_200_stalled_counts_every_shed();

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}