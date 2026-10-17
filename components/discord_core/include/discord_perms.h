#ifndef DISCORD_PERMS_H
#define DISCORD_PERMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "discord_slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Discord permission bits, as sent in the interaction `member.permissions` field.
 *
 * Only the ones this framework can act on are named. The full set is available
 * at https://docs.discord.com/developers/topics/permissions.
 */
#define DC_PERM_MANAGE_GUILD     (1ULL << 5)
#define DC_PERM_KICK_MEMBERS     (1ULL << 1)
#define DC_PERM_BAN_MEMBERS      (1ULL << 2)
#define DC_PERM_MODERATE_MEMBERS (1ULL << 40)
#define DC_PERM_MANAGE_ROLES     (1ULL << 28)
#define DC_PERM_MANAGE_MESSAGES  (1ULL << 13)
#define DC_PERM_MANAGE_WEBHOOKS  (1ULL << 29)
#define DC_PERM_MANAGE_NICKNAMES (1ULL << 26)
#define DC_PERM_ADMINISTRATOR     (1ULL << 3)

/** Maximum number of accounts allowed to run privileged commands. */
#define DC_ADMIN_SLOTS 8

/** Discord roles are never fetched; permissions arrive on the interaction. */
#define DC_PERMISSIONS_MAX 64

typedef struct {
    DiscordId user_id;
    bool     active;
} DcAdmin;

typedef struct {
    DcAdmin  admins[DC_ADMIN_SLOTS];
    uint32_t count;
} DcAdminList;

/**
 * An action requiring authorization.
 *
 * Every privileged REST call the framework exposes must name one of these.
 * There is deliberately no "unprivileged" default: a new action must pick its
 * requirement explicitly, so forgetting is a visible act rather than an
 * omission.
 */
typedef enum {
    DC_ACTION_SELF = 0,      /* acts only on the caller: profile, rank, captcha */
    DC_ACTION_MODERATE,      /* timeout, delete, kick: needs MODERATE_MEMBERS   */
    DC_ACTION_BAN,           /* ban: needs BAN_MEMBERS                        */
    DC_ACTION_ADMIN,         /* config, nuke, reset: needs ADMINISTRATOR       */
    DC_ACTION_COUNT
} DcAction;

typedef enum {
    DC_AUTH_OK = 0,
    DC_AUTH_ERR_NO_PERMISSION = -1,
    DC_AUTH_ERR_UNKNOWN_ACTION = -2,
    DC_AUTH_ERR_TARGET_REQUIRED = -3,
    DC_AUTH_ERR_SELF_ACTION = -4
} DcAuthStatus;

/** True when `actor` may perform `action`, given the permissions Discord sent. */
DcAuthStatus dc_auth_check(uint64_t actor_permissions, DcAction action);

/** Human-readable reason, safe to log and to show. English, no internals. */
const char *dc_auth_reason(DcAuthStatus status);

/* Admin allowlist ------------------------------------------------------- */

void dc_admin_init(DcAdminList *list);
bool dc_admin_add(DcAdminList *list, DiscordId user_id);
bool dc_admin_remove(DcAdminList *list, DiscordId user_id);
bool dc_admin_is(const DcAdminList *list, DiscordId user_id);
uint32_t dc_admin_count(const DcAdminList *list);

/**
 * Guild and channel scoping.
 *
 * An event arrives with its own guild_id and channel_id. Acting on a channel
 * that was never configured is how a command aimed at one guild ends up
 * writing into another, so every destination is checked before the call.
 */
typedef struct {
    DiscordId guild_id;
    DiscordId allowed_channels[DC_ADMIN_SLOTS];
    uint32_t  allowed_count;
} DcScope;

void dc_scope_init(DcScope *scope, DiscordId guild_id);
bool dc_scope_add_channel(DcScope *scope, DiscordId channel_id);
bool dc_scope_allows_channel(const DcScope *scope, DiscordId channel_id);
bool dc_scope_allows_guild(const DcScope *scope, DiscordId guild_id);

size_t dc_auth_footprint(void);

#ifdef __cplusplus
}
#endif

#endif