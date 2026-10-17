#include <stdio.h>
#include <string.h>

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

static void test_footprint(void)
{
    size_t total = dc_state_footprint();
    printf("     DcBotState = %zu bytes (%.2f KiB)\n", total, (double)total / 1024.0);
    check(total < 8192, "bot state fits in 8 KiB");
    check(total > 0, "footprint is non-zero");

    printf("     ticket=%zu captcha=%zu spam=%zu rank=%zu\n",
           sizeof(DcTicket), sizeof(DcCaptcha), sizeof(DcSpamUser), sizeof(DcRankUser));
}

static void test_tickets(void)
{
    DcBotState s;
    memset(&s, 0, sizeof(s));

    DcTicket *t = dc_state_open_ticket(&s, 111, 222);
    check(t != NULL, "ticket opened");
    t->message_id = 9001;

    DcTicket *found = dc_state_find_ticket(&s, 9001, 111);
    check(found == t, "ticket found by message id");
    check(found->user_id == 111 && found->channel_id == 222, "ticket ids preserved");
    check(found->status == DC_TICKET_OPEN, "ticket starts open");

    check(dc_state_find_ticket(&s, 0, 111) == NULL, "zero message id never matches");
    check(dc_state_find_ticket(&s, 4242, 111) == NULL, "unknown message id returns NULL");

    found->status = DC_TICKET_CLOSED;
    check(dc_state_find_ticket(&s, 9001, 111)->status == DC_TICKET_CLOSED, "status mutable");
}

static void test_tickets_bounded(void)
{
    DcBotState s;
    memset(&s, 0, sizeof(s));

    size_t before = dc_state_footprint();
    for (int i = 0; i < 500; i++) {
        DcTicket *t = dc_state_open_ticket(&s, (DiscordId)(1000 + i), 222);
        t->message_id = (DiscordId)(9000 + i);
    }
    check(dc_state_footprint() == before, "footprint constant under 500 tickets");

    bool ids_unique = true;
    for (uint32_t i = 0; i < DC_TICKET_SLOTS; i++) {
        for (uint32_t j = i + 1; j < DC_TICKET_SLOTS; j++) {
            if (s.tickets[i].message_id == s.tickets[j].message_id) {
                ids_unique = false;
            }
        }
    }
    check(ids_unique, "oldest slot recycled, no duplicate ids");

    DcTicket *found = dc_state_find_ticket(&s, 9000, 1001);
    check(found == NULL, "evicted ticket no longer found");
    check(dc_state_find_ticket(&s, 9499, 1499) != NULL, "most recent ticket retained");
}

static void test_ticket_ownership(void)
{
    DcBotState s;
    memset(&s, 0, sizeof(s));

    DcTicket *t = dc_state_open_ticket(&s, 111, 222);
    t->message_id = 9001;

    check(dc_state_ticket_belongs_to(&s, 9001, 111), "owner recognised");
    check(!dc_state_ticket_belongs_to(&s, 9001, 999), "non-owner rejected");
    check(!dc_state_ticket_belongs_to(&s, 9001, 0), "zero requester rejected");

    check(dc_state_find_ticket(&s, 9001, 111) == t, "owner gets the ticket");
    check(dc_state_find_ticket(&s, 9001, 999) == NULL,
          "non-owner gets nothing, not a writable pointer");
    check(dc_state_find_ticket(&s, 9001, 0) == NULL, "zero requester gets nothing");

    check(dc_state_close_ticket(&s, 9001, 111), "owner can close");
    check(t->status == DC_TICKET_CLOSED, "status updated");
    check(!dc_state_close_ticket(&s, 9001, 999), "non-owner cannot close");
}

static void test_ticket_open_lookup(void)
{
    DcBotState s;
    memset(&s, 0, sizeof(s));

    DcTicket *t = dc_state_open_ticket(&s, 111, 222);
    t->message_id = 9001;

    check(dc_state_find_open_ticket(&s, 9001, 111) == t, "open ticket returned");
    t->status = DC_TICKET_CLOSED;
    check(dc_state_find_open_ticket(&s, 9001, 111) == NULL, "closed ticket hidden");
    check(dc_state_find_ticket(&s, 9001, 111) == t, "still findable when closed");
}

static void test_captcha(void)
{
    DcBotState s;
    memset(&s, 0, sizeof(s));

    DcCaptcha *c = dc_state_open_captcha(&s, 42, "F7QK", 4, 1000, 60000);
    check(c != NULL, "captcha opened");
    check(c->answer_len == 4, "captcha answer length stored");

    check(!dc_state_verify_captcha(&s, 42, "WRNG", 4, 3), "wrong answer rejected");
    check(dc_state_open_captcha(&s, 42, "F7QK", 4, 2000, 60000) != NULL,
          "slot reusable after failed attempt");

    check(dc_state_verify_captcha(&s, 42, "F7QK", 4, 3), "correct answer accepted");

    check(dc_state_open_captcha(&s, 42, "ABCD", 4, 3000, 60000) != NULL, "captcha reopened");
    check(!dc_state_verify_captcha(&s, 42, "F7QK", 4, 3), "one-shot: old answer rejected");

    check(dc_state_open_captcha(&s, 42, "TOOLONGANSWER", 12, 4000, 60000) != NULL,
          "12-char answer accepted");
    check(dc_state_open_captcha(&s, 42, "WAYTOOLONGANSWERX", 17, 5000, 60000) == NULL,
          "oversized answer refused");
}

static void test_captcha_wrong_user(void)
{
    DcBotState s;
    memset(&s, 0, sizeof(s));

    dc_state_open_captcha(&s, 42, "ABCD", 4, 1000, 60000);
    check(!dc_state_verify_captcha(&s, 99, "ABCD", 4, 3), "other user cannot redeem");
    check(dc_state_verify_captcha(&s, 42, "ABCD", 4, 3), "owner can still redeem");
}

static void test_captcha_expiry(void)
{
    DcBotState s;
    memset(&s, 0, sizeof(s));

    dc_state_open_captcha(&s, 42, "ABCD", 4, 1000, 5000);
    dc_state_open_captcha(&s, 43, "EFGH", 4, 100000, 60000);
    check(!dc_state_verify_captcha(&s, 42, "ABCD", 4, 3), "expired captcha not redeemable");
}

static void test_spam_slots(void)
{
    DcBotState s;
    memset(&s, 0, sizeof(s));

    DcSpamUser *a = dc_state_spam_user(&s, 7);
    check(a != NULL && a->tracked, "spam user tracked");
    check(a->user_id == 7, "spam user id stored");
    check(dc_state_spam_user(&s, 7) == a, "same user returns same slot");

    for (int i = 0; i < 200; i++) {
        dc_state_spam_user(&s, (DiscordId)(100 + i));
    }
    check(dc_state_footprint() == sizeof(DcBotState), "footprint constant under flood");

    uint32_t tracked = 0;
    for (uint32_t i = 0; i < DC_SPAM_USERS; i++) {
        if (s.spam[i].tracked) {
            tracked++;
        }
    }
    check(tracked == DC_SPAM_USERS, "exactly SPAM_USERS slots in use");
}

static void test_rank_slots(void)
{
    DcBotState s;
    memset(&s, 0, sizeof(s));

    DcRankUser *r = dc_state_rank_user(&s, 500);
    check(r != NULL && r->tracked, "rank user tracked");
    r->xp = 1250;
    r->level = 4;
    check(dc_state_rank_user(&s, 500)->xp == 1250, "xp persists");
    check(dc_state_rank_user(&s, 500)->level == 4, "level persists");

    DcSpamUser *sp = dc_state_spam_user(&s, 501);
    sp->strikes = 5;
    check(dc_state_rank_user(&s, 500)->xp == 1250, "rank table unaffected by spam table");
    check(dc_state_spam_user(&s, 501)->strikes == 5, "spam table unaffected by rank table");
}

int main(void)
{
    test_footprint();
    test_tickets();
    test_tickets_bounded();
    test_ticket_ownership();
    test_ticket_open_lookup();
    test_captcha();
    test_captcha_wrong_user();
    test_captcha_expiry();
    test_spam_slots();
    test_rank_slots();

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
