#ifndef DISCORD_WS_DECODE_H
#define DISCORD_WS_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "discord_ackq.h"
#include "discord_etf.h"
#include "discord_feature.h"
#include "discord_slice.h"
#include "discord_ws.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The ETF-walking half of the gateway transport.
 *
 * discord_ws.c owns the connection and the gateway state machine;
 * this header is the seam between the two halves. The decode half
 * owns every byte of ETF walking: the envelope walk that pulls op,
 * s and t out of a frame and captures the raw "d" span, and the
 * "d"-span walks that decode HELLO, INVALID_SESSION and DISPATCH
 * payloads. discord_ws.c never touches a DiscordEtfReader directly
 * except through the entry points below, so the two halves can never
 * disagree about how a byte is consumed: both walk with the same
 * reader functions in this component, which consume bytes exactly
 * like the fixed core decoder in components/discord_etf, standard
 * Erlang external term format as discord/erlpack produces it.
 *
 * WIRING: the decode half feeds two sinks, the DcAckq every
 * interaction ACK is pushed into and the bot layer's interaction
 * handler. dc_ws_start binds both once, before the client starts,
 * through discord_ws_decode_wire. The decode half is the ACK
 * queue's single producer and honors the SPSC contract in
 * discord_ackq.h: it yields with vTaskDelay(0) after every push.
 *
 * THREADING: every entry point runs on the esp_websocket_client
 * task, inside the event handler, so no entry point blocks and no
 * two of them run at once.
 */

/**
 * One decoded gateway frame.
 *
 * The envelope fields op, seq, have_seq and type are filled by
 * discord_ws_decode_frame, which also hands the raw "d" span back.
 * The remaining fields are filled by the "d"-span walks: interval
 * by the HELLO walk, resumable by the INVALID_SESSION walk, and
 * the session fields by the READY walk, which the transport copies
 * into the DcGateway on the READY transition. A field the payload
 * does not carry keeps its zero value and its have flag stays
 * false, and a frame the caller zeroed before decoding stays
 * zero in every field the payload leaves untouched.
 */
typedef struct {
    uint32_t op;
    uint32_t seq;
    bool have_seq;
    DiscordSlice type;
    uint32_t interval;
    bool resumable;
    char session_id[64];
    char resume_url[128];
    bool have_session_id;
    bool have_resume_url;
} DcWsFrame;

/**
 * The two sinks the decode half feeds, bound once.
 *
 * ackq is the DcAckq this component owns; every interaction ACK is
 * pushed into it under the SPSC contract, with a vTaskDelay(0)
 * after each push. interaction_handler is the bot layer's decoded
 * interaction sink and runs on the connection task, so it must
 * return quickly, must not block and must not call dc_ackq_push.
 * Both pointers are borrowed, never owned. A NULL ackq disables ACK
 * enqueueing; a NULL handler only silences the callback.
 */
typedef struct {
    DcAckq *ackq;
    DcWsInteractionHandler interaction_handler;
    DcWsMessageHandler message_handler;
    /* Returns the static modal document for a slash command and its
     * subcommand (empty string when the command has none), or NULL to
     * defer normally. Borrowed flash, valid forever. */
    const char *(*modal_for)(const char *command, const char *sub);
    /* Per-invocation modal builder (channel-in-custom_id and friends).
     * Runs before modal_for; NULL return falls back to it. */
    const char *(*modal_build)(const DiscordInteraction *it, char *buf,
                               size_t cap);
    /* Commands whose deferred ACK must be ephemeral (data.flags 64):
     * true turns the type-5 ACK ephemeral, and the handler's later edit
     * of @original inherits the invoker-only visibility. Consulted only
     * when no modal opens; NULL flags none. */
    bool (*ephemeral_for)(const char *command);
    DcWsMemberHandler member_handler;
} DcWsDecodeSinks;

/** Binds the ACK queue and the interaction handler. Call once. */
void discord_ws_decode_wire(const DcWsDecodeSinks *sinks);

/**
 * Parses the envelope of one gateway frame.
 *
 * Walks the top-level map, applies the op, s and t scalars to
 * frame, and captures the raw "d" value, tag byte included, as the
 * span *d_data..*d_data + *d_len. *d_data stays NULL when the frame
 * carries no "d" key, which is the caller's signal not to run a
 * "d" walk. Returns false when the frame is not the shape the walk
 * assumed: a bad top-level tag, a truncated or over-nested value,
 * or a pair count that does not match the pairs walked, the last
 * checked through discord_etf_end_map. A false return means the
 * frame must be counted as dropped and frame must not be read.
 */
bool discord_ws_decode_frame(const uint8_t *data, size_t len,
                             DcWsFrame *frame, const uint8_t **d_data,
                             size_t *d_len);

/**
 * HELLO payload walk: the heartbeat interval in milliseconds.
 *
 * Returns the heartbeat_interval field of the map payload, or 0
 * when the payload is not a map or carries no such field.
 */
uint32_t discord_ws_decode_hello(const uint8_t *d_data, size_t d_len);

/**
 * INVALID_SESSION payload walk: the resumable boolean.
 *
 * Reads the payload as one ETF scalar and reports whether it is the
 * atom true. A payload that is not a scalar boolean reads as not
 * resumable, which is the safe direction: an unresumable session
 * costs one IDENTIFY, a misread resumable one costs a stuck bot.
 */
bool discord_ws_decode_invalid_session(const uint8_t *d_data,
                                       size_t d_len);

/**
 * READY payload walk: fills the session fields of the frame.
 *
 * Copies the session_id and resume_gateway_url string fields into
 * the frame and raises the matching have flags. Other fields of the
 * payload are skipped. The transport copies the fields into the
 * DcGateway on the READY transition.
 */
void discord_ws_decode_ready(DcWsFrame *frame, const uint8_t *d_data,
                             size_t d_len);

/**
 * DISPATCH payload walk: decodes the payload the frame's type names.
 *
 * Walks the "d" span of a frame whose envelope decode_frame
 * accepted and whose op is DC_GW_OP_DISPATCH. An
 * INTERACTION_CREATE payload is decoded in full and delivered
 * through the wired sinks: an ACK is enqueued for PING,
 * APPLICATION_COMMAND, MESSAGE_COMPONENT and MODAL_SUBMIT
 * interactions, and the handler receives every decoded interaction. A READY payload fills
 * the session fields. Any other type, and any payload that is not a
 * map, is skipped without a side effect.
 */
void discord_ws_decode_dispatch(DcWsFrame *frame, const uint8_t *d_data,
                                size_t d_len);

#ifdef __cplusplus
}
#endif

#endif
