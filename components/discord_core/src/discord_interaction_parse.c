#include "discord_interaction_parse.h"

#include <string.h>

uint32_t dc_interaction_clamp_count(uint32_t requested)
{
    return requested > DC_INTERACTION_VALUES ? DC_INTERACTION_VALUES : requested;
}

static uint32_t count_populated(const DiscordInteraction *it)
{
    uint32_t populated = 0;
    uint32_t i;

    for (i = 0; i < DC_INTERACTION_VALUES; i++) {
        if (it->values[i][0] != '\0') {
            populated++;
        }
    }

    return populated;
}

bool dc_interaction_add_value(DiscordInteraction *it, uint32_t index,
                              const char *value, size_t len)
{
    if (it == NULL || value == NULL) {
        return false;
    }

    if (index >= DC_INTERACTION_VALUES) {
        return false;
    }

    if (len > DC_INTERACTION_VALUE_LEN - 1) {
        len = DC_INTERACTION_VALUE_LEN - 1;
    }

    memcpy(it->values[index], value, len);
    it->values[index][len] = '\0';

    it->value_count = count_populated(it);

    return true;
}

/* Copies a string field, truncating to the slot. An empty source is a no-op:
 * the field stays as the caller left it, which is how "this payload carries
 * no subcommand" keeps reading as the empty string. */
static bool set_text(char *dst, size_t cap, const char *src, size_t len)
{
    if (dst == NULL || src == NULL) {
        return false;
    }
    if (len > cap - 1) {
        len = cap - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';

    return true;
}

bool dc_interaction_set_command(DiscordInteraction *it, const char *cmd, size_t len)
{
    if (it == NULL) {
        return false;
    }

    return set_text(it->command, sizeof(it->command), cmd, len);
}

bool dc_interaction_set_sub(DiscordInteraction *it, const char *sub, size_t len)
{
    if (it == NULL) {
        return false;
    }

    return set_text(it->sub, sizeof(it->sub), sub, len);
}

bool dc_interaction_set_custom_id(DiscordInteraction *it, const char *id, size_t len)
{
    if (it == NULL) {
        return false;
    }

    return set_text(it->custom_id, sizeof(it->custom_id), id, len);
}
