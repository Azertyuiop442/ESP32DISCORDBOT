/* discord_intq.c — bounded interaction ring (see discord_intq.h for the
 * threading contract and why the shed drops the newest entry). */

#include "discord_intq.h"

#include <stddef.h>

static void advance(uint32_t *index)
{
    *index = (*index + 1u) % DC_INTQ_CAPACITY;
}

void dc_intq_init(DcIntq *q)
{
    if (q == NULL) {
        return;
    }
    q->head = 0u;
    q->tail = 0u;
    q->count = 0u;
    q->peak = 0u;
    q->enqueued = 0u;
    q->dropped = 0u;
}

DcIntStatus dc_intq_push(DcIntq *q, const DiscordInteraction *it)
{
    if (q == NULL || it == NULL || it->id == 0u) {
        return DC_INTQ_INVALID;
    }

    /* An arrival is an arrival whether or not it fits: enqueued counts both,
     * so enqueued == delivered + dropped + live holds as an identity. */
    q->enqueued++;

    if (q->count >= DC_INTQ_CAPACITY) {
        q->dropped++;
        return DC_INTQ_FULL;
    }

    q->slots[q->tail] = *it;
    advance(&q->tail);
    q->count++;
    if (q->count > q->peak) {
        q->peak = q->count;
    }
    return DC_INT_OK;
}

bool dc_intq_pop(DcIntq *q, DiscordInteraction *out)
{
    if (q == NULL || out == NULL || q->count == 0u) {
        return false;
    }

    /* Whole-struct copy: DiscordInteraction is fixed arrays only, so there is
     * no pointer into the WebSocket receive buffer to outlive. */
    *out = q->slots[q->head];
    advance(&q->head);
    q->count--;
    return true;
}

bool dc_intq_peek(DcIntq *q, DiscordInteraction *out)
{
    if (q == NULL || q->count == 0u) {
        return false;
    }
    if (out != NULL) {
        *out = q->slots[q->head];
    }
    return true;
}

uint32_t dc_intq_depth(const DcIntq *q)
{
    return (q == NULL) ? 0u : q->count;
}

uint32_t dc_intq_free(const DcIntq *q)
{
    if (q == NULL) {
        return 0u;
    }
    return (q->count < DC_INTQ_CAPACITY) ? (DC_INTQ_CAPACITY - q->count) : 0u;
}

uint32_t dc_intq_dropped(const DcIntq *q)
{
    return (q == NULL) ? 0u : q->dropped;
}

uint32_t dc_intq_peak(const DcIntq *q)
{
    return (q == NULL) ? 0u : q->peak;
}

bool dc_intq_full(const DcIntq *q)
{
    return (q != NULL) && (q->count >= DC_INTQ_CAPACITY);
}

size_t dc_intq_footprint(void)
{
    return sizeof(DcIntq);
}
