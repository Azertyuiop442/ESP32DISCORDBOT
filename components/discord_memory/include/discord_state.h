#ifndef DISCORD_STATE_H
#define DISCORD_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "discord_slice.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DC_TICKET_SLOTS 16
#define DC_CAPTCHA_SLOTS 8
#define DC_SPAM_USERS 32
#define DC_SPAM_WINDOW 8
#define DC_RANK_USERS 128

typedef enum {
    DC_TICKET_OPEN = 0,
    DC_TICKET_CLOSED = 1,
    DC_TICKET_SPAM = 2
} DcTicketStatus;

typedef enum {
    DC_TICKET_BUG = 0,
    DC_TICKET_FEATURE = 1
} DcTicketKind;

typedef struct {
    DiscordId ticket_id;
    DiscordId user_id;
    DiscordId channel_id;
    DiscordId message_id;
    uint8_t status;
    uint8_t kind;
    bool used;
} DcTicket;

typedef struct {
    /* Guild the challenge was issued in. The DM reply carries no guild_id,
     * so the role grant on success reads it from here. */
    DiscordId guild_id;
    DiscordId user_id;
    uint64_t expires_at_ms;
    uint64_t issued_at_ms;
    uint32_t attempts;
    char answer[16];
    uint8_t answer_len;
    bool active;
} DcCaptcha;

typedef struct {
    DiscordId user_id;
    uint32_t hashes[DC_SPAM_WINDOW];
    uint32_t window_start_ms;
    uint8_t hash_count;
    uint8_t hash_index;
    uint8_t strikes;
    bool tracked;
} DcSpamUser;

typedef struct {
    DiscordId user_id;
    uint32_t xp;
    uint8_t level;
    bool tracked;
} DcRankUser;

typedef struct {
    DcTicket tickets[DC_TICKET_SLOTS];
    DcCaptcha captchas[DC_CAPTCHA_SLOTS];
    DcSpamUser spam[DC_SPAM_USERS];
    DcRankUser ranks[DC_RANK_USERS];
    uint32_t ticket_cursor;
    uint32_t captcha_cursor;
    uint32_t spam_cursor;
    uint32_t rank_cursor;
} DcBotState;

size_t dc_state_footprint(void);
DcTicket *dc_state_open_ticket(DcBotState *s, DiscordId user_id, DiscordId channel_id);

/**
 * Look up a ticket by its Discord message ID.
 *
 * The owner check is mandatory, not optional: without it any server member
 * could pass an arbitrary message_id and receive a writable pointer to
 * another user's ticket, including its owner field.
 */
DcTicket *dc_state_find_ticket(DcBotState *s, DiscordId message_id, DiscordId requester_id);

/** Same lookup, but only exposes the ticket if it is still open. */
DcTicket *dc_state_find_open_ticket(DcBotState *s, DiscordId message_id, DiscordId requester_id);

bool dc_state_ticket_belongs_to(const DcBotState *s, DiscordId message_id, DiscordId requester_id);
bool dc_state_close_ticket(DcBotState *s, DiscordId message_id, DiscordId requester_id);
DcCaptcha *dc_state_open_captcha(DcBotState *s, DiscordId user_id,
                                 const char *answer, uint8_t answer_len,
                                 uint64_t now_ms, uint64_t ttl_ms);
bool dc_state_verify_captcha(DcBotState *s, DiscordId user_id, const char *reply,
                             uint8_t reply_len, uint32_t max_attempts);
DcSpamUser *dc_state_spam_user(DcBotState *s, DiscordId user_id);
DcRankUser *dc_state_rank_user(DcBotState *s, DiscordId user_id);

#ifdef __cplusplus
}
#endif

#endif
