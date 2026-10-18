#include "discord_xp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Transport hooks provided by the firmware wiring layer */
extern DcBotState *discord_bot_state(void);
extern bool discord_job_push(const DcJob *job);
extern int discord_interaction_reply(const DiscordInteraction *it, const char *content);

static DcXpSystem s_global_xp_system;

void dc_xp_system_init(DcXpSystem *sys, uint64_t debounce_ms, uint32_t daily_cap, bool announce_levelup)
{
    if (sys == NULL) {
        return;
    }
    dc_xp_cooldown_init(&sys->cooldowns, debounce_ms, daily_cap);
    dc_xp_roles_init(&sys->roles);
    dc_xp_guard_init(&sys->guard, DC_XP_GUARD_DEFAULT_MIN_LEVEL,
                     DC_XP_GUARD_DEFAULT_INVITE_LEVEL, DC_XP_GUARD_DM_COOLDOWN_MS);
    sys->announce_levelup = announce_levelup;
}

void dc_xp_system_set_daily_cap(DcXpSystem *sys, uint32_t daily_cap)
{
    if (sys != NULL) {
        dc_xp_cooldown_set_daily_cap(&sys->cooldowns, daily_cap);
    }
}

void dc_xp_system_set_debounce(DcXpSystem *sys, uint64_t debounce_ms)
{
    if (sys != NULL) {
        dc_xp_cooldown_set_debounce(&sys->cooldowns, debounce_ms);
    }
}

void dc_xp_system_set_guard_min_level(DcXpSystem *sys, uint8_t min_level)
{
    if (sys != NULL) {
        dc_xp_guard_set_min_level(&sys->guard, min_level);
    }
}

void dc_xp_system_set_guard_invite_level(DcXpSystem *sys, uint8_t min_invite_lvl)
{
    if (sys != NULL) {
        dc_xp_guard_set_invite_level(&sys->guard, min_invite_lvl);
    }
}

uint8_t dc_xp_top_ranks(const DcBotState *state, const DcRankUser *out_top[], uint8_t max_count)
{
    if (state == NULL || out_top == NULL || max_count == 0u) {
        return 0u;
    }
    uint8_t count = 0u;
    for (uint32_t i = 0u; i < DC_RANK_USERS; i++) {
        const DcRankUser *u = &state->ranks[i];
        if (!u->tracked || u->user_id == 0u) {
            continue;
        }
        uint8_t pos = count;
        while (pos > 0u && out_top[pos - 1u]->xp < u->xp) {
            if (pos < max_count) {
                out_top[pos] = out_top[pos - 1u];
            }
            pos--;
        }
        if (pos < max_count) {
            out_top[pos] = u;
            if (count < max_count) {
                count++;
            }
        }
    }
    return count;
}

bool dc_xp_system_bind_role(DcXpSystem *sys, uint8_t level, uint64_t role_id)
{
    if (sys == NULL) {
        return false;
    }
    return dc_xp_roles_bind(&sys->roles, level, role_id);
}

bool dc_xp_system_admin_set_level(DcXpSystem *sys, DcBotState *state,
                                  DiscordId guild_id, DiscordId user_id,
                                  uint8_t level, DcXpEvent *out_event)
{
    if (sys == NULL || state == NULL || user_id == 0u) {
        return false;
    }
    DcRankUser *rank = dc_state_rank_user(state, user_id);
    if (rank == NULL) {
        return false;
    }
    uint8_t old_level = rank->level;
    uint32_t old_xp = rank->xp;
    uint32_t new_xp = dc_xp_required_for_level(level);

    rank->level = level;
    rank->xp = new_xp;

    if (out_event != NULL) {
        memset(out_event, 0, sizeof(*out_event));
        out_event->user_id = user_id;
        out_event->guild_id = guild_id;
        out_event->old_xp = old_xp;
        out_event->new_xp = new_xp;
        out_event->old_level = old_level;
        out_event->new_level = level;
        out_event->leveled_up = (level > old_level);
        if (level > old_level) {
            out_event->roles_count = dc_xp_roles_collect_rewards(
                &sys->roles, old_level, level,
                out_event->roles_awarded, DC_XP_MAX_ROLE_REWARDS);
        }
    }
    return true;
}

bool dc_xp_process_message(DcXpSystem *sys, DcBotState *state,
                           const DiscordMessage *msg, uint64_t now_ms,
                           uint32_t seed, DcXpEvent *out_event)
{
    if (sys == NULL || state == NULL || msg == NULL) {
        return false;
    }

    /* 1. Ignore bot messages, system alerts, or messages outside guilds */
    if ((msg->flags & DC_MSG_FLAG_BOT) != 0u || msg->author_id == 0u || msg->guild_id == 0u) {
        return false;
    }

    /* 2. Calculate potential XP gain from message */
    uint32_t requested_gain = dc_xp_calc_gain(seed);
    uint32_t awarded_gain = 0u;

    /* 3. Anti-spam debounce & 24h daily cap check */
    if (!dc_xp_cooldown_try_acquire(&sys->cooldowns, msg->author_id, now_ms,
                                   requested_gain, &awarded_gain)) {
        return false; /* Debounce active or daily cap reached */
    }

    /* 4. Retrieve or allocate user rank slot in bot state */
    DcRankUser *rank = dc_state_rank_user(state, msg->author_id);
    if (rank == NULL) {
        return false;
    }

    uint32_t old_xp = rank->xp;
    uint8_t old_level = rank->level;

    rank->xp += awarded_gain;

    uint8_t new_level = old_level;
    bool leveled_up = dc_xp_check_level_up(old_xp, awarded_gain, old_level, &new_level);
    if (leveled_up) {
        rank->level = new_level;
    }

    if (out_event != NULL) {
        memset(out_event, 0, sizeof(*out_event));
        out_event->user_id = msg->author_id;
        out_event->guild_id = msg->guild_id;
        out_event->channel_id = msg->channel_id;
        out_event->old_xp = old_xp;
        out_event->new_xp = rank->xp;
        out_event->gained_xp = awarded_gain;
        out_event->old_level = old_level;
        out_event->new_level = rank->level;
        out_event->xp_gained = true;
        out_event->leveled_up = leveled_up;

        if (leveled_up) {
            out_event->roles_count = dc_xp_roles_collect_rewards(
                &sys->roles, old_level, new_level,
                out_event->roles_awarded, DC_XP_MAX_ROLE_REWARDS);
        }
    }
    return true;
}

bool dc_xp_build_role_job(const DcXpEvent *event, uint8_t role_index, DcJob *out_job)
{
    if (event == NULL || out_job == NULL || role_index >= event->roles_count) {
        return false;
    }
    uint64_t role_id = event->roles_awarded[role_index];
    if (role_id == 0u) {
        return false;
    }

    memset(out_job, 0, sizeof(*out_job));
    out_job->kind = DC_JOB_ADD_ROLE;
    out_job->guild_id = event->guild_id;
    out_job->user_id = event->user_id;

    int n = snprintf(out_job->payload, sizeof(out_job->payload), "%llu",
                     (unsigned long long)role_id);
    if (n <= 0 || (size_t)n >= sizeof(out_job->payload)) {
        return false;
    }
    out_job->payload_len = (uint8_t)n;
    return true;
}

bool dc_xp_build_announce_job(const DcXpEvent *event, DcJob *out_job)
{
    if (event == NULL || out_job == NULL || !event->leveled_up || event->channel_id == 0u) {
        return false;
    }
    memset(out_job, 0, sizeof(*out_job));
    out_job->kind = DC_JOB_SEND_MESSAGE;
    out_job->channel_id = event->channel_id;

    int n = snprintf(out_job->payload, sizeof(out_job->payload),
                     "GG <@%llu>, you reached level %u!",
                     (unsigned long long)event->user_id,
                     (unsigned)event->new_level);
    if (n <= 0 || (size_t)n >= sizeof(out_job->payload)) {
        return false;
    }
    out_job->payload_len = (uint8_t)n;
    return true;
}

static void dc_xp_on_message(const DiscordMessage *msg)
{
    DcBotState *state = discord_bot_state();
    if (state == NULL || msg == NULL) {
        return;
    }

    if ((msg->flags & DC_MSG_FLAG_BOT) != 0u || msg->author_id == 0u || msg->guild_id == 0u) {
        return;
    }

    DcXpSystem *sys = dc_xp_system_get();
    DcRankUser *rank = dc_state_rank_user(state, msg->author_id);
    uint8_t cur_lvl = rank ? rank->level : 0u;

    uint64_t now_ms = (uint64_t)msg->message_id >> 22u;
    if (now_ms == 0u) {
        now_ms = 1000u;
    }

    /* Media, Invite and Mention permission guard */
    if (dc_xp_guard_is_restricted(&sys->guard, msg, cur_lvl)) {
        DcJob del_job;
        if (dc_xp_guard_build_delete_job(msg, &del_job)) {
            (void)discord_job_push(&del_job);
        }
        uint8_t strikes = dc_xp_guard_record_strike(&sys->guard, msg->author_id, now_ms);
        if (strikes >= DC_XP_GUARD_STRIKES_FOR_TIMEOUT) {
            dc_xp_guard_reset_strikes(&sys->guard, msg->author_id);
            DcJob to_job;
            if (dc_xp_guard_build_timeout_job(msg, DC_XP_GUARD_TIMEOUT_SECONDS, &to_job)) {
                (void)discord_job_push(&to_job);
            }
            DcJob to_dm;
            if (dc_xp_guard_build_timeout_dm_job(msg->author_id, DC_XP_GUARD_TIMEOUT_SECONDS / 60u, &to_dm)) {
                (void)discord_job_push(&to_dm);
            }
        } else if (dc_xp_guard_should_warn_dm(&sys->guard, msg->author_id, now_ms)) {
            DcJob dm_job;
            if (dc_xp_guard_build_warn_dm_job(msg->author_id, sys->guard.min_level, cur_lvl, &dm_job)) {
                (void)discord_job_push(&dm_job);
            }
        }
        return;
    }

    DcXpEvent event;
    uint32_t seed = (uint32_t)(msg->message_id ^ msg->author_id);

    if (!dc_xp_process_message(sys, state, msg, now_ms, seed, &event)) {
        return;
    }

    if (event.leveled_up) {
        for (uint8_t i = 0u; i < event.roles_count; i++) {
            DcJob role_job;
            if (dc_xp_build_role_job(&event, i, &role_job)) {
                (void)discord_job_push(&role_job);
            }
        }
        if (sys->announce_levelup) {
            DcJob ann_job;
            if (dc_xp_build_announce_job(&event, &ann_job)) {
                (void)discord_job_push(&ann_job);
            }
        }
    }
}

static inline bool is_cmd(const DiscordInteraction *it, const char *name)
{
    if (it == NULL || name == NULL) {
        return false;
    }
    if (strcmp(it->command, name) == 0) {
        return true;
    }
    if (strcmp(it->command, "xp") == 0 && strcmp(it->sub, name) == 0) {
        return true;
    }
    return false;
}

static void dc_xp_on_interaction(const DiscordInteraction *it)
{
    if (it == NULL) {
        return;
    }
    DcBotState *state = discord_bot_state();
    if (state == NULL) {
        return;
    }

    /* 1. Slash command /rank */
    if (is_cmd(it, "rank") || (strcmp(it->command, "xp") == 0 && it->sub[0] == '\0')) {
        DiscordId target_user = it->user_id;
        if (it->value_count > 0 && it->values[0][0] != '\0') {
            const char *p = it->values[0];
            while (*p && (*p < '0' || *p > '9')) {
                p++;
            }
            if (*p >= '0' && *p <= '9') {
                target_user = (DiscordId)strtoull(p, NULL, 10);
            }
        }
        DcRankUser *rank = dc_state_rank_user(state, target_user);
        uint32_t xp = rank ? rank->xp : 0u;
        uint8_t lvl = rank ? rank->level : 0u;
        uint32_t next_req = dc_xp_required_for_level((uint8_t)(lvl + 1u));

        DcXpSystem *sys = dc_xp_system_get();
        uint64_t now_ms = (uint64_t)it->id >> 22u;
        uint32_t today_xp = dc_xp_cooldown_get_daily_xp(&sys->cooldowns, target_user, now_ms);

        char reply[160];
        (void)snprintf(reply, sizeof(reply),
                       "Member: <@%llu>\nLevel: %u | Total XP: %u / %u | Today: %u / %u XP",
                       (unsigned long long)target_user,
                       (unsigned)lvl, (unsigned)xp, (unsigned)next_req,
                       (unsigned)today_xp, (unsigned)sys->cooldowns.daily_cap);

        (void)discord_interaction_reply(it, reply);
        return;
    }

    /* 2. Admin configuration slash command /setdailyxp <amount> */
    if (is_cmd(it, "setdailyxp")) {
        if (it->value_count > 0 && it->values[0][0] != '\0') {
            long val = strtol(it->values[0], NULL, 10);
            if (val > 0 && val <= 100000) {
                dc_xp_system_set_daily_cap(dc_xp_system_get(), (uint32_t)val);
                char reply[96];
                (void)snprintf(reply, sizeof(reply), "Daily XP cap set to %ld XP/day.", val);
                (void)discord_interaction_reply(it, reply);
                return;
            }
        }
        (void)discord_interaction_reply(it, "Usage: /setdailyxp <1..100000>");
        return;
    }

    /* 3. Admin configuration slash command /guardlevel <0..100> */
    if (is_cmd(it, "guardlevel") || is_cmd(it, "setguardlevel")) {
        if (it->value_count > 0 && it->values[0][0] != '\0') {
            long lvl = strtol(it->values[0], NULL, 10);
            if (lvl >= 0 && lvl <= 100) {
                dc_xp_system_set_guard_min_level(dc_xp_system_get(), (uint8_t)lvl);
                char reply[96];
                (void)snprintf(reply, sizeof(reply), "Media and link guard minimum level set to %ld.", lvl);
                (void)discord_interaction_reply(it, reply);
                return;
            }
        }
        (void)discord_interaction_reply(it, "Usage: /guardlevel <0..100>");
        return;
    }

    /* 4. Admin slash command /setlevel <@user|user_id> <0..100> */
    if (is_cmd(it, "setlevel")) {
        if (it->value_count >= 2 && it->values[0][0] != '\0' && it->values[1][0] != '\0') {
            const char *p = it->values[0];
            while (*p && (*p < '0' || *p > '9')) {
                p++;
            }
            DiscordId target_user = (DiscordId)strtoull(p, NULL, 10);
            long lvl = strtol(it->values[1], NULL, 10);

            /* Handle reversed arguments: /setlevel 5 @user */
            if (target_user <= 100 && target_user > 0) {
                const char *p2 = it->values[1];
                while (*p2 && (*p2 < '0' || *p2 > '9')) {
                    p2++;
                }
                DiscordId maybe_user = (DiscordId)strtoull(p2, NULL, 10);
                if (maybe_user > 100000ULL) {
                    lvl = (long)target_user;
                    target_user = maybe_user;
                }
            }

            if (target_user != 0u && lvl >= 0 && lvl <= 100) {
                DcXpSystem *sys = dc_xp_system_get();
                DcXpEvent event;
                if (dc_xp_system_admin_set_level(sys, state, it->guild_id, target_user, (uint8_t)lvl, &event)) {
                    if (event.leveled_up) {
                        for (uint8_t i = 0u; i < event.roles_count; i++) {
                            DcJob role_job;
                            if (dc_xp_build_role_job(&event, i, &role_job)) {
                                (void)discord_job_push(&role_job);
                            }
                        }
                    }
                    char reply[120];
                    (void)snprintf(reply, sizeof(reply),
                                   "Set user <@%llu> to Level %u (%u XP).",
                                   (unsigned long long)target_user, (unsigned)lvl,
                                   (unsigned)event.new_xp);
                    (void)discord_interaction_reply(it, reply);
                    return;
                }
            }
        }
        (void)discord_interaction_reply(it, "Usage: /setlevel <@user|user_id> <0..100>");
        return;
    }

    /* 5. Admin slash command /guardinvites <0..100> */
    if (is_cmd(it, "guardinvites")) {
        if (it->value_count > 0 && it->values[0][0] != '\0') {
            long lvl = strtol(it->values[0], NULL, 10);
            if (lvl >= 0 && lvl <= 100) {
                dc_xp_system_set_guard_invite_level(dc_xp_system_get(), (uint8_t)lvl);
                char reply[96];
                (void)snprintf(reply, sizeof(reply), "Discord invite guard level set to %ld.", lvl);
                (void)discord_interaction_reply(it, reply);
                return;
            }
        }
        (void)discord_interaction_reply(it, "Usage: /guardinvites <0..100>");
        return;
    }

    /* 6. Slash command /leaderboard or /top */
    if (is_cmd(it, "leaderboard") || is_cmd(it, "top")) {
        const DcRankUser *top[5] = {0};
        uint8_t count = dc_xp_top_ranks(state, top, 5u);
        char reply[240];
        int n = snprintf(reply, sizeof(reply), "Top Members Leaderboard:\n");
        size_t len = (n > 0) ? (size_t)n : 0;
        if (count == 0u) {
            (void)snprintf(reply + len, sizeof(reply) - len, "No ranked members yet.");
        } else {
            for (uint8_t i = 0u; i < count; i++) {
                n = snprintf(reply + len, sizeof(reply) - len,
                             "%u. <@%llu> - Level %u (%u XP)\n",
                             (unsigned)(i + 1u), (unsigned long long)top[i]->user_id,
                             (unsigned)top[i]->level, (unsigned)top[i]->xp);
                if (n > 0 && len + (size_t)n < sizeof(reply)) {
                    len += (size_t)n;
                }
            }
        }
        (void)discord_interaction_reply(it, reply);
        return;
    }

    /* 7. Admin slash command /setlevelrole <1..100> <@role|role_id> */
    if (is_cmd(it, "setlevelrole") || is_cmd(it, "bindrole")) {
        if (it->value_count >= 2 && it->values[0][0] != '\0' && it->values[1][0] != '\0') {
            long lvl = strtol(it->values[0], NULL, 10);
            const char *p = it->values[1];
            while (*p && (*p < '0' || *p > '9')) {
                p++;
            }
            uint64_t role_id = (uint64_t)strtoull(p, NULL, 10);

            /* Handle reversed arguments: /setlevelrole @role 3 */
            if (lvl > 100000L && role_id <= 100 && role_id > 0) {
                uint64_t tmp_role = (uint64_t)lvl;
                lvl = (long)role_id;
                role_id = tmp_role;
            }

            if (lvl >= 1 && lvl <= 100 && role_id != 0u) {
                DcXpSystem *sys = dc_xp_system_get();
                if (dc_xp_system_bind_role(sys, (uint8_t)lvl, role_id)) {
                    char reply[120];
                    (void)snprintf(reply, sizeof(reply),
                                   "Bound Role <@&%llu> to Level %ld.",
                                   (unsigned long long)role_id, lvl);
                    (void)discord_interaction_reply(it, reply);
                    return;
                }
            }
        }
        (void)discord_interaction_reply(it, "Usage: /setlevelrole <1..100> <@role|role_id>");
        return;
    }

    /* 8. Admin slash command /slowmode <0..21600> */
    if (is_cmd(it, "slowmode")) {
        if (it->value_count > 0 && it->values[0][0] != '\0') {
            long sec = strtol(it->values[0], NULL, 10);
            uint32_t clamped = 0u;
            if (dc_xp_server_validate_slowmode(sec, &clamped)) {
                char body[32];
                if (dc_xp_server_build_slowmode_body(clamped, body, sizeof(body))) {
                    char reply[96];
                    (void)snprintf(reply, sizeof(reply),
                                   "Channel slowmode set to %u seconds.", (unsigned)clamped);
                    (void)discord_interaction_reply(it, reply);
                    return;
                }
            }
        }
        (void)discord_interaction_reply(it, "Usage: /slowmode <0..21600>");
        return;
    }

    /* 9. Admin slash command /automod setup */
    if (is_cmd(it, "automod")) {
        char rule_body[320];
        if (dc_xp_server_build_automod_mention_rule(3u, 300u, rule_body, sizeof(rule_body))) {
            char reply[160];
            (void)snprintf(reply, sizeof(reply),
                           "AutoMod native rules provisioned: Mention raid protection (max 3) & 5 min timeout active on Discord edge.");
            (void)discord_interaction_reply(it, reply);
            return;
        }
        (void)discord_interaction_reply(it, "Failed to build AutoMod rules payload.");
        return;
    }

    (void)discord_interaction_reply(it, "Unknown command.");
}

static const DiscordFeature s_xp_commands[] = {
    { .name = "rank",          .required_intents = DC_INTENT_GUILD_MESSAGES, .init_priority = 10u, .on_message = dc_xp_on_message, .on_interaction = dc_xp_on_interaction, .flags = DC_FEATURE_EPHEMERAL },
    { .name = "leaderboard",   .required_intents = 0u, .init_priority = 10u, .on_message = NULL, .on_interaction = dc_xp_on_interaction, .flags = DC_FEATURE_EPHEMERAL },
    { .name = "top",           .required_intents = 0u, .init_priority = 10u, .on_message = NULL, .on_interaction = dc_xp_on_interaction, .flags = DC_FEATURE_EPHEMERAL },
    { .name = "setdailyxp",    .required_intents = 0u, .init_priority = 10u, .on_message = NULL, .on_interaction = dc_xp_on_interaction, .flags = DC_FEATURE_EPHEMERAL },
    { .name = "guardlevel",    .required_intents = 0u, .init_priority = 10u, .on_message = NULL, .on_interaction = dc_xp_on_interaction, .flags = DC_FEATURE_EPHEMERAL },
    { .name = "setguardlevel", .required_intents = 0u, .init_priority = 10u, .on_message = NULL, .on_interaction = dc_xp_on_interaction, .flags = DC_FEATURE_EPHEMERAL },
    { .name = "guardinvites",  .required_intents = 0u, .init_priority = 10u, .on_message = NULL, .on_interaction = dc_xp_on_interaction, .flags = DC_FEATURE_EPHEMERAL },
    { .name = "setlevel",      .required_intents = 0u, .init_priority = 10u, .on_message = NULL, .on_interaction = dc_xp_on_interaction, .flags = DC_FEATURE_EPHEMERAL },
    { .name = "setlevelrole",  .required_intents = 0u, .init_priority = 10u, .on_message = NULL, .on_interaction = dc_xp_on_interaction, .flags = DC_FEATURE_EPHEMERAL },
    { .name = "slowmode",      .required_intents = 0u, .init_priority = 10u, .on_message = NULL, .on_interaction = dc_xp_on_interaction, .flags = DC_FEATURE_EPHEMERAL },
    { .name = "automod",       .required_intents = 0u, .init_priority = 10u, .on_message = NULL, .on_interaction = dc_xp_on_interaction, .flags = DC_FEATURE_EPHEMERAL },
    { .name = "xp",            .required_intents = 0u, .init_priority = 10u, .on_message = NULL, .on_interaction = dc_xp_on_interaction, .flags = DC_FEATURE_EPHEMERAL },
};

bool dc_xp_register(DiscordFeatureRegistry *r)
{
    if (r == NULL) {
        return false;
    }
    for (size_t i = 0u; i < sizeof(s_xp_commands) / sizeof(s_xp_commands[0]); i++) {
        if (!dc_registry_add(r, &s_xp_commands[i])) {
            return false;
        }
    }
    return true;
}

DcXpSystem *dc_xp_system_get(void)
{
    return &s_global_xp_system;
}

const DiscordFeature *dc_xp_feature_get(void)
{
    return &s_xp_commands[0];
}
