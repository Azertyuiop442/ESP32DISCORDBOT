#include <stdio.h>
#include <string.h>

#include "discord_feature.h"
#include "discord_interaction_parse.h"

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

static int msg_hits;
static int int_hits;
static int captcha_int_hits;
static int tickets_int_hits;
static int ranks_int_hits;
static const char *last_feature;

static void on_msg_ranks(const DiscordMessage *m) { (void)m; msg_hits++; last_feature = "ranks"; }
static void on_msg_tickets(const DiscordMessage *m) { (void)m; msg_hits++; last_feature = "tickets"; }
static void on_int_captcha(const DiscordInteraction *i) { (void)i; int_hits++; captcha_int_hits++; }
static void on_int_tickets(const DiscordInteraction *i) { (void)i; int_hits++; tickets_int_hits++; last_feature = "tickets"; }
static void on_int_ranks(const DiscordInteraction *i) { (void)i; int_hits++; ranks_int_hits++; last_feature = "ranks"; }

static const DiscordFeature FEAT_RANKS = {
    "ranks", DC_INTENT_GUILDS, 0, on_msg_ranks, NULL, 0, NULL, NULL
};
static const DiscordFeature FEAT_TICKETS = {
    "tickets", DC_INTENT_GUILDS | DC_INTENT_GUILD_MESSAGES, 1, on_msg_tickets, NULL, 0, NULL, NULL
};
static const DiscordFeature FEAT_CAPTCHA = {
    "captcha", 0, 2, NULL, on_int_captcha, 0, NULL, NULL
};
static const DiscordFeature FEAT_SPAM = {
    "spam", DC_INTENT_GUILD_MESSAGES | DC_INTENT_MESSAGE_CONTENT, 3, on_msg_ranks, NULL, 0, NULL, NULL
};
static const DiscordFeature FEAT_TICKETS_INTERACTION = {
    "tickets", DC_INTENT_GUILDS | DC_INTENT_GUILD_MESSAGES, 1, NULL, on_int_tickets, 0, NULL, NULL
};
static const DiscordFeature FEAT_RANKS_INTERACTION = {
    "ranks", DC_INTENT_GUILDS, 0, on_msg_ranks, on_int_ranks, 0, NULL, NULL
};

static void test_empty(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    check(r.count == 0, "empty registry");
    check(dc_registry_intents(&r) == 0, "no intents");
    check(!dc_registry_has(&r, "ranks"), "nothing registered");
}

static void test_add_and_intents(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);

    check(dc_registry_add(&r, &FEAT_RANKS), "add ranks");
    check(dc_registry_add(&r, &FEAT_TICKETS), "add tickets");
    check(r.count == 2, "count is 2");

    uint32_t intents = dc_registry_intents(&r);
    check(intents == (DC_INTENT_GUILDS | DC_INTENT_GUILD_MESSAGES), "intents unioned");
    check((intents & DC_INTENT_MESSAGE_CONTENT) == 0,
          "no MESSAGE_CONTENT without the spam feature");
}

static void test_message_content_opt_in(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    dc_registry_add(&r, &FEAT_RANKS);
    dc_registry_add(&r, &FEAT_TICKETS);

    uint32_t before = dc_registry_intents(&r);
    check((before & DC_INTENT_MESSAGE_CONTENT) == 0, "privileged intent absent by default");

    dc_registry_add(&r, &FEAT_SPAM);
    uint32_t after = dc_registry_intents(&r);
    check((after & DC_INTENT_MESSAGE_CONTENT) != 0, "privileged intent only when opted in");
}

static void test_duplicate_and_overflow(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);

    check(dc_registry_add(&r, &FEAT_RANKS), "first add");
    check(!dc_registry_add(&r, &FEAT_RANKS), "duplicate refused");
    check(r.count == 1, "count unchanged after duplicate");

    check(!dc_registry_add(&r, NULL), "NULL refused");

    for (int i = 0; i < 30; i++) {
        char name[16];
        snprintf(name, sizeof(name), "tmp%d", i);
        DiscordFeature tmp = { name, 0, 0, NULL, NULL, 0, NULL, NULL };
        dc_registry_add(&r, &tmp);
    }
    check(r.count == DC_FEATURE_MAX, "registry capped at MAX");
    check(dc_registry_has(&r, "tmp0"), "first filler registered");
    check(dc_registry_has(&r, "tmp22"), "last accepted filler registered");
    check(!dc_registry_has(&r, "tmp29"), "features past the cap are refused");
    check(!dc_registry_has(&r, ""), "empty name refused");
}

static void test_long_name(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);

    char longname[64];
    memset(longname, 'x', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';

    DiscordFeature f = { longname, 0, 0, NULL, NULL, 0, NULL, NULL };
    check(!dc_registry_add(&r, &f), "over-long feature name refused");
    check(r.count == 0, "nothing registered on rejection");
}

static void test_dispatch(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    dc_registry_add(&r, &FEAT_RANKS);
    dc_registry_add(&r, &FEAT_TICKETS);

    msg_hits = 0;
    last_feature = NULL;

    DiscordMessage m;
    memset(&m, 0, sizeof(m));
    m.author_id = 42;

    dc_registry_dispatch_message(&r, &m);
    check(msg_hits == 2, "message dispatched to every feature");
    check(strcmp(last_feature, "tickets") == 0, "dispatch order preserved");
}

static void test_dispatch_interaction(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    dc_registry_add(&r, &FEAT_RANKS);
    dc_registry_add(&r, &FEAT_CAPTCHA);

    int_hits = 0;
    DiscordInteraction it;
    memset(&it, 0, sizeof(it));
    it.type = 5;

    dc_registry_dispatch_interaction(&r, &it);
    check(int_hits == 1, "only features with a handler are called");
}

static void test_has(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    dc_registry_add(&r, &FEAT_RANKS);

    check(dc_registry_has(&r, "ranks"), "finds registered feature");
    check(!dc_registry_has(&r, "captcha"), "does not find unregistered");
    check(!dc_registry_has(&r, NULL), "NULL name is safe");
    check(!dc_registry_has(&r, ""), "empty name is safe");
}

static void test_isolation(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    dc_registry_add(&r, &FEAT_CAPTCHA);

    msg_hits = 0;
    DiscordMessage m;
    memset(&m, 0, sizeof(m));

    dc_registry_dispatch_message(&r, &m);
    check(msg_hits == 0, "feature without a message handler is never called");
    check(dc_registry_intents(&r) == 0, "captcha needs no intents at all");
}

static void test_unknown_command_is_refused(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    dc_registry_add(&r, &FEAT_RANKS);

    int_hits = 0;
    DiscordInteraction it;
    memset(&it, 0, sizeof(it));
    snprintf(it.command, sizeof(it.command), "definitely-not-a-command");

    check(!dc_registry_route_interaction(&r, &it), "unknown command refused");
    check(int_hits == 0, "no feature handler ran for an unknown command");
    check(dc_registry_find_command(&r, "definitely-not-a-command") == NULL,
          "lookup returns NULL for unknown");
}

static void test_known_command_routes(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    dc_registry_add(&r, &FEAT_CAPTCHA);

    int_hits = 0;
    DiscordInteraction it;
    memset(&it, 0, sizeof(it));
    snprintf(it.command, sizeof(it.command), "captcha");

    check(dc_registry_route_interaction(&r, &it), "known command routed");
    check(int_hits == 1, "exactly one handler ran");
}

static void test_fanout_replaced_by_single_owner(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    dc_registry_add(&r, &FEAT_CAPTCHA);
    dc_registry_add(&r, &FEAT_TICKETS_INTERACTION);

    int_hits = 0;
    last_feature = NULL;

    DiscordInteraction it;
    memset(&it, 0, sizeof(it));
    snprintf(it.command, sizeof(it.command), "captcha");

    check(dc_registry_route_interaction(&r, &it), "captcha routed among two handlers");
    check(int_hits == 1, "only the owning handler runs, not every handler");
    check(last_feature == NULL, "the handler of the wrong command did not fire");
}

static void test_empty_registry_refuses_everything(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);

    int_hits = 0;
    DiscordInteraction it;
    memset(&it, 0, sizeof(it));
    snprintf(it.command, sizeof(it.command), "captcha");

    check(dc_registry_find_command(&r, "captcha") == NULL, "nothing to find in an empty registry");
    check(!dc_registry_route_interaction(&r, &it), "empty registry refuses the command");
    check(int_hits == 0, "empty registry runs no handler");
}

static void test_lookup_guards(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    dc_registry_add(&r, &FEAT_RANKS);

    check(dc_registry_find_command(&r, "ranks") != NULL, "lookup finds a registered command");
    check(dc_registry_find_command(&r, "ranks") == &FEAT_RANKS,
          "lookup returns the registered feature itself");
    check(dc_registry_find_command(&r, "Rank") == NULL, "lookup refuses the wrong case");
    check(dc_registry_find_command(&r, "rank") == NULL, "lookup refuses a prefix");
    check(dc_registry_find_command(&r, "ranks2") == NULL, "lookup refuses a superstring");
    check(dc_registry_find_command(&r, "") == NULL, "empty command matches nothing");
    check(dc_registry_find_command(&r, NULL) == NULL, "NULL command is safe");
}

static void test_matching_is_case_sensitive(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    dc_registry_add(&r, &FEAT_CAPTCHA);

    int_hits = 0;
    DiscordInteraction it;
    memset(&it, 0, sizeof(it));
    snprintf(it.command, sizeof(it.command), "Captcha");

    check(dc_registry_find_command(&r, "Captcha") == NULL, "mis-cased name is not found");
    check(!dc_registry_route_interaction(&r, &it), "mis-cased command is refused");
    check(int_hits == 0, "mis-cased command reaches no handler");

    snprintf(it.command, sizeof(it.command), "captcha");
    check(dc_registry_route_interaction(&r, &it), "canonical lower case still routes");
    check(int_hits == 1, "canonical command runs its handler once");
}

static void test_overlong_and_non_ascii_commands_cannot_overrun(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    dc_registry_add(&r, &FEAT_RANKS);
    dc_registry_add(&r, &FEAT_CAPTCHA);

    int_hits = 0;
    DiscordInteraction it;
    memset(&it, 0, sizeof(it));

    memset(it.command, 'z', sizeof(it.command) - 1);
    it.command[sizeof(it.command) - 1] = '\0';
    check(!dc_registry_route_interaction(&r, &it), "a command filling it->command is refused");
    check(int_hits == 0, "over-long command reaches no handler");

    memcpy(it.command, "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9", 14);
    it.command[14] = '\0';
    check(dc_registry_find_command(&r, it.command) == NULL, "non-ASCII name is not found");
    check(!dc_registry_route_interaction(&r, &it), "non-ASCII command is refused");
    check(int_hits == 0, "non-ASCII command reaches no handler");

    memset(it.command, 0, sizeof(it.command));
    check(dc_registry_find_command(&r, it.command) == NULL, "an all-zero command matches nothing");
    check(!dc_registry_route_interaction(&r, &it), "an all-zero command is refused");
    check(int_hits == 0, "an all-zero command reaches no handler");
}

static DiscordFeature mutable_feature = { "captcha", 0, 0, NULL, on_int_captcha, 0, NULL, NULL };

static void test_lookup_uses_the_copied_name_table(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    DiscordFeature before = mutable_feature;

    check(dc_registry_add(&r, &mutable_feature), "register a feature whose name can be rewritten");
    mutable_feature.name = "pwned";
    check(strcmp(mutable_feature.name, "captcha") != 0,
          "the feature's name was rewritten after registration");

    int_hits = 0;
    DiscordInteraction it;
    memset(&it, 0, sizeof(it));
    snprintf(it.command, sizeof(it.command), "captcha");

    check(dc_registry_find_command(&r, "captcha") != NULL,
          "the registry matches the name it copied at registration time");
    check(dc_registry_route_interaction(&r, &it), "routing uses the copied name, not the feature's");
    check(int_hits == 1, "the handler behind the copied name ran");
    check(dc_registry_find_command(&r, "pwned") == NULL,
          "a name assigned after registration does not become routable");

    mutable_feature = before;
}

static void test_owner_is_the_feature_past_index_zero(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    dc_registry_add(&r, &FEAT_CAPTCHA);
    dc_registry_add(&r, &FEAT_TICKETS_INTERACTION);
    dc_registry_add(&r, &FEAT_RANKS_INTERACTION);

    check(dc_registry_find_command(&r, "ranks") == &FEAT_RANKS_INTERACTION,
          "the owner is the entry stored past index zero");
    check(dc_registry_find_command(&r, "ranks") != &FEAT_CAPTCHA,
          "the owner is not the first entry that happens to carry a handler");

    int_hits = 0;
    captcha_int_hits = 0;
    tickets_int_hits = 0;
    ranks_int_hits = 0;
    last_feature = NULL;

    DiscordInteraction it;
    memset(&it, 0, sizeof(it));
    snprintf(it.command, sizeof(it.command), "ranks");

    check(dc_registry_route_interaction(&r, &it), "the command at index 2 routes");
    check(ranks_int_hits == 1, "the owning handler ran exactly once");
    check(captcha_int_hits == 0, "the handler at index 0 did not run at all");
    check(tickets_int_hits == 0, "the handler at index 1 did not run at all");
    check(int_hits == 1, "no other feature received the interaction");
    check(last_feature != NULL && strcmp(last_feature, "ranks") == 0,
          "the handler that ran is the one that owns the command");
}

static void test_ticket_owner_past_index_zero_is_identified(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    dc_registry_add(&r, &FEAT_CAPTCHA);
    dc_registry_add(&r, &FEAT_RANKS_INTERACTION);
    dc_registry_add(&r, &FEAT_TICKETS_INTERACTION);

    int_hits = 0;
    captcha_int_hits = 0;
    tickets_int_hits = 0;
    ranks_int_hits = 0;
    last_feature = NULL;

    DiscordInteraction it;
    memset(&it, 0, sizeof(it));
    snprintf(it.command, sizeof(it.command), "tickets");

    check(dc_registry_route_interaction(&r, &it), "the command at index 2 routes");
    check(tickets_int_hits == 1, "the owning handler ran exactly once");
    check(captcha_int_hits == 0, "the handler at index 0 did not run at all");
    check(ranks_int_hits == 0, "the handler at index 1 did not run at all");
    check(int_hits == 1, "no other feature received the interaction");
    check(last_feature != NULL && strcmp(last_feature, "tickets") == 0,
          "the handler that ran is the one that owns the command");
}

static void test_owner_without_an_interaction_handler_still_routes(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    dc_registry_add(&r, &FEAT_CAPTCHA);
    dc_registry_add(&r, &FEAT_RANKS);

    check(FEAT_RANKS.on_interaction == NULL, "the fixture models a message-only feature");
    check(dc_registry_find_command(&r, "ranks") == &FEAT_RANKS,
          "the message-only feature still owns its command");

    int_hits = 0;
    captcha_int_hits = 0;
    ranks_int_hits = 0;

    DiscordInteraction it;
    memset(&it, 0, sizeof(it));
    snprintf(it.command, sizeof(it.command), "ranks");

    check(dc_registry_route_interaction(&r, &it),
          "a matched feature with no handler still reports the command routed");
    check(ranks_int_hits == 0, "an absent handler is not invoked");
    check(captcha_int_hits == 0, "the absence of one handler does not divert the command");
    check(int_hits == 0, "routing a command no handler wanted runs nothing");
}

static void test_route_null_arguments(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    dc_registry_add(&r, &FEAT_CAPTCHA);

    int_hits = 0;
    captcha_int_hits = 0;

    DiscordInteraction it;
    memset(&it, 0, sizeof(it));
    snprintf(it.command, sizeof(it.command), "captcha");

    check(!dc_registry_route_interaction(NULL, &it), "a NULL registry refuses the command");
    check(!dc_registry_route_interaction(&r, NULL), "a NULL interaction is refused");
    check(!dc_registry_route_interaction(NULL, NULL), "two NULL arguments are refused");
    check(int_hits == 0, "no handler ran for any NULL argument");
    check(captcha_int_hits == 0, "the registered handler never ran for a NULL argument");
}

static void test_gateway_bytes_route_to_the_owning_feature(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    dc_registry_add(&r, &FEAT_CAPTCHA);
    dc_registry_add(&r, &FEAT_TICKETS_INTERACTION);

    int_hits = 0;
    captcha_int_hits = 0;
    tickets_int_hits = 0;

    DiscordInteraction it;
    memset(&it, 0, sizeof(it));
    static const char raw[] = "tickets";
    check(dc_interaction_set_command(&it, raw, sizeof(raw) - 1),
          "the gateway bytes land in it->command");
    check(strcmp(it.command, "tickets") == 0, "the parsed command is the canonical lower case name");

    check(dc_registry_route_interaction(&r, &it), "the parsed command routes");
    check(tickets_int_hits == 1, "the owning handler ran exactly once");
    check(captcha_int_hits == 0, "the other handler did not run at all");
    check(int_hits == 1, "only one feature received the parsed command");

    memset(&it, 0, sizeof(it));
    check(dc_interaction_set_command(&it, "TICKETS", 7),
          "a mis-cased name from a hand written request still parses");
    check(!dc_registry_route_interaction(&r, &it),
          "a mis-cased parsed command is refused, the match stays byte exact");
    check(tickets_int_hits == 1, "the mis-cased command reached no handler");
    check(int_hits == 1, "the mis-cased command added no handler call");
}

static void test_component_routes_by_custom_id_prefix(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    dc_registry_add(&r, &FEAT_CAPTCHA);
    dc_registry_add(&r, &FEAT_TICKETS_INTERACTION);

    int_hits = 0;
    captcha_int_hits = 0;
    tickets_int_hits = 0;

    DiscordInteraction it;
    memset(&it, 0, sizeof(it));
    snprintf(it.custom_id, sizeof(it.custom_id), "tickets:close:123");

    check(dc_registry_route_component(&r, &it), "component routes by prefix");
    check(tickets_int_hits == 1, "the owning handler ran exactly once");
    check(captcha_int_hits == 0, "the other handler did not run");
    check(int_hits == 1, "only one feature received the component");
}

static const char MODAL_DOC[] = "{\"type\":9}";

static const DiscordFeature FEAT_MODAL = {
    "announce", 0, 0, NULL, on_int_tickets, DC_FEATURE_WANTS_MODAL, MODAL_DOC, NULL
};

static void test_modal_fields_ride_along(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    check(dc_registry_add(&r, &FEAT_MODAL), "modal feature registers");
    {
        const DiscordFeature *f = dc_registry_find_command(&r, "announce");
        check(f != NULL, "modal feature found");
        check((f->flags & DC_FEATURE_WANTS_MODAL) != 0, "modal flag kept");
        check(f->modal_json != NULL && strcmp(f->modal_json, MODAL_DOC) == 0,
              "modal document borrowed intact");
    }
    {
        DiscordInteraction it;
        memset(&it, 0, sizeof(it));
        snprintf(it.custom_id, sizeof(it.custom_id), "announce:send");
        tickets_int_hits = 0;
        check(dc_registry_route_component(&r, &it), "modal submit routes");
        check(tickets_int_hits == 1, "modal submit runs the owner");
    }
}

static void test_component_refusals(void)
{
    DiscordFeatureRegistry r;
    dc_registry_init(&r);
    dc_registry_add(&r, &FEAT_CAPTCHA);
    dc_registry_add(&r, &FEAT_RANKS);

    int_hits = 0;
    DiscordInteraction it;
    memset(&it, 0, sizeof(it));

    snprintf(it.custom_id, sizeof(it.custom_id), "nope:action");
    check(!dc_registry_route_component(&r, &it), "unknown prefix refused");

    memset(&it, 0, sizeof(it));
    check(!dc_registry_route_component(&r, &it), "empty custom_id refused");

    memset(&it, 0, sizeof(it));
    snprintf(it.custom_id, sizeof(it.custom_id), ":action");
    check(!dc_registry_route_component(&r, &it), "empty prefix refused");

    memset(&it, 0, sizeof(it));
    snprintf(it.command, sizeof(it.command), "captcha");
    snprintf(it.custom_id, sizeof(it.custom_id), "captcha:ok");
    check(!dc_registry_route_component(&r, &it),
          "a command-shaped interaction never takes the component path");

    memset(&it, 0, sizeof(it));
    snprintf(it.custom_id, sizeof(it.custom_id), "ranks:show");
    check(dc_registry_route_component(&r, &it),
          "a matched message-only feature still reports routed");
    check(int_hits == 0, "its absent handler is not invoked");

    check(!dc_registry_route_component(NULL, &it), "NULL registry refused");
    check(!dc_registry_route_component(&r, NULL), "NULL interaction refused");
}

int main(void)
{
    test_empty();
    test_add_and_intents();
    test_message_content_opt_in();
    test_duplicate_and_overflow();
    test_long_name();
    test_dispatch();
    test_dispatch_interaction();
    test_has();
    test_isolation();
    test_unknown_command_is_refused();
    test_known_command_routes();
    test_fanout_replaced_by_single_owner();
    test_empty_registry_refuses_everything();
    test_lookup_guards();
    test_matching_is_case_sensitive();
    test_overlong_and_non_ascii_commands_cannot_overrun();
    test_lookup_uses_the_copied_name_table();
    test_owner_is_the_feature_past_index_zero();
    test_ticket_owner_past_index_zero_is_identified();
    test_owner_without_an_interaction_handler_still_routes();
    test_route_null_arguments();
    test_gateway_bytes_route_to_the_owning_feature();
    test_component_routes_by_custom_id_prefix();
    test_component_refusals();
    test_modal_fields_ride_along();

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}