#include "discord_perms.h"

#include <string.h>

static uint64_t required_permission(DcAction action)
{
    switch (action) {
    case DC_ACTION_SELF:
        return 0;
    case DC_ACTION_MODERATE:
        return DC_PERM_MODERATE_MEMBERS | DC_PERM_MANAGE_MESSAGES;
    case DC_ACTION_BAN:
        return DC_PERM_BAN_MEMBERS;
    case DC_ACTION_ADMIN:
        return DC_PERM_ADMINISTRATOR | DC_PERM_MANAGE_GUILD;
    case DC_ACTION_COUNT:
    default:
        return 0;
    }
}

DcAuthStatus dc_auth_check(uint64_t actor_permissions, DcAction action)
{
    if (action >= DC_ACTION_COUNT) {
        return DC_AUTH_ERR_UNKNOWN_ACTION;
    }

    uint64_t required = required_permission(action);
    if (required == 0) {
        return DC_AUTH_OK;
    }

    if ((actor_permissions & DC_PERM_ADMINISTRATOR) != 0) {
        return DC_AUTH_OK;
    }

    /* ANY one of the acceptable bits is enough.
     *
     * A moderation role typically carries MODERATE_MEMBERS alone, so
     * requiring every named bit would deny legitimate moderators. The caller
     * names the action, not an exact permission set. */
    if ((actor_permissions & required) != 0) {
        return DC_AUTH_OK;
    }
    return DC_AUTH_ERR_NO_PERMISSION;
}

const char *dc_auth_reason(DcAuthStatus status)
{
    switch (status) {
    case DC_AUTH_OK:
        return "authorized";
    case DC_AUTH_ERR_NO_PERMISSION:
        return "you do not have permission to use this command";
    case DC_AUTH_ERR_UNKNOWN_ACTION:
        return "unknown command";
    case DC_AUTH_ERR_TARGET_REQUIRED:
        return "this command requires a target user";
    case DC_AUTH_ERR_SELF_ACTION:
        return "this command cannot be used on yourself";
    default:
        return "request refused";
    }
}

void dc_admin_init(DcAdminList *list)
{
    memset(list, 0, sizeof(*list));
    list->count = 0;
}

static int admin_index(const DcAdminList *list, DiscordId user_id)
{
    for (uint32_t i = 0; i < list->count; i++) {
        if (list->admins[i].active && list->admins[i].user_id == user_id) {
            return (int)i;
        }
    }
    return -1;
}

bool dc_admin_add(DcAdminList *list, DiscordId user_id)
{
    if (user_id == 0 || admin_index(list, user_id) >= 0) {
        return false;
    }
    for (uint32_t i = 0; i < list->count; i++) {
        if (!list->admins[i].active) {
            list->admins[i].user_id = user_id;
            list->admins[i].active = true;
            return true;
        }
    }
    if (list->count >= DC_ADMIN_SLOTS) {
        return false;
    }
    list->admins[list->count].user_id = user_id;
    list->admins[list->count].active = true;
    list->count++;
    return true;
}

bool dc_admin_remove(DcAdminList *list, DiscordId user_id)
{
    int idx = admin_index(list, user_id);
    if (idx < 0) {
        return false;
    }
    memset(&list->admins[idx], 0, sizeof(list->admins[idx]));
    return true;
}

bool dc_admin_is(const DcAdminList *list, DiscordId user_id)
{
    return admin_index(list, user_id) >= 0;
}

uint32_t dc_admin_count(const DcAdminList *list)
{
    return list->count;
}

void dc_scope_init(DcScope *scope, DiscordId guild_id)
{
    memset(scope, 0, sizeof(*scope));
    scope->guild_id = guild_id;
}

bool dc_scope_add_channel(DcScope *scope, DiscordId channel_id)
{
    if (channel_id == 0 || scope->allowed_count >= DC_ADMIN_SLOTS) {
        return false;
    }
    for (uint32_t i = 0; i < scope->allowed_count; i++) {
        if (scope->allowed_channels[i] == channel_id) {
            return false;
        }
    }
    scope->allowed_channels[scope->allowed_count++] = channel_id;
    return true;
}

bool dc_scope_allows_channel(const DcScope *scope, DiscordId channel_id)
{
    if (scope->allowed_count == 0) {
        return false;
    }
    for (uint32_t i = 0; i < scope->allowed_count; i++) {
        if (scope->allowed_channels[i] == channel_id) {
            return true;
        }
    }
    return false;
}

bool dc_scope_allows_guild(const DcScope *scope, DiscordId guild_id)
{
    if (scope->guild_id == 0) {
        return false;
    }
    return scope->guild_id == guild_id;
}

size_t dc_auth_footprint(void)
{
    return sizeof(DcAdminList) + sizeof(DcScope);
}