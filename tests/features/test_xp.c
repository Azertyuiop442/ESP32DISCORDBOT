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
    DcXpSystem sys;
    dc_xp_system_init(&sys, 5000u, 1000u, true);
    dc_xp_system_bind_role(&sys, 1, 777111ULL);

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
    check(!dc_xp_process_message(&sys, &s_mock_bot_state, &bot_msg, 1000u, 42u, &evt),
          "bot message ignored by XP engine");

    /* Valid user message */
    DiscordMessage user_msg = {
        .author_id = 4242,
        .guild_id = 1234,
        .channel_id = 5678,
        .flags = 0u
    };
    check(dc_xp_process_message(&sys, &s_mock_bot_state, &user_msg, 1000u, 42u, &evt),
          "valid member message awards XP");
    check(evt.new_xp >= DC_XP_GAIN_MIN && evt.new_xp <= DC_XP_GAIN_MAX, "XP added to user state");
    check(!evt.leveled_up, "not leveled up yet");

    /* Fast-forward and award enough XP to cross level 1 threshold (100 XP) */
    DcRankUser *rank = dc_state_rank_user(&s_mock_bot_state, 4242);
    rank->xp = 95u; /* 5 XP away from level 1 */

    check(dc_xp_process_message(&sys, &s_mock_bot_state, &user_msg, 8000u, 42u, &evt),
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
}

int main(void)
{
    printf("== XP, Anti-Farm Daily Cap & Role Rewards Tests ==\n");
    test_xp_curve();
    test_xp_cooldown_and_daily_cap();
    test_xp_roles();
    test_xp_orchestrator();

    if (failures == 0) {
        printf("\nPASSED (0 failure(s))\n");
        return 0;
    } else {
        printf("\nFAILED (%d failure(s))\n", failures);
        return 1;
    }
}
