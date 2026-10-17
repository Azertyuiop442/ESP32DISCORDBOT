#include "discord_feature.h"

#include <string.h>

void dc_registry_init(DiscordFeatureRegistry *r)
{
    memset(r, 0, sizeof(*r));
    r->count = 0;
    r->required_intents = 0;
}

bool dc_registry_add(DiscordFeatureRegistry *r, const DiscordFeature *f)
{
    if (!f || !f->name || r->count >= DC_FEATURE_MAX) {
        return false;
    }
    size_t len = strlen(f->name);
    if (len == 0 || len >= DC_FEATURE_NAME_MAX) {
        return false;
    }

    for (uint32_t i = 0; i < r->count; i++) {
        if (strcmp(r->names[i], f->name) == 0) {
            return false;
        }
    }

    memset(r->names[r->count], 0, DC_FEATURE_NAME_MAX);
    memcpy(r->names[r->count], f->name, len);

    r->items[r->count] = f;
    r->required_intents |= f->required_intents;
    r->count++;
    return true;
}

uint32_t dc_registry_intents(const DiscordFeatureRegistry *r)
{
    return r->required_intents;
}

bool dc_registry_has(const DiscordFeatureRegistry *r, const char *name)
{
    if (!name || name[0] == '\0') {
        return false;
    }
    for (uint32_t i = 0; i < r->count; i++) {
        if (strcmp(r->names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

void dc_registry_dispatch_message(const DiscordFeatureRegistry *r,
                                  const DiscordMessage *msg)
{
    for (uint32_t i = 0; i < r->count; i++) {
        if (r->items[i]->on_message) {
            r->items[i]->on_message(msg);
        }
    }
}

void dc_registry_dispatch_interaction(const DiscordFeatureRegistry *r,
                                       const DiscordInteraction *it)
{
    for (uint32_t i = 0; i < r->count; i++) {
        if (r->items[i]->on_interaction) {
            r->items[i]->on_interaction(it);
        }
    }
}

const DiscordFeature *dc_registry_find_command(const DiscordFeatureRegistry *r,
                                               const char *command)
{
    if (r == NULL || command == NULL || command[0] == '\0') {
        return NULL;
    }

    for (uint32_t i = 0; i < r->count; i++) {
        if (strcmp(r->names[i], command) == 0) {
            return r->items[i];
        }
    }
    return NULL;
}

bool dc_registry_route_interaction(DiscordFeatureRegistry *r,
                                   const DiscordInteraction *it)
{
    if (r == NULL || it == NULL) {
        return false;
    }

    const DiscordFeature *f = dc_registry_find_command(r, it->command);
    if (f == NULL) {
        return false;
    }

    if (f->on_interaction) {
        f->on_interaction(it);
    }
    return true;
}

bool dc_registry_route_component(DiscordFeatureRegistry *r,
                                 const DiscordInteraction *it)
{
    char owner[DC_FEATURE_NAME_MAX];
    size_t i;

    if (r == NULL || it == NULL) {
        return false;
    }
    /* Components carry no command: custom_id names the owner as
     * "feature:action:args". Anything else is not routable. */
    if (it->command[0] != '\0' || it->custom_id[0] == '\0') {
        return false;
    }
    for (i = 0; i < sizeof(owner) - 1u && it->custom_id[i] != '\0'; i++) {
        if (it->custom_id[i] == ':') break;
        owner[i] = it->custom_id[i];
    }
    owner[i] = '\0';
    if (owner[0] == '\0') {
        return false;
    }

    const DiscordFeature *f = dc_registry_find_command(r, owner);
    if (f == NULL) {
        return false;
    }

    if (f->on_interaction) {
        f->on_interaction(it);
    }
    return true;
}