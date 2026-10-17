#include "discord_xp_roles.h"
#include <string.h>

void dc_xp_roles_init(DcXpRolesTable *tbl)
{
    if (tbl == NULL) {
        return;
    }
    memset(tbl->bindings, 0, sizeof(tbl->bindings));
    tbl->count = 0u;
}

bool dc_xp_roles_bind(DcXpRolesTable *tbl, uint8_t level, uint64_t role_id)
{
    if (tbl == NULL || level == 0u || role_id == 0u) {
        return false;
    }

    /* Update existing binding if level is already registered */
    for (uint8_t i = 0u; i < tbl->count; i++) {
        if (tbl->bindings[i].configured && tbl->bindings[i].level == level) {
            tbl->bindings[i].role_id = role_id;
            return true;
        }
    }

    if (tbl->count >= DC_XP_MAX_ROLE_REWARDS) {
        return false; /* Table full */
    }

    tbl->bindings[tbl->count].level = level;
    tbl->bindings[tbl->count].role_id = role_id;
    tbl->bindings[tbl->count].configured = true;
    tbl->count++;
    return true;
}

uint64_t dc_xp_roles_find(const DcXpRolesTable *tbl, uint8_t level)
{
    if (tbl == NULL || level == 0u) {
        return 0u;
    }
    for (uint8_t i = 0u; i < tbl->count; i++) {
        if (tbl->bindings[i].configured && tbl->bindings[i].level == level) {
            return tbl->bindings[i].role_id;
        }
    }
    return 0u;
}

uint8_t dc_xp_roles_collect_rewards(const DcXpRolesTable *tbl, uint8_t old_level,
                                    uint8_t new_level, uint64_t *out_roles, uint8_t max_out)
{
    if (tbl == NULL || out_roles == NULL || max_out == 0u || new_level <= old_level) {
        return 0u;
    }

    uint8_t collected = 0u;
    for (uint8_t i = 0u; i < tbl->count; i++) {
        const DcXpRoleBinding *b = &tbl->bindings[i];
        if (b->configured && b->level > old_level && b->level <= new_level) {
            out_roles[collected++] = b->role_id;
            if (collected >= max_out) {
                break;
            }
        }
    }
    return collected;
}
