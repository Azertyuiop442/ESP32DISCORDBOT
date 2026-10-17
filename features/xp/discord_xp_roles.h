#ifndef DISCORD_XP_ROLES_H
#define DISCORD_XP_ROLES_H

#include <stdbool.h>
#include <stdint.h>
#include "discord_slice.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DC_XP_MAX_ROLE_REWARDS 16u

typedef struct {
    uint8_t   level;
    uint64_t  role_id;
    bool      configured;
} DcXpRoleBinding;

typedef struct {
    DcXpRoleBinding bindings[DC_XP_MAX_ROLE_REWARDS];
    uint8_t         count;
} DcXpRolesTable;

/**
 * Initializes the roles rewards table.
 */
void dc_xp_roles_init(DcXpRolesTable *tbl);

/**
 * Binds a Discord role ID to be awarded when reaching a specific level.
 * Returns true on success, false if table is full or invalid parameters.
 */
bool dc_xp_roles_bind(DcXpRolesTable *tbl, uint8_t level, uint64_t role_id);

/**
 * Finds the role ID bound to an exact level (returns 0 if none).
 */
uint64_t dc_xp_roles_find(const DcXpRolesTable *tbl, uint8_t level);

/**
 * Collects all role rewards unlocked when progressing from old_level to new_level.
 * Handles single or multiple level jumps safely.
 * Returns the number of role IDs written to out_roles (bounded by max_out).
 */
uint8_t dc_xp_roles_collect_rewards(const DcXpRolesTable *tbl, uint8_t old_level,
                                    uint8_t new_level, uint64_t *out_roles, uint8_t max_out);

#ifdef __cplusplus
}
#endif

#endif /* DISCORD_XP_ROLES_H */
