#ifndef DISCORD_FEATURE_H
#define DISCORD_FEATURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "discord_slice.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DC_FEATURE_MAX 28
#define DC_FEATURE_NAME_MAX 24

/**
 * Discord gateway intent bits, sent verbatim in IDENTIFY.
 *
 * These are the wire values from the Discord docs (and
 * discord-api-types/gateway/v10.d.ts), not a compact internal encoding:
 * dc_registry_intents ORs the flags and send_identify puts the result in
 * the IDENTIFY payload as-is, so any bit here that does not match the
 * gateway mask silently withholds the event. (1u << 9) for GUILD_MESSAGES,
 * not (1u << 2), is what makes MESSAGE_CREATE arrive at all.
 */
#define DC_INTENT_GUILDS          (1u << 0)  /* 1        */
#define DC_INTENT_GUILD_MEMBERS   (1u << 1)  /* 2        */
#define DC_INTENT_GUILD_MESSAGES  (1u << 9)  /* 512      */
#define DC_INTENT_MESSAGE_CONTENT (1u << 15) /* 32768    */
#define DC_INTENT_DIRECT_MESSAGES (1u << 12) /* 4096     */

typedef struct {
    DiscordId guild_id;
    DiscordId channel_id;
    DiscordId author_id;
    DiscordId message_id;
    DiscordSlice content;
    uint8_t   flags;
} DiscordMessage;

/** Set in DiscordMessage.flags when the author is a bot. */
#define DC_MSG_FLAG_BOT 0x01u
/** Set when the message carries an image attachment (content_type). */
#define DC_MSG_FLAG_IMAGE 0x02u

#define DC_INTERACTION_VALUES 5
#define DC_INTERACTION_VALUE_LEN 256
#define DC_INTERACTION_TOKEN_MAX 256
#define DC_INTERACTION_SUB_MAX 32

typedef struct {
    DiscordId id;
    DiscordId application_id;
    DiscordId user_id;
    DiscordId channel_id;
    DiscordId guild_id;
    DiscordId message_id;
    uint32_t  type;
    char      command[32];
    /**
     * Subcommand name, empty when the command has none.
     *
     * Discord nests a subcommand as data.options[0] = {"name":"open",
     * "options":[...the real values...]}, so the subcommand name is not a
     * value and cannot be recovered from values[] once the flat option
     * walk has run. A feature that owns several subcommands routes on
     * it->command and branches on it->sub; one that owns exactly one
     * subcommand-shaped form declares DiscordFeature.modal_sub and lets
     * the ACK path open the modal for that subcommand only.
     */
    char      sub[DC_INTERACTION_SUB_MAX];
    char      custom_id[64];
    uint32_t  value_count;
    char      values[DC_INTERACTION_VALUES][DC_INTERACTION_VALUE_LEN];
    char      token[DC_INTERACTION_TOKEN_MAX]; /**< Interaction token; valid for 15 min. */
} DiscordInteraction;

/**
 * What an ACK actually needs.
 *
 * The interaction token and ID are the only two fields required to answer
 * within the 3 second deadline. Carrying the full DiscordInteraction through
 * the ACK queue would cost ~1.7 KiB per entry, most of which is the
 * values[5][256] block the ACK never reads.
 *
 * The handler still receives the full interaction; the queue only delays
 * the acknowledgement, which is a pure network call.
 */
typedef struct {
    DiscordId id;
    char      token[256];
    uint8_t   kind;
    bool      deferred;
    /* Kind 5 only: send the deferred ACK as an ephemeral one
     * ({"type":5,"data":{"flags":64}}), so every later edit of this
     * message stays invoker-only. Fits in padding: sizeof is unchanged. */
    bool      ephemeral;
    /* Full {"type":9,...} modal document for kind 9, borrowed flash. */
    const char *modal_json;
} DiscordAck;

typedef struct {
    const char *name;
    uint32_t    required_intents;
    uint32_t    init_priority;
    void (*on_message)(const DiscordMessage *msg);
    void (*on_interaction)(const DiscordInteraction *it);
    /* Appended late (existing positional initializers still compile:
     * missing fields zero). */
    uint32_t    flags;
    /* Static {"type":9,...} modal shown instead of the deferred ACK. */
    const char *modal_json;
    /* Subcommand that owns modal_json, NULL when the whole command does.
     * A command carrying several subcommands must not open the form for
     * all of them: "/bug open" shows the form, "/bug status fixed" has to
     * defer so its handler can run. Compared against it->sub. */
    const char *modal_sub;
} DiscordFeature;

/** Feature shows a modal on its slash command instead of deferring. */
#define DC_FEATURE_WANTS_MODAL (1u << 0)

/**
 * Feature's slash replies are ephemeral, like Node's ephemeral: true.
 *
 * Visibility is fixed when the message is created, not when it is edited,
 * so the flag acts on the deferred ACK (type 5, data.flags 64) rather than
 * on any reply: the handler's later edit of @original then inherits the
 * invoker-only visibility. A command that shows a modal for part of its
 * subcommands keeps this flag: the modal sub never defers, and the
 * deferring subs get the ephemeral ACK.
 */
#define DC_FEATURE_EPHEMERAL (1u << 1)

typedef struct {
    const DiscordFeature *items[DC_FEATURE_MAX];
    uint32_t              count;
    uint32_t              required_intents;
    char                  names[DC_FEATURE_MAX][DC_FEATURE_NAME_MAX];
} DiscordFeatureRegistry;

void dc_registry_init(DiscordFeatureRegistry *r);
bool dc_registry_add(DiscordFeatureRegistry *r, const DiscordFeature *f);
uint32_t dc_registry_intents(const DiscordFeatureRegistry *r);
bool dc_registry_has(const DiscordFeatureRegistry *r, const char *name);
void dc_registry_dispatch_message(const DiscordFeatureRegistry *r,
                                  const DiscordMessage *msg);
void dc_registry_dispatch_interaction(const DiscordFeatureRegistry *r,
                                       const DiscordInteraction *it);

/**
 * Resolves the one feature that owns a slash command name.
 *
 * Matching is case-sensitive and exact, and the decision is deliberate.
 * Discord resolves a typed command against the registered command name
 * server-side, so the name handed to the gateway is already the canonical
 * lowercase form; a member typing /Ping is answered by Discord itself, never
 * forwarded. Lower-casing here would be dead code that only widens the set of
 * inputs that resolve to a privileged handler, so the router compares bytes.
 *
 * The scan runs over the registry's copied names[][] table rather than
 * dereferencing through items[][], so the scan never dereferences a pointer
 * the caller owns. That is a property of the comparison, not a safety
 * guarantee for the result: the pointer handed back IS items[i], still owned by
 * the caller, and it dangles the moment the caller's DiscordFeature goes out
 * of scope. A caller writing find_command(&r, cmd)->on_interaction(it) gets
 * the crash this cannot prevent, because reading the handler field is the
 * caller's dereference of its own memory. Owning features by value in the
 * registry is the real fix, and it is the planned next change; until then a
 * registered feature must outlive the registry.
 *
 * Returns the owning feature, or NULL when `command` is NULL, empty, or names
 * nothing registered. A NULL `r` is also NULL. An over-long name cannot
 * overrun anything: the comparison is strcmp, which stops at the first NUL,
 * and the caller's own NUL termination is all this reads.
 */
const DiscordFeature *dc_registry_find_command(const DiscordFeatureRegistry *r,
                                               const char *command);

/**
 * Dispatches an interaction to the single feature owning it->command.
 *
 * This is the path for command dispatch. dc_registry_dispatch_interaction
 * remains the fan-out for a feature that genuinely handles every event; using
 * it for slash commands means an unknown command still reaches handlers built
 * for a specific one.
 *
 * Returns true when a feature owned the command and its handler ran. Returns
 * false when the command is unknown, an empty interaction name, or the
 * registry is NULL; in every one of those cases no handler is invoked at all.
 * A matched feature with a NULL on_interaction still returns true, since the
 * command was routed and only the handler is absent. That shape is the normal
 * one for a message-only feature, so the NULL handler is a routed-and-absent
 * case to check for, not a caller error.
 *
 * This router covers application commands only. A button, select menu or modal
 * identifies itself through it->custom_id and carries an empty it->command, so
 * such an interaction matches nothing here: the caller falls through to
 * dc_registry_route_component, which owns the "feature:action:args" prefix.
 */
bool dc_registry_route_interaction(DiscordFeatureRegistry *r,
                                   const DiscordInteraction *it);

/**
 * Dispatches a component interaction (button, select, modal) to the
 * feature owning its custom_id prefix.
 *
 * Convention, documented not enforced elsewhere: custom_id is shaped
 * "feature:action:args" and the owner is the span before the first ':'.
 * Matching is exact on that span; a custom_id with no owner, an unknown
 * owner, or a non-empty command (application commands never arrive here)
 * routes nothing and returns false. A matched feature with a NULL
 * on_interaction still returns true (routed-and-absent, same ruling as
 * the command router).
 */
bool dc_registry_route_component(DiscordFeatureRegistry *r,
                                 const DiscordInteraction *it);

#ifdef __cplusplus
}
#endif

#endif