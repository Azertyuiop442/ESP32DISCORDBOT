#ifndef DISCORD_INTERACTION_PARSE_H
#define DISCORD_INTERACTION_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "discord_feature.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t dc_interaction_clamp_count(uint32_t requested);

/**
 * Stores one option/modal answer at its positional index.
 *
 * INDEX IS THE OPTION ORDER on the wire, and an empty answer still
 * consumes its index: the decoder writes the empty string there. So
 * it->value_count counts NON-EMPTY answers, not the number of slots the
 * payload touched, and it is NOT an index bound. Reading a field as
 * `value_count >= 3 ? values[2] : ""` is wrong for any form whose second
 * field can be left blank: the third answer is at values[2] regardless, and
 * the gate then throws it away. Read values[i] positionally and test the
 * string for emptiness; reserve value_count for "did anything arrive at
 * all".
 */
bool dc_interaction_add_value(DiscordInteraction *it, uint32_t index,
                              const char *value, size_t len);

bool dc_interaction_set_command(DiscordInteraction *it, const char *cmd, size_t len);

/**
 * Records the subcommand name (see DiscordInteraction.sub).
 *
 * The decoder calls this once, for the innermost option map that nests
 * another options list; a command with no subcommand leaves it untouched.
 */
bool dc_interaction_set_sub(DiscordInteraction *it, const char *sub, size_t len);

bool dc_interaction_set_custom_id(DiscordInteraction *it, const char *id, size_t len);

#ifdef __cplusplus
}
#endif

#endif