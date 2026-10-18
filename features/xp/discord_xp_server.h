#ifndef DISCORD_XP_SERVER_H
#define DISCORD_XP_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DC_DISCORD_SLOWMODE_MAX 21600u /* 6 hours maximum per Discord API */
#define DC_DISCORD_PERM_EMBED_LINKS  (1ULL << 14) /* 0x4000 */
#define DC_DISCORD_PERM_ATTACH_FILES (1ULL << 15) /* 0x8000 */

/**
 * Validates and clamps slowmode seconds within Discord API limits (0..21600).
 * Returns true if valid, false if negative or malformed.
 */
bool dc_xp_server_validate_slowmode(long seconds, uint32_t *out_clamped);

/**
 * Builds the JSON body for channel slowmode: {"rate_limit_per_user":<seconds>}
 */
bool dc_xp_server_build_slowmode_body(uint32_t seconds, char *out, size_t cap);

/**
 * Builds the native AutoMod rule JSON payload for mention raid protection.
 * Configured at Discord edge with mention limit and automatic timeout.
 */
bool dc_xp_server_build_automod_mention_rule(uint8_t mention_limit, uint32_t timeout_sec,
                                             char *out, size_t cap);

/**
 * Builds the channel permission overwrite JSON body for blocking media/embeds:
 * Denies ATTACH_FILES (0x8000) and EMBED_LINKS (0x4000).
 */
bool dc_xp_server_build_guard_overwrite_body(uint64_t deny_bits, uint64_t allow_bits,
                                             char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* DISCORD_XP_SERVER_H */
