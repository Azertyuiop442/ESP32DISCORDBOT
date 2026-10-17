#include <stdio.h>
#include <string.h>

#include "discord_perms.h"

static int failures = 0;

static void check(int cond, const char *name)
{
    if (!cond) {
        printf("FAIL %s\n", name);
        failures++;
    } else {
        printf("ok   %s\n", name);
    }
}

/* ==========================================================================
 * Abuse cases. Each of these is something an ordinary Discord user on the
 * server can attempt. They are written as attacks, not as features.
 * ====================================================================== */

/* ATTACK: a normal member invokes an admin-only command. */
static void test_abuse_member_runs_admin_command(void)
{
    uint64_t member = 0; /* a fresh joiner has no permissions */

    check(dc_auth_check(member, DC_ACTION_SELF) == DC_AUTH_OK,
          "member may run self commands");
    check(dc_auth_check(member, DC_ACTION_ADMIN) == DC_AUTH_ERR_NO_PERMISSION,
          "member refused admin command");
    check(dc_auth_check(member, DC_ACTION_BAN) == DC_AUTH_ERR_NO_PERMISSION,
          "member refused ban");
    check(dc_auth_check(member, DC_ACTION_MODERATE) == DC_AUTH_ERR_NO_PERMISSION,
          "member refused moderation");
}

/* ATTACK: a member escalates by having one unrelated permission. */
static void test_abuse_unrelated_permission_rejected(void)
{
    uint64_t view_channel = (1ULL << 10);
    check(dc_auth_check(view_channel, DC_ACTION_BAN) == DC_AUTH_ERR_NO_PERMISSION,
          "VIEW_CHANNEL does not grant ban");
    check(dc_auth_check(view_channel, DC_ACTION_ADMIN) == DC_AUTH_ERR_NO_PERMISSION,
          "VIEW_CHANNEL does not grant admin");
}

/* ATTACK: enumerating action values to find one that passes. */
static void test_abuse_unknown_action(void)
{
    check(dc_auth_check(DC_PERM_ADMINISTRATOR, DC_ACTION_COUNT) ==
              DC_AUTH_ERR_UNKNOWN_ACTION,
          "out-of-range action refused even for admin");
    check(dc_auth_check(DC_PERM_ADMINISTRATOR, (DcAction)99) ==
              DC_AUTH_ERR_UNKNOWN_ACTION,
          "garbage action value refused");
}

/* ATTACK: a real moderator with the single relevant bit. */
static void test_moderator_is_permitted(void)
{
    check(dc_auth_check(DC_PERM_MODERATE_MEMBERS, DC_ACTION_MODERATE) == DC_AUTH_OK,
          "MODERATE_MEMBERS alone allows timeout");
    check(dc_auth_check(DC_PERM_BAN_MEMBERS, DC_ACTION_BAN) == DC_AUTH_OK,
          "BAN_MEMBERS allows ban");
    check(dc_auth_check(DC_PERM_ADMINISTRATOR, DC_ACTION_ADMIN) == DC_AUTH_OK,
          "ADMINISTRATOR allows admin commands");
}

/* ATTACK: cross-guild. An event from another guild must not be acted on. */
static void test_cross_guild_rejected(void)
{
    DcScope scope;
    dc_scope_init(&scope, 111111);
    dc_scope_add_channel(&scope, 222222);

    check(dc_scope_allows_guild(&scope, 111111), "own guild allowed");
    check(!dc_scope_allows_guild(&scope, 999999), "foreign guild refused");
    check(!dc_scope_allows_guild(&scope, 0), "zero guild refused");
}

/* ATTACK: cross-channel. A command aimed at the log channel must not
 * write into an arbitrary channel of the same guild. */
static void test_cross_channel_rejected(void)
{
    DcScope scope;
    dc_scope_init(&scope, 111111);
    dc_scope_add_channel(&scope, 222222);
    dc_scope_add_channel(&scope, 222223);

    check(dc_scope_allows_channel(&scope, 222222), "configured channel allowed");
    check(dc_scope_allows_channel(&scope, 222223), "second configured channel allowed");
    check(!dc_scope_allows_channel(&scope, 999999), "unconfigured channel refused");
}

/* ATTACK: an empty allowlist must deny everything, not allow everything. */
static void test_empty_scope_denies(void)
{
    DcScope scope;
    dc_scope_init(&scope, 111111);

    check(!dc_scope_allows_channel(&scope, 222222), "empty allowlist denies");
    check(!dc_scope_allows_channel(&scope, 0), "zero channel denied");

    DcScope unscoped;
    dc_scope_init(&unscoped, 0);
    check(!dc_scope_allows_guild(&unscoped, 111111), "zero configured guild denies all");
}

static void test_scope_bounds(void)
{
    DcScope scope;
    dc_scope_init(&scope, 111111);

    for (uint32_t i = 0; i < DC_ADMIN_SLOTS; i++) {
        check(dc_scope_add_channel(&scope, 1000 + i), "channel added");
    }
    check(!dc_scope_add_channel(&scope, 9999), "allowlist capped");
    check(!dc_scope_add_channel(&scope, 0), "zero channel refused");
    check(!dc_scope_add_channel(&scope, 1000), "duplicate channel refused");
}

/* ATTACK: the admin list is the last line of defence, so it must be exact. */
static void test_admin_list(void)
{
    DcAdminList admins;
    dc_admin_init(&admins);

    check(dc_admin_add(&admins, 42), "admin added");
    check(dc_admin_is(&admins, 42), "admin recognised");
    check(!dc_admin_add(&admins, 42), "duplicate refused");
    check(!dc_admin_add(&admins, 0), "zero id refused");
    check(!dc_admin_is(&admins, 43), "non-admin rejected");

    check(dc_admin_remove(&admins, 42), "admin removed");
    check(!dc_admin_is(&admins, 42), "removed admin no longer authorised");
    check(!dc_admin_remove(&admins, 42), "removing twice fails safely");
}

static void test_admin_list_bounded(void)
{
    DcAdminList admins;
    dc_admin_init(&admins);

    for (uint32_t i = 1; i <= DC_ADMIN_SLOTS; i++) {
        check(dc_admin_add(&admins, i), "admin added");
    }
    check(!dc_admin_add(&admins, 9999), "list capped at 8");
    check(dc_admin_count(&admins) == DC_ADMIN_SLOTS, "count is bounded");

    check(dc_admin_is(&admins, 1), "early admin still present at capacity");
    check(dc_admin_remove(&admins, 1), "removal at capacity");
    check(dc_admin_add(&admins, 9999), "space reused after removal");
}

static void test_reasons_are_safe_to_show(void)
{
    check(strcmp(dc_auth_reason(DC_AUTH_OK), "authorized") == 0, "ok reason");
    check(strlen(dc_auth_reason(DC_AUTH_ERR_NO_PERMISSION)) > 0, "denial has a reason");
    check(strstr(dc_auth_reason(DC_AUTH_ERR_UNKNOWN_ACTION), "unknown") != NULL,
          "unknown action is explained");
}

static void test_footprint(void)
{
    printf("     auth footprint = %zu B\n", dc_auth_footprint());
    check(dc_auth_footprint() < 1024, "authorization state under 1 KiB");
}

int main(void)
{
    test_abuse_member_runs_admin_command();
    test_abuse_unrelated_permission_rejected();
    test_abuse_unknown_action();
    test_moderator_is_permitted();
    test_cross_guild_rejected();
    test_cross_channel_rejected();
    test_empty_scope_denies();
    test_scope_bounds();
    test_admin_list();
    test_admin_list_bounded();
    test_reasons_are_safe_to_show();
    test_footprint();

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}