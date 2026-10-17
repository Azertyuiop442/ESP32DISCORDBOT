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

bool dc_xp_system_bind_role(DcXpSystem *sys, uint8_t level, uint64_t role_id)
{
    if (sys == NULL) {
        return false;
    }
    return dc_xp_roles_bind(&sys->roles, level, role_id);
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

    DcXpSystem *sys = dc_xp_system_get();
    DcXpEvent event;
    uint64_t now_ms = (uint64_t)msg->message_id >> 22u;
    if (now_ms == 0u) {
        now_ms = 1000u;
    }
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
    if (strcmp(it->command, "rank") == 0) {
        DiscordId target_user = it->user_id;
        DcRankUser *rank = dc_state_rank_user(state, target_user);
        uint32_t xp = rank ? rank->xp : 0u;
        uint8_t lvl = rank ? rank->level : 0u;
        uint32_t next_req = dc_xp_required_for_level((uint8_t)(lvl + 1u));

        DcXpSystem *sys = dc_xp_system_get();
        uint64_t now_ms = (uint64_t)it->id >> 22u;
        uint32_t today_xp = dc_xp_cooldown_get_daily_xp(&sys->cooldowns, target_user, now_ms);

        char reply[160];
        (void)snprintf(reply, sizeof(reply),
                       "Level: %u | Total XP: %u / %u | Today: %u / %u XP",
                       (unsigned)lvl, (unsigned)xp, (unsigned)next_req,
                       (unsigned)today_xp, (unsigned)sys->cooldowns.daily_cap);

        (void)discord_interaction_reply(it, reply);
        return;
    }

    /* 2. Admin configuration slash command /setdailyxp <amount> */
    if (strcmp(it->command, "setdailyxp") == 0) {
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
}

static const DiscordFeature s_xp_feature = {
    .name = "xp",
    .required_intents = DC_INTENT_GUILD_MESSAGES,
    .init_priority = 10u,
    .on_message = dc_xp_on_message,
    .on_interaction = dc_xp_on_interaction
};

DcXpSystem *dc_xp_system_get(void)
{
    return &s_global_xp_system;
}

const DiscordFeature *dc_xp_feature_get(void)
{
    return &s_xp_feature;
}
