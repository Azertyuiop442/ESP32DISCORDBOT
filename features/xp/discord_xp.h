#ifndef DISCORD_XP_H
#define DISCORD_XP_H

#include <stdbool.h>
#include <stdint.h>

#include "discord_feature.h"
#include "discord_jobq.h"
#include "discord_state.h"
#include "discord_xp_curve.h"
#include "discord_xp_cooldown.h"
#include "discord_xp_roles.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    DcXpCooldownTable cooldowns;
    DcXpRolesTable    roles;
    bool              announce_levelup;
} DcXpSystem;

typedef struct {
    DiscordId user_id;
    DiscordId guild_id;
    DiscordId channel_id;
    uint32_t  old_xp;
    uint32_t  new_xp;
    uint32_t  gained_xp;
    uint8_t   old_level;
    uint8_t   new_level;
    bool      xp_gained;
    bool      leveled_up;
    uint8_t   roles_count;
    uint64_t  roles_awarded[DC_XP_MAX_ROLE_REWARDS];
} DcXpEvent;

/**
 * Initializes the XP system instance with debounce, 24h daily cap, and announce flag.
 */
void dc_xp_system_init(DcXpSystem *sys, uint64_t debounce_ms, uint32_t daily_cap, bool announce_levelup);

/**
 * Dynamically reconfigures the 24-hour daily XP cap.
 */
void dc_xp_system_set_daily_cap(DcXpSystem *sys, uint32_t daily_cap);

/**
 * Dynamically reconfigures the conversational debounce (in milliseconds).
 */
void dc_xp_system_set_debounce(DcXpSystem *sys, uint64_t debounce_ms);

/**
 * Configures a role reward for reaching a level.
 */
bool dc_xp_system_bind_role(DcXpSystem *sys, uint8_t level, uint64_t role_id);

/**
 * Core processing: inspects an incoming DiscordMessage, verifies sender & cooldown/daily cap,
 * increments user XP in bot state, evaluates level-up, and populates out_event.
 * Returns true if XP was awarded, false otherwise (e.g. bot sender, daily cap reached).
 */
bool dc_xp_process_message(DcXpSystem *sys, DcBotState *state,
                           const DiscordMessage *msg, uint64_t now_ms,
                           uint32_t seed, DcXpEvent *out_event);

/**
 * Builds a DC_JOB_ADD_ROLE outbound job from an awarded role index in DcXpEvent.
 * The payload is encoded as a decimal snowflake string per transport contract.
 */
bool dc_xp_build_role_job(const DcXpEvent *event, uint8_t role_index, DcJob *out_job);

/**
 * Builds a DC_JOB_SEND_MESSAGE congratulations message to channel_id.
 */
bool dc_xp_build_announce_job(const DcXpEvent *event, DcJob *out_job);

/**
 * Global XP system singleton and DiscordFeature registration provider.
 */
DcXpSystem *dc_xp_system_get(void);
const DiscordFeature *dc_xp_feature_get(void);

#ifdef __cplusplus
}
#endif

#endif /* DISCORD_XP_H */
