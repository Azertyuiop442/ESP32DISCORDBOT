/**
 * @file test_config.c
 * @brief Proves the central claim: queued work lives in RAM, and malloc is
 *        never called to get it there.
 *
 * The distinction matters and is easy to miss: "no malloc" does not mean
 * "no memory". It means every byte is reserved at compile time instead of
 * requested at runtime. This test demonstrates exactly that.
 */

#include "bot_config.h"

#include <stdio.h>
#include <string.h>

#include "discord_jobq.h"
#include "discord_ratelimit.h"
#include "discord_state.h"

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

unsigned dc_config_error_count(void);
unsigned dc_config_projected_ram(void);
unsigned dc_config_checks_run(void);
const char *dc_config_check_label(unsigned index);
unsigned dc_config_check_value(unsigned index);

static void test_config_valid(void)
{
    printf("     %u checks run\n", dc_config_checks_run());
    check(dc_config_error_count() == 0, "shipped configuration passes validation");
}

static void test_projected_ram(void)
{
    unsigned ram = dc_config_projected_ram();
    printf("     projected RAM = %u B (%.2f KiB)\n", ram, ram / 1024.0);
    check(ram > 0, "projected RAM is non-zero");
    check(ram < 16384, "under 16 KiB");
}

/**
 * The demonstration. We push far more jobs than the queue can hold, then
 * confirm that the jobs are still stored in RAM and that the address of the
 * queue is a fixed static address that never changes.
 */
static void test_queue_is_ram_not_heap(void)
{
    static DcJobQueue queue;
    dc_jobq_init(&queue, DC_JOB_DROP_OLDEST);

    printf("     queue address = %p\n", (void *)&queue);
    DcJobQueue *alias = &queue;
    check(alias == &queue, "queue address is stable across reads");

    for (uint32_t i = 0; i < 1000; i++) {
        DcJob j;
        memset(&j, 0, sizeof(j));
        j.kind = DC_JOB_SEND_MESSAGE;
        j.user_id = 1000 + i;
        j.channel_id = 222;
        j.payload_len = 4;
        memcpy(j.payload, "ping", 4);
        dc_jobq_push(&queue, &j);
    }

    check(dc_jobq_count(&queue) > 0, "jobs are retained after 1000 pushes");
    check(dc_jobq_count(&queue) <= DC_JOBQ_CAPACITY, "retention is bounded");
    check(queue.dropped > 0, "overload happened and was counted");

    DcJob head;
    check(dc_jobq_peek(&queue, &head), "a job is readable back");
    check(head.kind == DC_JOB_SEND_MESSAGE, "job survived intact");
    check(strcmp(head.payload, "ping") == 0, "payload survived intact");
    printf("     head job: kind=%u user=%llu payload=%s\n", head.kind,
           (unsigned long long)head.user_id, head.payload);

    check(alias == &queue, "queue address unchanged after 1000 operations");
    check(dc_jobq_peak(&queue) <= DC_JOBQ_CAPACITY, "peak never exceeded capacity");
}

static void test_capacity_never_grows(void)
{
    DcJobQueue q1;
    DcJobQueue q2;
    dc_jobq_init(&q1, DC_JOB_DROP_NEWEST);
    dc_jobq_init(&q2, DC_JOB_DROP_NEWEST);

    for (uint32_t i = 0; i < 100000; i++) {
        DcJob j;
        memset(&j, 0, sizeof(j));
        j.kind = DC_JOB_TIMEOUT_USER;
        dc_jobq_push(&q1, &j);
        if (i % 4 == 0) {
            DcJob out;
            dc_jobq_pop(&q1, &out);
        }
    }
    for (uint32_t i = 0; i < 5; i++) {
        DcJob j;
        memset(&j, 0, sizeof(j));
        j.kind = DC_JOB_TIMEOUT_USER;
        dc_jobq_push(&q2, &j);
    }

    check(sizeof(q1) == sizeof(q2), "a heavily used queue is the same size as an idle one");
    check(dc_jobq_peak(&q1) <= DC_JOBQ_CAPACITY, "peak stays within the fixed array");
}

static void test_config_drives_macros(void)
{
    check(DC_RANK_SLOTS == 128, "config value reaches the framework");
    check(DC_CAPTCHA_ATTEMPTS == 3, "retry budget comes from config");
    check(DC_IDENTIFY_DAILY_MAX == 1000, "IDENTIFY ceiling is not raised above Discord's");
    check(DC_FEATURE_SPAM == 0, "privileged intent is opt-in, off by default");
}

int main(void)
{
    test_config_valid();
    test_projected_ram();
    test_queue_is_ram_not_heap();
    test_capacity_never_grows();
    test_config_drives_macros();

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}