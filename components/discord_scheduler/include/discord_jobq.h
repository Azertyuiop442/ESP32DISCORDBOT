#ifndef DISCORD_JOBQ_H
#define DISCORD_JOBQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "discord_slice.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DC_JOBQ_CAPACITY 16
/* 112 B: RAW message text for SEND_MESSAGE/SEND_DM (captcha DM sentence,
 * log lines, poll pings). The JSON body is built later on the rest task,
 * where escaping grows at most 6 B/byte inside its own 1 KiB buffer; the
 * job only carries the text. DcJob 80 -> 144 B, so the 16-slot ring
 * costs one extra KiB static. */
#define DC_JOB_PAYLOAD 112

typedef enum {
    DC_JOB_NONE = 0,
    /** channel_id = target, payload = raw text, aux_id = accent color
     * (raw 24-bit RGB, 0 = default blurple). Body built on the rest task. */
    DC_JOB_SEND_MESSAGE,
    DC_JOB_TIMEOUT_USER,
    DC_JOB_DELETE_MESSAGE,
    DC_JOB_ADD_ROLE,
    DC_JOB_REMOVE_ROLE,
    DC_JOB_EDIT_MESSAGE,
    DC_JOB_CUSTOM,
    DC_JOB_NUKE,
    /** user_id = recipient, payload = raw text, aux_id = accent color
     * (0 = default blurple). Two chained REST calls:
     * POST /users/@me/channels then POST /channels/{dm}/messages. */
    DC_JOB_SEND_DM
} DcJobKind;

typedef enum {
    DC_JOB_OK = 0,
    DC_JOBQ_FULL = -1,
    DC_JOBQ_BAD_KIND = -2
} DcJobStatus;

typedef struct {
    DiscordId  guild_id;
    DiscordId  channel_id;
    DiscordId  user_id;
    uint32_t   aux_id;
    uint8_t    kind;
    uint8_t    attempts;
    uint8_t    payload_len;
    char       payload[DC_JOB_PAYLOAD];
} DcJob;

typedef enum {
    DC_JOB_DROP_NEWEST = 0,
    DC_JOB_DROP_OLDEST = 1,
    DC_JOB_REJECT = 2
} DcJobOverloadPolicy;

typedef struct {
    DcJob      slots[DC_JOBQ_CAPACITY];
    uint32_t   head;
    uint32_t   tail;
    uint32_t   count;
    uint32_t   peak;
    uint64_t   enqueued;
    uint64_t   executed;
    uint64_t   dropped;
    uint64_t   failed;
    DcJobOverloadPolicy policy;
} DcJobQueue;

void dc_jobq_init(DcJobQueue *q, DcJobOverloadPolicy policy);
DcJobStatus dc_jobq_push(DcJobQueue *q, const DcJob *job);
bool dc_jobq_pop(DcJobQueue *q, DcJob *out);
bool dc_jobq_peek(const DcJobQueue *q, DcJob *out);
uint32_t dc_jobq_count(const DcJobQueue *q);
uint32_t dc_jobq_peak(const DcJobQueue *q);
uint32_t dc_jobq_free(const DcJobQueue *q);
bool dc_jobq_full(const DcJobQueue *q);
uint32_t dc_jobq_load_permille(const DcJobQueue *q);
void dc_jobq_note_executed(DcJobQueue *q, bool success);

#ifdef __cplusplus
}
#endif

#endif