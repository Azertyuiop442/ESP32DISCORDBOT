#include "discord_state.h"

#include <string.h>

typedef bool (*InUseFn)(const void *slot);

static void *claim_slot(void *table, size_t stride, uint32_t *cursor, uint32_t cap,
                        InUseFn in_use)
{
    for (uint32_t i = 0; i < cap; i++) {
        if (!in_use((const uint8_t *)table + i * stride)) {
            *cursor = i + 1;
            memset((uint8_t *)table + i * stride, 0, stride);
            return (uint8_t *)table + i * stride;
        }
    }
    uint32_t idx = *cursor % cap;
    *cursor = idx + 1;
    memset((uint8_t *)table + idx * stride, 0, stride);
    return (uint8_t *)table + idx * stride;
}

static bool ticket_used(const void *p)
{
    return ((const DcTicket *)p)->used;
}

static bool spam_used(const void *p)
{
    return ((const DcSpamUser *)p)->tracked;
}

static bool rank_used(const void *p)
{
    return ((const DcRankUser *)p)->tracked;
}

size_t dc_state_footprint(void)
{
    return sizeof(DcBotState);
}

DcTicket *dc_state_open_ticket(DcBotState *s, DiscordId user_id, DiscordId channel_id)
{
    DcTicket *t = claim_slot(s->tickets, sizeof(DcTicket), &s->ticket_cursor,
                             DC_TICKET_SLOTS, ticket_used);
    t->user_id = user_id;
    t->channel_id = channel_id;
    t->status = DC_TICKET_OPEN;
    t->kind = DC_TICKET_BUG;
    t->used = true;
    return t;
}

DcTicket *dc_state_find_ticket(DcBotState *s, DiscordId message_id, DiscordId requester_id)
{
    if (message_id == 0 || requester_id == 0) {
        return NULL;
    }
    for (uint32_t i = 0; i < DC_TICKET_SLOTS; i++) {
        if (s->tickets[i].used && s->tickets[i].message_id == message_id &&
            s->tickets[i].user_id == requester_id) {
            return &s->tickets[i];
        }
    }
    return NULL;
}

DcTicket *dc_state_find_open_ticket(DcBotState *s, DiscordId message_id,
                                    DiscordId requester_id)
{
    DcTicket *t = dc_state_find_ticket(s, message_id, requester_id);
    if (!t || t->status != DC_TICKET_OPEN) {
        return NULL;
    }
    return t;
}

bool dc_state_ticket_belongs_to(const DcBotState *s, DiscordId message_id,
                                DiscordId requester_id)
{
    if (message_id == 0 || requester_id == 0) {
        return false;
    }
    for (uint32_t i = 0; i < DC_TICKET_SLOTS; i++) {
        if (s->tickets[i].used && s->tickets[i].message_id == message_id &&
            s->tickets[i].user_id == requester_id) {
            return true;
        }
    }
    return false;
}

bool dc_state_close_ticket(DcBotState *s, DiscordId message_id, DiscordId requester_id)
{
    DcTicket *t = dc_state_find_ticket(s, message_id, requester_id);
    if (!t) {
        return false;
    }
    t->status = DC_TICKET_CLOSED;
    return true;
}

DcCaptcha *dc_state_open_captcha(DcBotState *s, DiscordId user_id,
                                 const char *answer, uint8_t answer_len,
                                 uint64_t now_ms, uint64_t ttl_ms)
{
    if (answer_len == 0 || answer_len >= sizeof(s->captchas[0].answer)) {
        return NULL;
    }
    for (uint32_t i = 0; i < DC_CAPTCHA_SLOTS; i++) {
        DcCaptcha *c = &s->captchas[i];
        if (!c->active || c->expires_at_ms <= now_ms) {
            memset(c, 0, sizeof(*c));
            memcpy(c->answer, answer, answer_len);
            c->answer_len = answer_len;
            c->user_id = user_id;
            c->issued_at_ms = now_ms;
            c->expires_at_ms = now_ms + ttl_ms;
            c->attempts = 0;
            c->active = true;
            return c;
        }
    }
    return NULL;
}

bool dc_state_verify_captcha(DcBotState *s, DiscordId user_id, const char *reply,
                             uint8_t reply_len, uint32_t max_attempts)
{
    for (uint32_t i = 0; i < DC_CAPTCHA_SLOTS; i++) {
        DcCaptcha *c = &s->captchas[i];
        if (!c->active || c->user_id != user_id) {
            continue;
        }
        if (c->answer_len == reply_len && memcmp(c->answer, reply, reply_len) == 0) {
            memset(c, 0, sizeof(*c));
            return true;
        }
        c->attempts++;
        if (c->attempts >= max_attempts) {
            memset(c, 0, sizeof(*c));
        }
        return false;
    }
    return false;
}

DcSpamUser *dc_state_spam_user(DcBotState *s, DiscordId user_id)
{
    for (uint32_t i = 0; i < DC_SPAM_USERS; i++) {
        if (s->spam[i].tracked && s->spam[i].user_id == user_id) {
            return &s->spam[i];
        }
    }
    DcSpamUser *u = claim_slot(s->spam, sizeof(DcSpamUser), &s->spam_cursor,
                               DC_SPAM_USERS, spam_used);
    u->user_id = user_id;
    u->tracked = true;
    return u;
}

DcRankUser *dc_state_rank_user(DcBotState *s, DiscordId user_id)
{
    for (uint32_t i = 0; i < DC_RANK_USERS; i++) {
        if (s->ranks[i].tracked && s->ranks[i].user_id == user_id) {
            return &s->ranks[i];
        }
    }
    DcRankUser *u = claim_slot(s->ranks, sizeof(DcRankUser), &s->rank_cursor,
                               DC_RANK_USERS, rank_used);
    u->user_id = user_id;
    u->tracked = true;
    return u;
}

bool dc_state_forget_user(DcBotState *s, DiscordId user_id)
{
    if (s == NULL || user_id == 0u) {
        return false;
    }
    bool found = false;
    for (uint32_t i = 0; i < DC_RANK_USERS; i++) {
        if (s->ranks[i].tracked && s->ranks[i].user_id == user_id) {
            memset(&s->ranks[i], 0, sizeof(DcRankUser));
            found = true;
        }
    }
    for (uint32_t i = 0; i < DC_SPAM_USERS; i++) {
        if (s->spam[i].tracked && s->spam[i].user_id == user_id) {
            memset(&s->spam[i], 0, sizeof(DcSpamUser));
            found = true;
        }
    }
    return found;
}

