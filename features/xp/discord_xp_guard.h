#ifndef DISCORD_XP_GUARD_H
#define DISCORD_XP_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#include "discord_feature.h"
#include "discord_jobq.h"
#include "discord_slice.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DC_XP_GUARD_DEFAULT_MIN_LEVEL    3u
#define DC_XP_GUARD_DEFAULT_INVITE_LEVEL 5u
#define DC_XP_GUARD_MAX_MENTIONS         3u
#define DC_XP_GUARD_STRIKES_FOR_TIMEOUT  3u
#define DC_XP_GUARD_TIMEOUT_SECONDS      300u    /* 5 minutes */
#define DC_XP_GUARD_STRIKE_WINDOW_MS     60000ULL/* 60s window */
#define DC_XP_GUARD_DM_COOLDOWN_MS       30000ULL/* 30s DM debounce */
#define DC_XP_GUARD_WARNED_MAX           32u

typedef struct {
    DiscordId user_id;
    uint64_t  last_strike_ms;
    uint64_t  last_dm_ms;
    uint8_t   strikes;
} DcXpGuardWarnEntry;

typedef struct {
    uint8_t            min_level;
    uint8_t            min_invite_level;
    uint64_t           dm_cooldown_ms;
    DcXpGuardWarnEntry warned[DC_XP_GUARD_WARNED_MAX];
    uint8_t            warned_cursor;
} DcXpGuard;

void dc_xp_guard_init(DcXpGuard *guard, uint8_t min_level, uint8_t min_invite_lvl, uint64_t dm_cooldown_ms);
void dc_xp_guard_set_min_level(DcXpGuard *guard, uint8_t min_level);
void dc_xp_guard_set_invite_level(DcXpGuard *guard, uint8_t min_invite_lvl);

bool dc_xp_guard_has_links(DiscordSlice content);
bool dc_xp_guard_has_invites(DiscordSlice content);
uint8_t dc_xp_guard_count_mentions(DiscordSlice content);

bool dc_xp_guard_is_restricted(const DcXpGuard *guard, const DiscordMessage *msg, uint8_t user_level);
uint8_t dc_xp_guard_record_strike(DcXpGuard *guard, DiscordId user_id, uint64_t now_ms);
void dc_xp_guard_reset_strikes(DcXpGuard *guard, DiscordId user_id);
bool dc_xp_guard_should_warn_dm(DcXpGuard *guard, DiscordId user_id, uint64_t now_ms);

bool dc_xp_guard_build_delete_job(const DiscordMessage *msg, DcJob *out_job);
bool dc_xp_guard_build_timeout_job(const DiscordMessage *msg, uint32_t seconds, DcJob *out_job);
bool dc_xp_guard_build_warn_dm_job(DiscordId user_id, uint8_t min_lvl, uint8_t cur_lvl, DcJob *out_job);
bool dc_xp_guard_build_timeout_dm_job(DiscordId user_id, uint32_t minutes, DcJob *out_job);

#ifdef __cplusplus
}
#endif

#endif /* DISCORD_XP_GUARD_H */
