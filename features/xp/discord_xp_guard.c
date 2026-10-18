#include "discord_xp_guard.h"

#include <stdio.h>
#include <string.h>

void dc_xp_guard_init(DcXpGuard *guard, uint8_t min_level, uint8_t min_invite_lvl, uint64_t dm_cooldown_ms)
{
    if (guard == NULL) return;
    guard->min_level = min_level;
    guard->min_invite_level = min_invite_lvl;
    guard->dm_cooldown_ms = dm_cooldown_ms;
    guard->warned_cursor = 0u;
    memset(guard->warned, 0, sizeof(guard->warned));
}

void dc_xp_guard_set_min_level(DcXpGuard *guard, uint8_t min_level)
{
    if (guard != NULL) guard->min_level = min_level;
}

void dc_xp_guard_set_invite_level(DcXpGuard *guard, uint8_t min_invite_lvl)
{
    if (guard != NULL) guard->min_invite_level = min_invite_lvl;
}

static inline bool mem_case_eq(const uint8_t *a, const char *b, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char ca = (char)a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return false;
    }
    return true;
}

bool dc_xp_guard_has_links(DiscordSlice content)
{
    if (content.ptr == NULL || content.len < 7u) return false;
    for (uint16_t i = 0u; i + 7u <= content.len; i++) {
        if ((content.ptr[i] == 'h' || content.ptr[i] == 'H')) {
            if (mem_case_eq(&content.ptr[i], "http://", 7u)) return true;
            if (i + 8u <= content.len && mem_case_eq(&content.ptr[i], "https://", 8u)) return true;
        }
    }
    return false;
}

bool dc_xp_guard_has_invites(DiscordSlice content)
{
    if (content.ptr == NULL || content.len < 11u) return false;
    for (uint16_t i = 0u; i + 11u <= content.len; i++) {
        if ((content.ptr[i] == 'd' || content.ptr[i] == 'D') &&
            mem_case_eq(&content.ptr[i], "discord.gg/", 11u)) {
            return true;
        }
    }
    return false;
}

uint8_t dc_xp_guard_count_mentions(DiscordSlice content)
{
    if (content.ptr == NULL || content.len < 2u) return 0u;
    uint8_t count = 0u;
    for (uint16_t i = 0u; i + 1u < content.len; i++) {
        if (content.ptr[i] == '<' && content.ptr[i + 1] == '@') {
            if (++count >= DC_XP_GUARD_MAX_MENTIONS + 1u) return count;
        }
        if (i + 9u <= content.len && mem_case_eq(&content.ptr[i], "@everyone", 9u)) return 99u;
        if (i + 5u <= content.len && mem_case_eq(&content.ptr[i], "@here", 5u)) return 99u;
    }
    return count;
}

bool dc_xp_guard_is_restricted(const DcXpGuard *guard, const DiscordMessage *msg, uint8_t user_level)
{
    if (guard == NULL || msg == NULL) return false;

    if (guard->min_level > 0u && user_level < guard->min_level) {
        if ((msg->flags & DC_MSG_FLAG_IMAGE) != 0u || dc_xp_guard_has_links(msg->content)) return true;
        if (dc_xp_guard_count_mentions(msg->content) > DC_XP_GUARD_MAX_MENTIONS) return true;
    }
    if (guard->min_invite_level > 0u && user_level < guard->min_invite_level) {
        if (dc_xp_guard_has_invites(msg->content)) return true;
    }
    return false;
}

uint8_t dc_xp_guard_record_strike(DcXpGuard *guard, DiscordId user_id, uint64_t now_ms)
{
    if (guard == NULL || user_id == 0u) return 0u;
    for (uint8_t i = 0u; i < DC_XP_GUARD_WARNED_MAX; i++) {
        if (guard->warned[i].user_id == user_id) {
            if (guard->warned[i].last_strike_ms != 0u &&
                now_ms < guard->warned[i].last_strike_ms + DC_XP_GUARD_STRIKE_WINDOW_MS) {
                guard->warned[i].last_strike_ms = now_ms;
                return ++guard->warned[i].strikes;
            }
            guard->warned[i].strikes = 1u;
            guard->warned[i].last_strike_ms = now_ms;
            return 1u;
        }
    }
    uint8_t slot = guard->warned_cursor;
    guard->warned_cursor = (uint8_t)((guard->warned_cursor + 1u) % DC_XP_GUARD_WARNED_MAX);
    guard->warned[slot].user_id = user_id;
    guard->warned[slot].last_strike_ms = now_ms;
    guard->warned[slot].last_dm_ms = 0u;
    guard->warned[slot].strikes = 1u;
    return 1u;
}

void dc_xp_guard_reset_strikes(DcXpGuard *guard, DiscordId user_id)
{
    if (guard == NULL || user_id == 0u) return;
    for (uint8_t i = 0u; i < DC_XP_GUARD_WARNED_MAX; i++) {
        if (guard->warned[i].user_id == user_id) {
            guard->warned[i].strikes = 0u;
            return;
        }
    }
}

bool dc_xp_guard_should_warn_dm(DcXpGuard *guard, DiscordId user_id, uint64_t now_ms)
{
    if (guard == NULL || user_id == 0u) return false;
    for (uint8_t i = 0u; i < DC_XP_GUARD_WARNED_MAX; i++) {
        if (guard->warned[i].user_id == user_id) {
            if (guard->warned[i].last_dm_ms != 0u &&
                now_ms < guard->warned[i].last_dm_ms + guard->dm_cooldown_ms) {
                return false;
            }
            guard->warned[i].last_dm_ms = now_ms;
            return true;
        }
    }
    uint8_t slot = guard->warned_cursor;
    guard->warned_cursor = (uint8_t)((guard->warned_cursor + 1u) % DC_XP_GUARD_WARNED_MAX);
    guard->warned[slot].user_id = user_id;
    guard->warned[slot].last_dm_ms = now_ms;
    guard->warned[slot].last_strike_ms = 0u;
    guard->warned[slot].strikes = 0u;
    return true;
}

bool dc_xp_guard_build_delete_job(const DiscordMessage *msg, DcJob *out_job)
{
    if (msg == NULL || out_job == NULL) return false;
    memset(out_job, 0, sizeof(*out_job));
    out_job->kind = DC_JOB_DELETE_MESSAGE;
    out_job->guild_id = msg->guild_id;
    out_job->channel_id = msg->channel_id;
    out_job->user_id = msg->author_id;
    int n = snprintf(out_job->payload, sizeof(out_job->payload), "%llu", (unsigned long long)msg->message_id);
    if (n <= 0 || (size_t)n >= sizeof(out_job->payload)) return false;
    out_job->payload_len = (uint8_t)n;
    return true;
}

bool dc_xp_guard_build_timeout_job(const DiscordMessage *msg, uint32_t seconds, DcJob *out_job)
{
    if (msg == NULL || out_job == NULL || seconds == 0u) return false;
    memset(out_job, 0, sizeof(*out_job));
    out_job->kind = DC_JOB_TIMEOUT_USER;
    out_job->guild_id = msg->guild_id;
    out_job->channel_id = msg->channel_id;
    out_job->user_id = msg->author_id;
    out_job->aux_id = seconds;
    return true;
}

bool dc_xp_guard_build_warn_dm_job(DiscordId user_id, uint8_t min_lvl, uint8_t cur_lvl, DcJob *out_job)
{
    if (user_id == 0u || out_job == NULL) return false;
    memset(out_job, 0, sizeof(*out_job));
    out_job->kind = DC_JOB_SEND_DM;
    out_job->user_id = user_id;
    out_job->aux_id = 0xED4245u;
    int n = snprintf(out_job->payload, sizeof(out_job->payload),
                     "You need Level %u to post links or images (current: %u). Use /rank to check stats.",
                     (unsigned)min_lvl, (unsigned)cur_lvl);
    if (n <= 0 || (size_t)n >= sizeof(out_job->payload)) return false;
    out_job->payload_len = (uint8_t)n;
    return true;
}

bool dc_xp_guard_build_timeout_dm_job(DiscordId user_id, uint32_t minutes, DcJob *out_job)
{
    if (user_id == 0u || out_job == NULL) return false;
    memset(out_job, 0, sizeof(*out_job));
    out_job->kind = DC_JOB_SEND_DM;
    out_job->user_id = user_id;
    out_job->aux_id = 0xED4245u;
    int n = snprintf(out_job->payload, sizeof(out_job->payload),
                     "You have been timed out for %u min due to repeated unauthorized actions. Check /rank.",
                     (unsigned)minutes);
    if (n <= 0 || (size_t)n >= sizeof(out_job->payload)) return false;
    out_job->payload_len = (uint8_t)n;
    return true;
}
