#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "discord_feature.h"
#include "discord_interaction_parse.h"

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

static void test_value_clamp(void)
{
    DiscordInteraction it;
    memset(&it, 0, sizeof(it));

    check(dc_interaction_clamp_count(6) == DC_INTERACTION_VALUES, "count clamps to the slot count");
    check(dc_interaction_clamp_count(UINT32_MAX) == DC_INTERACTION_VALUES, "UINT32_MAX clamps to the slot count");
    check(dc_interaction_clamp_count(2) == 2, "count below the cap is unchanged");

    check(!dc_interaction_add_value(&it, 0, NULL, 1), "NULL value refused");

    check(dc_interaction_add_value(&it, 0, "a", 1), "value 0 stored");
    check(it.value_count == 1, "value_count counts the one populated slot");

    check(dc_interaction_add_value(&it, 3, "d", 1), "value 3 stored");
    check(it.value_count == 2, "value_count counts populated slots not the highest index");

    check(dc_interaction_add_value(&it, 4, "e", 1), "value 4 stored (five slots)");
    check(it.value_count == 3, "fifth slot counts");
    check(!dc_interaction_add_value(&it, 5, "f", 1), "value 5 refused");
    check(it.value_count == 3, "refused index leaves value_count alone");
    check(it.values[1][0] == '\0' && it.values[2][0] == '\0', "untouched slots stay empty");

    check(!dc_interaction_add_value(&it, UINT32_MAX, "f", 1),
          "UINT32_MAX index refused without overflow");
    check(it.value_count == 3, "UINT32_MAX index leaves value_count alone");

    check(!dc_interaction_add_value(NULL, 0, "x", 1), "NULL interaction refused");
}

static void test_value_count_semantics(void)
{
    DiscordInteraction it;
    memset(&it, 0, sizeof(it));

    check(it.value_count == 0, "zeroed interaction reports no values");

    check(dc_interaction_add_value(&it, 3, "third", 5), "index 3 alone stored");
    check(it.value_count == 1, "index 3 alone counts as one populated slot");

    check(dc_interaction_add_value(&it, 0, "first", 5), "index 0 stored after index 3");
    check(it.value_count == 2, "index 3 then index 0 counts two populated slots");
    check(strcmp(it.values[3], "third") == 0, "index 3 content intact after index 0 write");
    check(strcmp(it.values[0], "first") == 0, "index 0 content intact");

    check(dc_interaction_add_value(&it, 0, "FIRST", 5), "index 0 rewritten");
    check(it.value_count == 2, "rewriting one slot does not double count");
    check(strcmp(it.values[0], "FIRST") == 0, "rewritten content replaces the old");

    check(dc_interaction_add_value(&it, 1, "second", 6), "index 1 stored");
    check(dc_interaction_add_value(&it, 2, "2nd", 3), "index 2 stored");
    check(it.value_count == 4, "four populated slots report four");
    check(it.value_count <= DC_INTERACTION_VALUES, "value_count never exceeds the slot count");
}

static void test_value_termination(void)
{
    DiscordInteraction it;
    char big_value[300];
    char big[64];

    memset(big_value, 'B', sizeof(big_value));
    memset(big, 'A', sizeof(big));

    memset(&it, 'x', sizeof(it));
    it.value_count = 0;

    check(dc_interaction_add_value(&it, 0, "abcdefghij", 10), "short value stored verbatim");
    check(it.values[0][10] == '\0', "short value is NUL terminated");
    check(strlen(it.values[0]) == 10, "value below the cap is not truncated");

    check(dc_interaction_add_value(&it, 0, big_value, sizeof(big_value)),
          "oversized value accepted");
    check(it.values[0][DC_INTERACTION_VALUE_LEN - 1] == '\0',
          "truncated value is NUL terminated");
    check(strlen(it.values[0]) == DC_INTERACTION_VALUE_LEN - 1, "value truncated to 255");

    memset(&it, 'x', sizeof(it));
    it.value_count = 0;

    check(dc_interaction_set_command(&it, big, sizeof(big)), "oversized command accepted");
    check(it.command[sizeof(it.command) - 1] == '\0', "truncated command is NUL terminated");
    check(strlen(it.command) == sizeof(it.command) - 1, "command truncated to 31");

    memset(&it, 'x', sizeof(it));
    it.value_count = 0;

    check(dc_interaction_set_custom_id(&it, big, sizeof(big)),
          "oversized custom_id accepted");
    check(it.custom_id[sizeof(it.custom_id) - 1] == '\0',
          "truncated custom_id is NUL terminated");
    check(strlen(it.custom_id) == sizeof(it.custom_id) - 1, "custom_id truncated to 63");
}

static void test_sub_field(void)
{
    DiscordInteraction it;
    char big[64];

    memset(big, 'S', sizeof(big));

    memset(&it, 0, sizeof(it));
    check(it.sub[0] == '\0', "a zeroed interaction carries no subcommand");
    check(!dc_interaction_set_sub(NULL, "open", 4), "NULL interaction refused");
    check(!dc_interaction_set_sub(&it, NULL, 4), "NULL sub refused");

    check(dc_interaction_set_sub(&it, "open", 4), "subcommand stored");
    check(strcmp(it.sub, "open") == 0, "subcommand reads back verbatim");

    memset(&it, 'x', sizeof(it));
    check(dc_interaction_set_sub(&it, big, sizeof(big)),
          "oversized subcommand accepted");
    check(it.sub[sizeof(it.sub) - 1] == '\0',
          "truncated subcommand is NUL terminated");
    check(strlen(it.sub) == sizeof(it.sub) - 1,
          "subcommand truncated to 31");

    /* The field the ring routes on: a command with subcommands reaches one
     * handler, so the branch has to be able to read the name back. */
    memset(&it, 0, sizeof(it));
    (void)dc_interaction_set_command(&it, "bug", 3);
    check(dc_interaction_set_sub(&it, "status", 6),
          "subcommand set alongside a command name");
    check(strcmp(it.command, "bug") == 0 && strcmp(it.sub, "status") == 0,
          "command and subcommand are separate fields, neither clobbers the "
          "other");
}

static void test_empty_slot_keeps_position(void)
{
    DiscordInteraction it;

    /* The exact shape a modal submit with a skipped field arrives in, and
     * the trap every handler has to avoid: value_count counts NON-EMPTY
     * answers, so it is not an index bound and a `value_count >= 3` gate
     * throws away the third field. */
    memset(&it, 0, sizeof(it));
    check(dc_interaction_add_value(&it, 0, "title", 5), "field 0 stored");
    check(dc_interaction_add_value(&it, 1, "desc", 4), "field 1 stored");
    check(dc_interaction_add_value(&it, 2, "", 0), "field 2 skipped, empty "
                                                   "string keeps its index");
    check(dc_interaction_add_value(&it, 3, "win", 3), "field 3 stored after "
                                                     "the skipped one");
    check(it.value_count == 3u, "value_count counts three non-empty answers, "
                                "not four touched slots");
    check(it.values[3][0] == 'w', "field 3 stays at index 3: read it "
                                  "positionally, never through value_count");
    check(it.values[2][0] == '\0', "the skipped field reads as an empty "
                                   "string at its own index");
}

int main(void)
{
    test_value_clamp();
    test_value_count_semantics();
    test_value_termination();
    test_sub_field();
    test_empty_slot_keeps_position();

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}