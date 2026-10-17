#include "discord_jobq.h"

#include <string.h>

void dc_jobq_init(DcJobQueue *q, DcJobOverloadPolicy policy)
{
    memset(q, 0, sizeof(*q));
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->policy = policy;
}

static void advance_tail(DcJobQueue *q)
{
    q->tail = (q->tail + 1) % DC_JOBQ_CAPACITY;
}

static void advance_head(DcJobQueue *q)
{
    q->head = (q->head + 1) % DC_JOBQ_CAPACITY;
}

DcJobStatus dc_jobq_push(DcJobQueue *q, const DcJob *job)
{
    if (!job || job->kind == DC_JOB_NONE || job->kind > DC_JOB_SEND_DM) {
        return DC_JOBQ_BAD_KIND;
    }

    if (q->count == DC_JOBQ_CAPACITY) {
        switch (q->policy) {
        case DC_JOB_REJECT:
            q->dropped++;
            return DC_JOBQ_FULL;
        case DC_JOB_DROP_OLDEST:
            advance_head(q);
            q->count--;
            q->dropped++;
            break;
        case DC_JOB_DROP_NEWEST:
        default:
            q->dropped++;
            return DC_JOBQ_FULL;
        }
    }

    q->slots[q->tail] = *job;
    advance_tail(q);
    q->count++;
    q->enqueued++;
    if (q->count > q->peak) {
        q->peak = q->count;
    }
    return DC_JOB_OK;
}

bool dc_jobq_pop(DcJobQueue *q, DcJob *out)
{
    if (q->count == 0) {
        return false;
    }
    if (out) {
        *out = q->slots[q->head];
    }
    advance_head(q);
    q->count--;
    return true;
}

bool dc_jobq_peek(const DcJobQueue *q, DcJob *out)
{
    if (q->count == 0) {
        return false;
    }
    if (out) {
        *out = q->slots[q->head];
    }
    return true;
}

uint32_t dc_jobq_count(const DcJobQueue *q)
{
    return q->count;
}

uint32_t dc_jobq_peak(const DcJobQueue *q)
{
    return q->peak;
}

uint32_t dc_jobq_free(const DcJobQueue *q)
{
    return DC_JOBQ_CAPACITY - q->count;
}

bool dc_jobq_full(const DcJobQueue *q)
{
    return q->count == DC_JOBQ_CAPACITY;
}

uint32_t dc_jobq_load_permille(const DcJobQueue *q)
{
    return (uint32_t)((uint64_t)q->count * 1000u / DC_JOBQ_CAPACITY);
}

void dc_jobq_note_executed(DcJobQueue *q, bool success)
{
    if (q->count > 0) {
        advance_head(q);
        q->count--;
    }
    if (success) {
        q->executed++;
    } else {
        q->failed++;
    }
}