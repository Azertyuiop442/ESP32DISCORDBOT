#include "discord_ackq.h"

#include <string.h>

void dc_ackq_init(DcAckq *q)
{
    if (q == NULL) {
        return;
    }

    memset(q, 0, sizeof(*q));
    q->head = 0;
    q->tail = 0;
    q->count = 0;
}

static uint32_t slot_after(uint32_t slot)
{
    return (slot + 1u) % DC_ACKQ_CAPACITY;
}

static void advance_tail(DcAckq *q)
{
    q->tail = slot_after(q->tail);
}

static void advance_head(DcAckq *q)
{
    q->head = slot_after(q->head);
}

static void mark_entry_live(DcAckq *q)
{
    q->head_stamp_ms = 0;
    q->head_stamped = false;
}

DcAckStatus dc_ackq_push(DcAckq *q, const DiscordAck *ack)
{
    if (q == NULL || ack == NULL || ack->id == 0 || ack->kind == 0) {
        return DC_ACK_INVALID;
    }

    if (q->count == DC_ACKQ_CAPACITY) {
        advance_head(q);
        mark_entry_live(q);
        q->count--;
        q->dropped++;
    }

    q->slots[q->tail] = *ack;
    advance_tail(q);
    q->count++;
    q->enqueued++;
    if (q->count > q->peak) {
        q->peak = q->count;
    }

    return DC_ACK_OK;
}

bool dc_ackq_pop(DcAckq *q, DiscordAck *out)
{
    if (q == NULL || out == NULL || q->count == 0) {
        return false;
    }

    *out = q->slots[q->head];
    advance_head(q);
    q->count--;
    mark_entry_live(q);

    return true;
}

bool dc_ackq_peek(const DcAckq *q, DiscordAck *out)
{
    if (q == NULL || q->count == 0) {
        return false;
    }

    if (out != NULL) {
        *out = q->slots[q->head];
    }

    return true;
}

uint32_t dc_ackq_depth(const DcAckq *q)
{
    return q == NULL ? 0u : q->count;
}

uint32_t dc_ackq_dropped(const DcAckq *q)
{
    return q == NULL ? 0u : q->dropped;
}

uint32_t dc_ackq_oldest_age_ms(DcAckq *q, uint64_t now_ms)
{
    if (q == NULL) {
        return 0u;
    }

    uint32_t count = q->count;
    uint32_t stamped = q->head_stamped ? 1u : 0u;
    uint32_t stamp = q->head_stamp_ms;

    if (count == 0u) {
        return 0u;
    }

    if (stamped == 0u) {
        q->head_stamp_ms = (uint32_t)(now_ms & DC_ACKQ_AGE_WRAP_MS);
        q->head_stamped = true;
        return 0u;
    }

    uint32_t clock = (uint32_t)(now_ms & DC_ACKQ_AGE_WRAP_MS);
    if (clock <= stamp) {
        return 0u;
    }

    return clock - stamp;
}

size_t dc_ackq_footprint(void)
{
    return sizeof(DcAckq);
}
