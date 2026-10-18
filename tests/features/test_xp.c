#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "discord_xp.h"
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

/* Stubs for transport functions so test links cleanly on host */
static DcBotState s_mock_bot_state;
static DcJob s_mock_jobs[16];
static uint32_t s_mock_jobs_count = 0;
static char s_last_reply[160];

DcBotState *discord_bot_state(void)
{
    return &s_mock_bot_state;
}

bool discord_job_push(const DcJob *job)
{
    if (job != NULL && s_mock_jobs_count < 16u) {
        s_mock_jobs[s_mock_jobs_count++] = *job;
        return true;
    }
    return false;
}

int discord_interaction_reply(const DiscordInteraction *it, const char *content)
{
    (void)it;
    if (content != NULL) {
        snprintf(s_last_reply, sizeof(s_last_reply), "%s", content);
    }
    return 0;
}

/* ----------------- Sub-domain 1: Curve Tests ----------------- */
static void test_xp_curve(void)
{
    check(dc_xp_required_for_level(0) == 0u, "level 0 requires 0 XP");
    check(dc_xp_required_for_level(1) == 100u, "level 1 requires 100 XP (50*1*2)");
    check(dc_xp_required_for_level(2) == 300u, "level 2 requires 300 XP (50*2*3)");
    check(dc_xp_required_for_level(3) == 600u, "level 3 requires 600 XP (50*3*4)");
    check(dc_xp_required_for_level(5) == 1500u, "level 5 requires 1500 XP");

    /* Monotonicity check */
    bool monotonic = true;
    for (uint8_t l = 1; l < DC_XP_MAX_LEVEL; l++) {
        if (dc_xp_required_for_level(l) >= dc_xp_required_for_level((uint8_t)(l + 1u))) {
            monotonic = false;
            break;
        }
    }
    check(monotonic, "XP curve is strictly monotonic");

    /* Level for XP mapping */
    check(dc_xp_level_for_xp(0) == 0u, "0 XP is level 0");
    check(dc_xp_level_for_xp(99) == 0u, "99 XP is level 0");
    check(dc_xp_level_for_xp(100) == 1u, "100 XP is level 1");
    check(dc_xp_level_for_xp(299) == 1u, "299 XP is level 1");
    check(dc_xp_level_for_xp(300) == 2u, "300 XP is level 2");
    check(dc_xp_level_for_xp(1500) == 5u, "1500 XP is level 5");

    /* Gain range check */
    bool gain_bounds_ok = true;
    for (uint32_t s = 0; s < 100; s++) {
        uint32_t g = dc_xp_calc_gain(s);
        if (g < DC_XP_GAIN_MIN || g > DC_XP_GAIN_MAX) {
            gain_bounds_ok = false;
            break;
        }
    }
    check(gain_bounds_ok, "XP gain is always within 15..25 range");

    /* Level up evaluation */
    uint8_t nlvl = 0;
    check(!dc_xp_check_level_up(0, 50, 0, &nlvl), "50 XP does not trigger level 1");
    check(nlvl == 0, "level remains 0");
    check(dc_xp_check_level_up(90, 20, 0, &nlvl), "110 XP triggers level 1");
    check(nlvl == 1, "new level is 1");
}

/* ----------------- Sub-domain 2: Cooldown & Daily Cap Tests ----------------- */
static void test_xp_cooldown_and_daily_cap(void)
{
    DcXpCooldownTable tbl;
    dc_xp_cooldown_init(&tbl, 5000u, 100u); /* 5s conversational debounce, 100 XP daily cap */

    uint32_t awarded = 0u;

    /* Message 1 at t=0s -> allowed (20 XP) */
    check(dc_xp_cooldown_try_acquire(&tbl, 1001, 1000u, 20u, &awarded), "message 1 acquires XP");
    check(awarded == 20u, "awarded full 20 XP");
    check(dc_xp_cooldown_get_daily_xp(&tbl, 1001, 1000u) == 20u, "daily xp accumulator is 20");

    /* Message 2 at t=2s (rapid flood < 5s) -> debounced */
    check(!dc_xp_cooldown_try_acquire(&tbl, 1001, 3000u, 20u, &awarded), "rapid-fire < 5s debounced");

    /* Message 3 at t=7s (natural conversational reply after 6s) -> allowed */
    check(dc_xp_cooldown_try_acquire(&tbl, 1001, 7000u, 25u, &awarded), "message 3 in conversation allowed");
    check(awarded == 25u, "awarded 25 XP");
    check(dc_xp_cooldown_get_daily_xp(&tbl, 1001, 7000u) == 45u, "daily xp accumulator is 45");

    /* Message 4 at t=15s -> allowed (+25 XP -> 70 XP) */
    check(dc_xp_cooldown_try_acquire(&tbl, 1001, 15000u, 25u, &awarded), "message 4 in conversation allowed");
    check(dc_xp_cooldown_get_daily_xp(&tbl, 1001, 15000u) == 70u, "daily xp accumulator is 70");

    /* Message 5 at t=25s -> allowed (+25 XP -> 95 XP) */
    check(dc_xp_cooldown_try_acquire(&tbl, 1001, 25000u, 25u, &awarded), "message 5 in conversation allowed");
    check(dc_xp_cooldown_get_daily_xp(&tbl, 1001, 25000u) == 95u, "daily xp accumulator is 95");

    /* Message 6 at t=35s requests 20 XP, but daily cap is 100 -> clamped to 5 XP! */
    check(dc_xp_cooldown_try_acquire(&tbl, 1001, 35000u, 20u, &awarded), "message 6 clamped at daily cap");
    check(awarded == 5u, "granted exactly 5 XP to reach 100 cap");
    check(dc_xp_cooldown_get_daily_xp(&tbl, 1001, 35000u) == 100u, "daily xp saturated at 100");

    /* Message 7 at t=60s on same day -> rejected because daily cap is 100 */
    check(!dc_xp_cooldown_try_acquire(&tbl, 1001, 60000u, 20u, &awarded), "message after daily cap saturated rejected");

    /* Day rollover: advance time to Day 2 (> 86,400,000 ms) */
    uint64_t next_day_ms = 86400000ULL + 5000ULL;
    check(dc_xp_cooldown_try_acquire(&tbl, 1001, next_day_ms, 20u, &awarded), "daily cap resets automatically on day rollover");
    check(awarded == 20u, "awarded full XP on new day");
    check(dc_xp_cooldown_get_daily_xp(&tbl, 1001, next_day_ms) == 20u, "daily accumulator reset to 20 for new day");
}

/* ----------------- Sub-domain 3: Roles Tests ----------------- */
static void test_xp_roles(void)
{
    DcXpRolesTable tbl;
    dc_xp_roles_init(&tbl);

    check(dc_xp_roles_bind(&tbl, 5, 888888ULL), "bind role to level 5");
    check(dc_xp_roles_bind(&tbl, 10, 999999ULL), "bind role to level 10");

    check(dc_xp_roles_find(&tbl, 5) == 888888ULL, "role found for level 5");
    check(dc_xp_roles_find(&tbl, 10) == 999999ULL, "role found for level 10");
    check(dc_xp_roles_find(&tbl, 3) == 0ULL, "unbound level has 0 role");

    /* Collection across level transitions */
    uint64_t roles[4];
    uint8_t count = dc_xp_roles_collect_rewards(&tbl, 0, 4, roles, 4);
    check(count == 0, "jump 0->4 awards no roles");

    count = dc_xp_roles_collect_rewards(&tbl, 4, 5, roles, 4);
    check(count == 1 && roles[0] == 888888ULL, "jump 4->5 awards level 5 role");

    /* Multi-level jump */
    count = dc_xp_roles_collect_rewards(&tbl, 4, 12, roles, 4);
    check(count == 2, "jump 4->12 awards two milestone roles");
    check(roles[0] == 888888ULL && roles[1] == 999999ULL, "both role IDs collected in order");
}

/* ----------------- Sub-domain 4: Orchestrator Tests ----------------- */
static void test_xp_orchestrator(void)
{
    DcXpSystem *sys = dc_xp_system_get();
    dc_xp_system_init(sys, 5000u, 1000u, true);
    dc_xp_system_bind_role(sys, 1, 777111ULL);

    memset(&s_mock_bot_state, 0, sizeof(s_mock_bot_state));
    s_mock_jobs_count = 0;

    /* Bot messages ignored */
    DiscordMessage bot_msg = {
        .author_id = 9999,
        .guild_id = 1234,
        .channel_id = 5678,
        .flags = DC_MSG_FLAG_BOT
    };
    DcXpEvent evt;
    check(!dc_xp_process_message(sys, &s_mock_bot_state, &bot_msg, 1000u, 42u, &evt),
          "bot message ignored by XP engine");

    /* Valid user message */
    DiscordMessage user_msg = {
        .author_id = 4242,
        .guild_id = 1234,
        .channel_id = 5678,
        .flags = 0u
    };
    check(dc_xp_process_message(sys, &s_mock_bot_state, &user_msg, 1000u, 42u, &evt),
          "valid member message awards XP");
    check(evt.new_xp >= DC_XP_GAIN_MIN && evt.new_xp <= DC_XP_GAIN_MAX, "XP added to user state");
    check(!evt.leveled_up, "not leveled up yet");

    /* Fast-forward and award enough XP to cross level 1 threshold (100 XP) */
    DcRankUser *rank = dc_state_rank_user(&s_mock_bot_state, 4242);
    rank->xp = 95u; /* 5 XP away from level 1 */

    check(dc_xp_process_message(sys, &s_mock_bot_state, &user_msg, 8000u, 42u, &evt),
          "second message crosses level threshold");
    check(evt.leveled_up, "leveled up to level 1");
    check(evt.new_level == 1u, "new level is 1");
    check(evt.roles_count == 1u, "one role reward collected");
    check(evt.roles_awarded[0] == 777111ULL, "role 777111 awarded");

    /* Build and verify role job */
    DcJob role_job;
    check(dc_xp_build_role_job(&evt, 0, &role_job), "role job built successfully");
    check(role_job.kind == DC_JOB_ADD_ROLE, "job kind is DC_JOB_ADD_ROLE");
    check(role_job.guild_id == 1234, "guild id matches");
    check(role_job.user_id == 4242, "user id matches");
    check(strcmp(role_job.payload, "777111") == 0, "role snowflake encoded as digit string");

    /* Build and verify level announcement job */
    DcJob ann_job;
    check(dc_xp_build_announce_job(&evt, &ann_job), "announce job built successfully");
    check(ann_job.kind == DC_JOB_SEND_MESSAGE, "job kind is DC_JOB_SEND_MESSAGE");
    check(ann_job.channel_id == 5678, "channel id matches");
    check(strstr(ann_job.payload, "level 1") != NULL, "message mentions level 1");
    check(strstr(ann_job.payload, "4242") != NULL, "message mentions user id");

    /* Interaction /rank command test */
    DiscordInteraction it = {
        .id = (1000ULL << 22u),
        .user_id = 4242,
        .command = "rank"
    };
    dc_xp_feature_get()->on_interaction(&it);
    check(strstr(s_last_reply, "Level: 1") != NULL, "rank command reports current level");
    check(strstr(s_last_reply, "Total XP:") != NULL, "rank command reports total XP");
    check(strstr(s_last_reply, "Today:") != NULL, "rank command reports daily XP progress");

    /* Interaction /setdailyxp command test */
    DiscordInteraction set_it = {
        .command = "setdailyxp",
        .value_count = 1,
        .values = { "2500" }
    };
    dc_xp_feature_get()->on_interaction(&set_it);
    check(strstr(s_last_reply, "set to 2500") != NULL, "setdailyxp confirms new cap");
    check(dc_xp_system_get()->cooldowns.daily_cap == 2500u, "system daily cap reconfigured");

    /* Test on_message with restricted image from level 0 user */
    s_mock_jobs_count = 0u;
    DiscordMessage bad_upload = {
        .message_id = (2000ULL << 22u),
        .author_id = 8888,
        .guild_id = 1234,
        .channel_id = 5678,
        .flags = DC_MSG_FLAG_IMAGE
    };
    dc_xp_feature_get()->on_message(&bad_upload);
    check(s_mock_jobs_count == 2u, "restricted upload triggers delete and warn DM jobs");
    check(s_mock_jobs[0].kind == DC_JOB_DELETE_MESSAGE, "first job deletes message");
    check(s_mock_jobs[1].kind == DC_JOB_SEND_DM, "second job warns author via DM");
    DcRankUser *spammer_rank = dc_state_rank_user(&s_mock_bot_state, 8888);
    check(spammer_rank->xp == 0u, "no XP awarded for restricted message");

    /* Test second image upload immediately (flood): delete message but do NOT spam DMs */
    bad_upload.message_id = (2001ULL << 22u);
    dc_xp_feature_get()->on_message(&bad_upload);
    check(s_mock_jobs_count == 3u, "flood upload triggers delete job but NO duplicate DM");
    check(s_mock_jobs[2].kind == DC_JOB_DELETE_MESSAGE, "third job deletes message");

    /* Interaction /guardlevel command test */
    DiscordInteraction guard_it = {
        .command = "guardlevel",
        .value_count = 1,
        .values = { "5" }
    };
    dc_xp_feature_get()->on_interaction(&guard_it);
    check(strstr(s_last_reply, "set to 5") != NULL, "guardlevel confirms level 5");
    check(dc_xp_system_get()->guard.min_level == 5u, "system guard level updated");

    /* Interaction /setlevel command test (admin sets user level) */
    DiscordInteraction setlvl_it = {
        .guild_id = 1234,
        .command = "setlevel",
        .value_count = 2,
        .values = { "<@4242>", "5" }
    };
    dc_xp_feature_get()->on_interaction(&setlvl_it);
    check(strstr(s_last_reply, "Set user <@4242> to Level 5") != NULL, "setlevel confirms assignment in English");
    check(rank->level == 5u, "user level set to 5");
    check(rank->xp == 1500u, "user XP set to level 5 requirement (1500 XP)");

    /* Test strike escalation to timeout (3 strikes in 60s) */
    s_mock_jobs_count = 0u;
    bad_upload.author_id = 7777;
    bad_upload.message_id = (3000ULL << 22u);
    dc_xp_feature_get()->on_message(&bad_upload); /* Strike 1: delete + warn DM */
    check(s_mock_jobs_count == 2u, "strike 1 triggers delete + warn DM");

    bad_upload.message_id = (3005ULL << 22u);
    dc_xp_feature_get()->on_message(&bad_upload); /* Strike 2: delete */
    check(s_mock_jobs_count == 3u, "strike 2 triggers delete only");

    bad_upload.message_id = (3010ULL << 22u);
    dc_xp_feature_get()->on_message(&bad_upload); /* Strike 3: timeout! */
    check(s_mock_jobs_count == 6u, "strike 3 triggers delete, timeout job, and timeout notification DM");
    check(s_mock_jobs[3].kind == DC_JOB_DELETE_MESSAGE, "job 4 is delete message");
    check(s_mock_jobs[4].kind == DC_JOB_TIMEOUT_USER, "job 5 is timeout user");
    check(s_mock_jobs[4].aux_id == 300u, "timeout duration is 300 seconds (5 min)");
    check(s_mock_jobs[5].kind == DC_JOB_SEND_DM, "job 6 is timeout DM");
    check(strstr(s_mock_jobs[5].payload, "timed out for 5 min") != NULL, "DM mentions 5 min timeout");

    /* Interaction /guardinvites command test */
    DiscordInteraction inv_it = {
        .command = "guardinvites",
        .value_count = 1,
        .values = { "7" }
    };
    dc_xp_feature_get()->on_interaction(&inv_it);
    check(strstr(s_last_reply, "set to 7") != NULL, "guardinvites confirms level 7");
    check(dc_xp_system_get()->guard.min_invite_level == 7u, "guard invite level updated to 7");

    /* Interaction /leaderboard command test */
    DiscordInteraction lb_it = {
        .command = "leaderboard"
    };
    dc_xp_feature_get()->on_interaction(&lb_it);
    check(strstr(s_last_reply, "Top Members Leaderboard:") != NULL, "leaderboard header present");
    check(strstr(s_last_reply, "4242") != NULL, "leaderboard lists top user 4242");
    check(strstr(s_last_reply, "Level 5") != NULL, "leaderboard displays user level 5");

    /* Interaction /setlevelrole command test */
    DiscordInteraction role_it = {
        .command = "setlevelrole",
        .value_count = 2,
        .values = { "3", "<@&888333111>" }
    };
    dc_xp_feature_get()->on_interaction(&role_it);
    check(strstr(s_last_reply, "Bound Role <@&888333111> to Level 3") != NULL, "setlevelrole confirms binding");
    check(dc_xp_roles_find(&dc_xp_system_get()->roles, 3u) == 888333111ULL, "role bound in roles table");

    /* Interaction /slowmode command test */
    DiscordInteraction slow_it = {
        .command = "slowmode",
        .value_count = 1,
        .values = { "30" }
    };
    dc_xp_feature_get()->on_interaction(&slow_it);
    check(strstr(s_last_reply, "slowmode set to 30 seconds") != NULL, "slowmode confirms 30 seconds");

    slow_it.values[0][0] = '9';
    slow_it.values[0][1] = '9';
    slow_it.values[0][2] = '9';
    slow_it.values[0][3] = '9';
    slow_it.values[0][4] = '9';
    slow_it.values[0][5] = '\0';
    dc_xp_feature_get()->on_interaction(&slow_it);
    check(strstr(s_last_reply, "slowmode set to 21600 seconds") != NULL, "slowmode clamped to Discord 21600 max");

    /* Interaction /automod command test */
    DiscordInteraction am_it = {
        .command = "automod"
    };
    dc_xp_feature_get()->on_interaction(&am_it);
    check(strstr(s_last_reply, "AutoMod native rules provisioned") != NULL, "automod setup confirms native rule creation");

    /* Ephemeral flag verification */
    check((dc_xp_feature_get()->flags & DC_FEATURE_EPHEMERAL) != 0u, "XP feature replies are ephemeral");

    /* Comprehensive Registry & Routing Verification */
    DiscordFeatureRegistry reg;
    dc_registry_init(&reg);
    check(dc_xp_register(&reg), "dc_xp_register adds all XP slash commands");
    check(dc_registry_has(&reg, "rank"), "registry contains /rank");
    check(dc_registry_has(&reg, "leaderboard"), "registry contains /leaderboard");
    check(dc_registry_has(&reg, "setlevel"), "registry contains /setlevel");
    check(dc_registry_has(&reg, "setlevelrole"), "registry contains /setlevelrole");
    check(dc_registry_has(&reg, "slowmode"), "registry contains /slowmode");
    check(dc_registry_has(&reg, "automod"), "registry contains /automod");

    /* Route /rank via registry */
    DiscordInteraction rank_it = {
        .id = (2500ULL << 22u),
        .user_id = 4242,
        .command = "rank"
    };
    check(dc_registry_route_interaction(&reg, &rank_it), "/rank routes cleanly via registry");
    check(strstr(s_last_reply, "Level: 5") != NULL, "/rank reports level 5");

    /* Route /rank with target user option (e.g. /rank @Shepai) */
    DiscordInteraction rank_target_it = {
        .id = (2501ULL << 22u),
        .user_id = 4242,
        .command = "rank",
        .value_count = 1,
        .values = { "<@999888777>" }
    };
    check(dc_registry_route_interaction(&reg, &rank_target_it), "/rank with target user routes");
    check(strstr(s_last_reply, "999888777") != NULL, "/rank displays target member ID");

    /* Route /setlevel with reversed arguments (level: 3, user: @999888777) */
    DiscordInteraction setlvl_rev = {
        .guild_id = 1234,
        .command = "setlevel",
        .value_count = 2,
        .values = { "3", "<@999888777>" }
    };
    check(dc_registry_route_interaction(&reg, &setlvl_rev), "/setlevel with swapped args routes");
    check(strstr(s_last_reply, "Set user <@999888777> to Level 3") != NULL, "reversed args parsed properly");
}

/* ----------------- Sub-domain 4: Media, Invites & Mention Guard Tests ----------------- */
static void test_xp_guard(void)
{
    /* 1. Link & Invite detection */
    const char *plain = "Hello this is a regular chat message";
    DiscordSlice s_plain = { .ptr = (const uint8_t *)plain, .len = (uint16_t)strlen(plain) };
    check(!dc_xp_guard_has_links(s_plain), "plain message has no links");
    check(!dc_xp_guard_has_invites(s_plain), "plain message has no invites");

    const char *with_https = "Check this project: https://github.com/Azertyuiop442/Plexus";
    DiscordSlice s_https = { .ptr = (const uint8_t *)with_https, .len = (uint16_t)strlen(with_https) };
    check(dc_xp_guard_has_links(s_https), "https URL detected");
    check(!dc_xp_guard_has_invites(s_https), "standard https URL is not an invite");

    const char *with_invite = "Join here discord.gg/abc12345";
    DiscordSlice s_invite = { .ptr = (const uint8_t *)with_invite, .len = (uint16_t)strlen(with_invite) };
    check(dc_xp_guard_has_invites(s_invite), "discord.gg invite detected");

    /* 2. Mention Spam Detection */
    const char *with_mentions = "Hello <@111> <@222> <@333> <@444> join us";
    DiscordSlice s_mentions = { .ptr = (const uint8_t *)with_mentions, .len = (uint16_t)strlen(with_mentions) };
    check(dc_xp_guard_count_mentions(s_mentions) > DC_XP_GUARD_MAX_MENTIONS, "mention spam (>3) detected");

    const char *with_everyone = "Important announcement @everyone look!";
    DiscordSlice s_everyone = { .ptr = (const uint8_t *)with_everyone, .len = (uint16_t)strlen(with_everyone) };
    check(dc_xp_guard_count_mentions(s_everyone) >= 99u, "@everyone mention detected");

    /* 3. Guard evaluation with separate invite threshold */
    DcXpGuard guard;
    dc_xp_guard_init(&guard, 3u, 5u, 30000ULL); /* Requires Level 3 for links/media, Level 5 for invites */

    DiscordMessage msg_text = { .flags = 0u, .content = s_plain };
    DiscordMessage msg_img  = { .flags = DC_MSG_FLAG_IMAGE, .content = s_plain };
    DiscordMessage msg_link = { .flags = 0u, .content = s_https };
    DiscordMessage msg_inv  = { .flags = 0u, .content = s_invite };

    check(!dc_xp_guard_is_restricted(&guard, &msg_text, 0u), "text message allowed at level 0");
    check(dc_xp_guard_is_restricted(&guard, &msg_img, 0u), "image message restricted at level 0");
    check(dc_xp_guard_is_restricted(&guard, &msg_link, 2u), "link message restricted at level 2");
    check(!dc_xp_guard_is_restricted(&guard, &msg_link, 3u), "link message allowed at level 3");
    check(dc_xp_guard_is_restricted(&guard, &msg_inv, 3u), "invite message restricted at level 3 (< 5)");
    check(!dc_xp_guard_is_restricted(&guard, &msg_inv, 5u), "invite message allowed at level 5");

    /* 4. Strike recording & Timeout job builders */
    DiscordId spammer = 9999ULL;
    check(dc_xp_guard_record_strike(&guard, spammer, 1000u) == 1u, "strike 1 recorded");
    check(dc_xp_guard_record_strike(&guard, spammer, 5000u) == 2u, "strike 2 recorded within window");
    check(dc_xp_guard_record_strike(&guard, spammer, 10000u) == 3u, "strike 3 recorded within window");

    DiscordMessage bad_msg = {
        .guild_id = 111,
        .channel_id = 222,
        .author_id = spammer,
        .message_id = 987654321ULL,
        .flags = DC_MSG_FLAG_IMAGE
    };
    DcJob to_job;
    check(dc_xp_guard_build_timeout_job(&bad_msg, 300u, &to_job), "timeout job built successfully");
    check(to_job.kind == DC_JOB_TIMEOUT_USER, "job kind is DC_JOB_TIMEOUT_USER");
    check(to_job.guild_id == 111, "timeout job guild_id matches");
    check(to_job.user_id == spammer, "timeout job user_id matches");
    check(to_job.aux_id == 300u, "timeout duration is 300s");

    DcJob to_dm;
    check(dc_xp_guard_build_timeout_dm_job(spammer, 5u, &to_dm), "timeout DM job built successfully");
    check(to_dm.kind == DC_JOB_SEND_DM, "job kind is DC_JOB_SEND_DM");
    check(strstr(to_dm.payload, "timed out for 5 min") != NULL, "timeout DM payload mentions 5 min");
}

/* ----------------- Sub-domain 5: Server-Side Delegated Features Tests ----------------- */
static void test_xp_server(void)
{
    /* 1. Slowmode validation & clamping */
    uint32_t clamped = 0u;
    check(!dc_xp_server_validate_slowmode(-1, &clamped), "negative slowmode rejected");
    check(dc_xp_server_validate_slowmode(0, &clamped) && clamped == 0u, "0 slowmode allowed (disabled)");
    check(dc_xp_server_validate_slowmode(120, &clamped) && clamped == 120u, "120s slowmode preserved");
    check(dc_xp_server_validate_slowmode(99999, &clamped) && clamped == DC_DISCORD_SLOWMODE_MAX,
          "excess slowmode clamped to 21600 (6 hours)");
    check(!dc_xp_server_validate_slowmode(10, NULL), "NULL output pointer safely rejected");

    /* 2. Slowmode JSON payload */
    char body[48];
    check(dc_xp_server_build_slowmode_body(60u, body, sizeof(body)), "slowmode payload built");
    check(strcmp(body, "{\"rate_limit_per_user\":60}") == 0, "slowmode JSON matches Discord format");
    check(!dc_xp_server_build_slowmode_body(60u, body, 10u), "tiny buffer safely refused");

    /* 3. AutoMod native mention raid rule */
    char am_buf[384];
    check(dc_xp_server_build_automod_mention_rule(3u, 300u, am_buf, sizeof(am_buf)),
          "automod mention rule built");
    check(strstr(am_buf, "\"mention_total_limit\":3") != NULL, "automod carries mention limit");
    check(strstr(am_buf, "\"duration_seconds\":300") != NULL, "automod carries 5 min timeout action");
    check(strstr(am_buf, "\"trigger_type\":5") != NULL, "automod trigger_type is 5 (MENTION_SPAM)");
    check(!dc_xp_server_build_automod_mention_rule(0u, 300u, am_buf, sizeof(am_buf)),
          "zero mention limit refused");
    check(!dc_xp_server_build_automod_mention_rule(3u, 300u, am_buf, 50u),
          "tiny buffer safely refused for automod rule");

    /* 4. Permission overwrite body (client-side upload lockout) */
    char perm_buf[80];
    uint64_t deny_media = DC_DISCORD_PERM_ATTACH_FILES | DC_DISCORD_PERM_EMBED_LINKS; /* 49152 */
    check(dc_xp_server_build_guard_overwrite_body(deny_media, 0ULL, perm_buf, sizeof(perm_buf)),
          "guard overwrite body built");
    check(strstr(perm_buf, "\"deny\":\"49152\"") != NULL, "deny bitmask is 49152 (0xC000)");
    check(strstr(perm_buf, "\"type\":0") != NULL, "type 0 targets role overwrite");
    check(!dc_xp_server_build_guard_overwrite_body(deny_media, 0ULL, perm_buf, 20u),
          "tiny buffer safely refused for permission overwrite");
}

int main(void)
{
    printf("== XP, Anti-Farm, Server Delegation, Guard & Role Rewards Tests ==\n");
    test_xp_curve();
    test_xp_cooldown_and_daily_cap();
    test_xp_roles();
    test_xp_guard();
    test_xp_server();
    test_xp_orchestrator();

    if (failures == 0) {
        printf("\nPASSED (0 failure(s))\n");
        return 0;
    } else {
        printf("\nFAILED (%d failure(s))\n", failures);
        return 1;
    }
}
