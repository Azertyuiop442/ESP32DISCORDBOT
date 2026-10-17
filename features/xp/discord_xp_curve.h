#ifndef DISCORD_XP_CURVE_H
#define DISCORD_XP_CURVE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DC_XP_MAX_LEVEL      100u
#define DC_XP_GAIN_MIN       15u
#define DC_XP_GAIN_MAX       25u

/**
 * Calculates total XP required to reach a specific level.
 * Formula: 50 * level * (level + 1)
 * Level 0 = 0 XP, Level 1 = 100 XP, Level 2 = 300 XP, Level 5 = 1500 XP.
 */
uint32_t dc_xp_required_for_level(uint8_t level);

/**
 * Calculates current level for a given amount of total XP.
 * Capped at DC_XP_MAX_LEVEL.
 */
uint8_t dc_xp_level_for_xp(uint32_t xp);

/**
 * Deterministic XP gain calculation per message based on seed.
 * Yields between DC_XP_GAIN_MIN and DC_XP_GAIN_MAX.
 */
uint32_t dc_xp_calc_gain(uint32_t seed);

/**
 * Evaluates whether adding gained_xp triggers a level up.
 * Returns true if new_level > current_level, writing new_level to out_new_level.
 */
bool dc_xp_check_level_up(uint32_t current_xp, uint32_t gained_xp,
                          uint8_t current_level, uint8_t *out_new_level);

#ifdef __cplusplus
}
#endif

#endif /* DISCORD_XP_CURVE_H */
