#ifndef DISCORD_XP_COOLDOWN_H
#define DISCORD_XP_COOLDOWN_H

#include <stdbool.h>
#include <stdint.h>
#include "discord_slice.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DC_XP_COOLDOWN_SLOTS       64u
#define DC_XP_DEFAULT_DEBOUNCE_MS  5000u   /* 5s debounce: conversation-friendly, blocks macro flood */
#define DC_XP_DEFAULT_DAILY_CAP    1000u   /* 1000 XP max per 24h rolling day */

typedef struct {
    DiscordId user_id;
    uint64_t  last_xp_ms;
    uint32_t  xp_today;
    uint32_t  day_key;
    bool      occupied;
} DcXpCooldownSlot;

typedef struct {
    DcXpCooldownSlot slots[DC_XP_COOLDOWN_SLOTS];
    uint32_t         cursor;
    uint64_t         debounce_ms;
    uint32_t         daily_cap;
} DcXpCooldownTable;

/**
 * Initializes the anti-spam table with debounce duration and 24h daily cap.
 */
void dc_xp_cooldown_init(DcXpCooldownTable *tbl, uint64_t debounce_ms, uint32_t daily_cap);

/**
 * Evaluates whether a user can gain XP for an incoming message.
 * Verifies debounce (natural conversation friendly, e.g. 5s) and enforces
 * the 24-hour daily cap to stop malicious power/permission farming.
 *
 * If eligible, records timestamp, adds gain to xp_today, and returns true.
 * If daily cap or debounce is exceeded, returns false.
 * out_awarded_xp (if non-null) receives the exact XP granted (clamped to daily cap).
 */
bool dc_xp_cooldown_try_acquire(DcXpCooldownTable *tbl, DiscordId user_id,
                                uint64_t now_ms, uint32_t requested_xp,
                                uint32_t *out_awarded_xp);

/**
 * Configures the 24-hour maximum XP cap dynamically.
 */
void dc_xp_cooldown_set_daily_cap(DcXpCooldownTable *tbl, uint32_t daily_cap);

/**
 * Configures the debounce interval (in milliseconds).
 */
void dc_xp_cooldown_set_debounce(DcXpCooldownTable *tbl, uint64_t debounce_ms);

/**
 * Returns user's accumulated XP for the current 24-hour day.
 */
uint32_t dc_xp_cooldown_get_daily_xp(const DcXpCooldownTable *tbl, DiscordId user_id, uint64_t now_ms);

/**
 * Returns remaining debounce milliseconds before the user can gain XP again (0 if expired).
 */
uint64_t dc_xp_cooldown_remaining_ms(const DcXpCooldownTable *tbl, DiscordId user_id, uint64_t now_ms);

/**
 * Resets/erases cooldown and daily XP history for a user (e.g. /forget).
 */
void dc_xp_cooldown_reset(DcXpCooldownTable *tbl, DiscordId user_id);


#ifdef __cplusplus
}
#endif

#endif /* DISCORD_XP_COOLDOWN_H */
