#include "discord_xp_cooldown.h"
#include <string.h>

#define DC_MS_PER_DAY 86400000ULL /* 24 * 3600 * 1000 */

static uint32_t day_key_from_ms(uint64_t now_ms)
{
    return (uint32_t)(now_ms / DC_MS_PER_DAY);
}

void dc_xp_cooldown_init(DcXpCooldownTable *tbl, uint64_t debounce_ms, uint32_t daily_cap)
{
    if (tbl == NULL) {
        return;
    }
    memset(tbl->slots, 0, sizeof(tbl->slots));
    tbl->cursor = 0u;
    tbl->debounce_ms = (debounce_ms > 0u) ? debounce_ms : DC_XP_DEFAULT_DEBOUNCE_MS;
    tbl->daily_cap = (daily_cap > 0u) ? daily_cap : DC_XP_DEFAULT_DAILY_CAP;
}

void dc_xp_cooldown_set_daily_cap(DcXpCooldownTable *tbl, uint32_t daily_cap)
{
    if (tbl != NULL) {
        tbl->daily_cap = daily_cap;
    }
}

void dc_xp_cooldown_set_debounce(DcXpCooldownTable *tbl, uint64_t debounce_ms)
{
    if (tbl != NULL) {
        tbl->debounce_ms = debounce_ms;
    }
}

bool dc_xp_cooldown_try_acquire(DcXpCooldownTable *tbl, DiscordId user_id,
                                uint64_t now_ms, uint32_t requested_xp,
                                uint32_t *out_awarded_xp)
{
    if (tbl == NULL || user_id == 0u || requested_xp == 0u) {
        return false;
    }

    uint32_t current_day = day_key_from_ms(now_ms);
    DcXpCooldownSlot *slot = NULL;

    /* 1. Search for existing entry */
    for (uint32_t i = 0u; i < DC_XP_COOLDOWN_SLOTS; i++) {
        if (tbl->slots[i].occupied && tbl->slots[i].user_id == user_id) {
            slot = &tbl->slots[i];
            break;
        }
    }

    /* 2. User found: evaluate day rollover and debounce */
    if (slot != NULL) {
        if (slot->day_key != current_day) {
            slot->day_key = current_day;
            slot->xp_today = 0u;
        }

        /* Check 24h daily cap */
        if (tbl->daily_cap > 0u && slot->xp_today >= tbl->daily_cap) {
            return false; /* Daily XP ceiling reached */
        }

        /* Check conversational debounce */
        if (now_ms >= slot->last_xp_ms && (now_ms - slot->last_xp_ms) < tbl->debounce_ms) {
            return false; /* Debounce active */
        }
    } else {
        /* User not found: allocate new or evict oldest */
        uint32_t chosen_idx = tbl->cursor;
        for (uint32_t i = 0u; i < DC_XP_COOLDOWN_SLOTS; i++) {
            uint32_t idx = (tbl->cursor + i) % DC_XP_COOLDOWN_SLOTS;
            DcXpCooldownSlot *s = &tbl->slots[idx];
            if (!s->occupied || s->day_key != current_day) {
                chosen_idx = idx;
                break;
            }
        }

        slot = &tbl->slots[chosen_idx];
        slot->user_id = user_id;
        slot->day_key = current_day;
        slot->xp_today = 0u;
        slot->occupied = true;
        tbl->cursor = (chosen_idx + 1u) % DC_XP_COOLDOWN_SLOTS;
    }

    /* Calculate allowed XP clamped by remaining daily quota */
    uint32_t granted = requested_xp;
    if (tbl->daily_cap > 0u) {
        uint32_t remaining = tbl->daily_cap - slot->xp_today;
        if (granted > remaining) {
            granted = remaining;
        }
    }

    slot->xp_today += granted;
    slot->last_xp_ms = now_ms;

    if (out_awarded_xp != NULL) {
        *out_awarded_xp = granted;
    }
    return true;
}

uint32_t dc_xp_cooldown_get_daily_xp(const DcXpCooldownTable *tbl, DiscordId user_id, uint64_t now_ms)
{
    if (tbl == NULL || user_id == 0u) {
        return 0u;
    }
    uint32_t current_day = day_key_from_ms(now_ms);
    for (uint32_t i = 0u; i < DC_XP_COOLDOWN_SLOTS; i++) {
        const DcXpCooldownSlot *s = &tbl->slots[i];
        if (s->occupied && s->user_id == user_id) {
            return (s->day_key == current_day) ? s->xp_today : 0u;
        }
    }
    return 0u;
}

uint64_t dc_xp_cooldown_remaining_ms(const DcXpCooldownTable *tbl, DiscordId user_id, uint64_t now_ms)
{
    if (tbl == NULL || user_id == 0u) {
        return 0u;
    }
    for (uint32_t i = 0u; i < DC_XP_COOLDOWN_SLOTS; i++) {
        const DcXpCooldownSlot *s = &tbl->slots[i];
        if (s->occupied && s->user_id == user_id) {
            if (now_ms >= s->last_xp_ms) {
                uint64_t elapsed = now_ms - s->last_xp_ms;
                if (elapsed < tbl->debounce_ms) {
                    return tbl->debounce_ms - elapsed;
                }
            }
            return 0u;
        }
    }
    return 0u;
}

void dc_xp_cooldown_reset(DcXpCooldownTable *tbl, DiscordId user_id)
{
    if (tbl == NULL || user_id == 0u) {
        return;
    }
    for (uint32_t i = 0u; i < DC_XP_COOLDOWN_SLOTS; i++) {
        if (tbl->slots[i].occupied && tbl->slots[i].user_id == user_id) {
            memset(&tbl->slots[i], 0, sizeof(DcXpCooldownSlot));
            break;
        }
    }
}

