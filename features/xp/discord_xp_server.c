#include "discord_xp_server.h"

#include <stdio.h>
#include <string.h>

bool dc_xp_server_validate_slowmode(long seconds, uint32_t *out_clamped)
{
    if (seconds < 0 || out_clamped == NULL) {
        return false;
    }
    if ((unsigned long)seconds > DC_DISCORD_SLOWMODE_MAX) {
        *out_clamped = DC_DISCORD_SLOWMODE_MAX;
    } else {
        *out_clamped = (uint32_t)seconds;
    }
    return true;
}

bool dc_xp_server_build_slowmode_body(uint32_t seconds, char *out, size_t cap)
{
    if (out == NULL || cap < 32u) {
        return false;
    }
    if (seconds > DC_DISCORD_SLOWMODE_MAX) {
        seconds = DC_DISCORD_SLOWMODE_MAX;
    }
    int n = snprintf(out, cap, "{\"rate_limit_per_user\":%u}", (unsigned)seconds);
    return (n > 0 && (size_t)n < cap);
}

bool dc_xp_server_build_automod_mention_rule(uint8_t mention_limit, uint32_t timeout_sec,
                                             char *out, size_t cap)
{
    if (out == NULL || cap < 256u || mention_limit == 0u) {
        return false;
    }
    if (timeout_sec > 2419200u) {
        timeout_sec = 2419200u;
    }
    int n = snprintf(out, cap,
        "{\"name\":\"Anti-Spam Mention Raid\",\"event_type\":1,\"trigger_type\":5,"
        "\"trigger_metadata\":{\"mention_total_limit\":%u,\"mention_raid_protection_enabled\":true},"
        "\"actions\":[{\"type\":1,\"metadata\":{\"custom_message\":\"Mention raid blocked by bot.\"}},"
        "{\"type\":3,\"metadata\":{\"duration_seconds\":%u}}],\"enabled\":true}",
        (unsigned)mention_limit, (unsigned)timeout_sec);
    return (n > 0 && (size_t)n < cap);
}

bool dc_xp_server_build_guard_overwrite_body(uint64_t deny_bits, uint64_t allow_bits,
                                             char *out, size_t cap)
{
    if (out == NULL || cap < 64u) {
        return false;
    }
    int n = snprintf(out, cap, "{\"deny\":\"%llu\",\"allow\":\"%llu\",\"type\":0}",
                     (unsigned long long)deny_bits, (unsigned long long)allow_bits);
    return (n > 0 && (size_t)n < cap);
}
