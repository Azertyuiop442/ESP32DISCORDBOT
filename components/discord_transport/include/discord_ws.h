#ifndef DISCORD_WS_H
#define DISCORD_WS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "discord_ackq.h"
#include "discord_feature.h"
#include "discord_gateway.h"
#include "discord_identify.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Gateway WebSocket transport: the only owner of the connection.
 *
 * This module wraps esp_websocket_client and nothing else opens a socket.
 * It owns the three instances the pure-C core needs around the connection:
 * the DcGateway state machine, the DcAckq ACK queue and the DcIdentify
 * budget. Task 11 reaches them through the accessors below; the gateway
 * task drives the state machine, the ack task pops the queue and the
 * monitor task reads telemetry. No other task may call dc_gateway_*,
 * dc_ackq_push or dc_identify_record on these instances.
 *
 * WIRING, the contract Task 11 codes against. All five setters are called
 * once, before dc_ws_start, from the task that also calls dc_ws_start:
 *
 *   dc_ws_set_ca_store()        pinned Discord root PEM, installed with
 *                               esp_tls_set_global_ca_store(). crt_bundle_attach
 *                               stays NULL on purpose: the bundle costs 64-128
 *                               KB of flash against a 1500 KB slot.
 *   dc_ws_set_token_provider()  the bot token, read from provisioning at
 *                               runtime. No token literal exists anywhere in
 *                               this component and the token is never logged.
 *   dc_ws_set_intents()         the intent bitmask, dc_registry_intents().
 *   dc_ws_set_interaction_handler() the bot layer's decoded-interaction sink.
 *   dc_ws_set_identify_store()  the NVS store behind the IDENTIFY budget.
 *
 * dc_ws_start() validates the wiring, loads the persisted IDENTIFY budget
 * and starts the client. An unreadable store returns ESP_FAIL: running
 * unidentified burns quota, so this is the one permitted fatal-at-boot.
 * dc_ws_gateway_tick() is the whole gateway-task loop body: it ticks the
 * state machine, sends the heartbeat it may ask for and reconnects when
 * the backoff elapses. dc_ws_stop() tears the client down and clears the
 * fatal latch so a re-provisioned token can start again.
 *
 * THREADING. Two contexts touch the DcGateway, and both are sanctioned:
 * the esp_websocket_client task, through the event handler, performs every
 * event-driven transition (connected, hello, op, heartbeat ack, close), and
 * the gateway task performs every time-driven transition through
 * dc_ws_gateway_tick, the only caller of dc_gateway_tick. The two contexts
 * never share a buffer: each builds frames in its own transmit buffer, so
 * no lock is needed and none is used, matching the pure-C core, which
 * carries no RTOS primitive. Both contexts call the dc_gateway_* family on
 * the same struct without synchronisation, so a transition and a tick can
 * interleave; the consequence of a torn transition is a failed RESUME,
 * which the IDENTIFY budget bounds, never memory unsafety, because every
 * field is a single word or a fixed char array. Task 11 must not call
 * dc_gateway_* on dc_ws_gateway() from any other context and must not call
 * dc_ws_gateway_tick from the event handler.
 *
 * The ACK queue is driven under the SPSC contract in discord_ackq.h: this
 * module is the single producer, the ack task is the single consumer, and
 * the producer yields with vTaskDelay(0) after every push. A push past
 * DC_ACKQ_CAPACITY sheds the oldest entry and counts the drop inside the
 * queue; this module never blocks and never inspects queue internals.
 *
 * CLOCK. Every now_ms in this module is epoch milliseconds in the Unix
 * domain, as the DcIdentify contract requires. dc_ws_start must therefore
 * be called only after SNTP has synced; before that, time() reads near
 * zero and the budget arithmetic would be meaningless.
 *
 * FATAL CLOSES. Close 4004, 4013 and 4014 latch the module: the client is
 * destroyed, no reconnect is ever scheduled from here again, and dc_ws_start
 * refuses until dc_ws_stop resets the latch. Each reconnect issues an
 * IDENTIFY, and 1000 of those revoke the token permanently, so failing fast
 * is the only safe response. The close code is taken from the close frame
 * the client delivers as a data event with op code 8; if a build of
 * esp_websocket_client stops delivering close frames, the code reads as 0,
 * the close is treated as recoverable, and the persisted IDENTIFY budget is
 * the backstop that still bounds the quota. Confirming the close-code path
 * is a hardware verification item.
 *
 * DECODER. Gateway frames are walked with discord_etf, and the
 * whole ETF walk lives in discord_ws_decode.c behind the
 * internal header discord_ws_decode.h: the envelope walk that
 * captures the raw "d" span, and the "d"-span walks for
 * HELLO, INVALID_SESSION and DISPATCH. The decoder's public
 * API reads a key and its scalar value only together and
 * fails on container values, so the walk reads keys with
 * discord_etf_next_map_key, descends into containers with
 * discord_etf_begin_map, and reads scalar values with a local
 * reader in discord_ws_decode.c that consumes bytes identically
 * to the decoder's fixed internal one, standard Erlang
 * external term format as erlpack produces it, which is where
 * INVALID_SESSION's resumable boolean arrives as an atom. The
 * two halves are wired once by dc_ws_start through
 * discord_ws_decode_wire, which binds the DcAckq the decode
 * half pushes ACKs into, as this module's single producer
 * under the SPSC contract, and the interaction handler the
 * decode half calls. Every write into
 * DiscordInteraction.values, .command and .custom_id goes
 * through dc_interaction_add_value, dc_interaction_set_command
 * and dc_interaction_set_custom_id; nothing else in this
 * component writes those three fields.
 *
 * LIMITS. One frame must fit in DC_GW_RX_BUFFER, the client's receive
 * buffer; a larger frame is dropped and counted, so a READY from a bot in
 * very many guilds can be missed, which costs a fresh IDENTIFY, never a
 * crash. An interaction token longer than the ACK queue's fixed 80 byte
 * field is truncated, which fails that one ACK server side.
 */

/**
 * Receives a decoded interaction, called on the esp_websocket_client task.
 *
 * The handler runs on the connection task, so it must return quickly and
 * must not block; the full interaction is passed by pointer and is valid
 * only for the duration of the call. The handler must not call
 * dc_ackq_push: this module is the queue's only producer.
 */
typedef void (*DcWsInteractionHandler)(const DiscordInteraction *it);

/**
 * Receives a decoded message, called on the esp_websocket_client task.
 *
 * Same contract as the interaction handler: return quickly, never block,
 * the DiscordMessage (including its zero-copy content slice) is valid
 * only for the call. No ACK is involved; delivery needs the GUILD_MESSAGES
 * intent, content needs the MESSAGE_CONTENT privileged intent.
 */
typedef void (*DcWsMessageHandler)(const DiscordMessage *msg);

/**
 * Receives a guild member join/leave, on the esp_websocket_client task.
 *
 * Needs the GUILD_MEMBERS privileged intent (portal toggle). Same
 * contract: return quickly, never block. joined true = ADD, false = REMOVE.
 */
typedef void (*DcWsMemberHandler)(uint64_t guild_id, uint64_t user_id,
                                  bool joined);

/**
 * Supplies the bot token from provisioning at runtime.
 *
 * Returns a pointer to the token and sets *len to its length in bytes, or
 * returns NULL when no token is stored. The pointer must stay valid until
 * the IDENTIFY or RESUME frame built from it has been sent.
 */
typedef const char *(*DcWsTokenProvider)(size_t *len);

/** Installs the pinned Discord root PEM and the global CA store. */
void dc_ws_set_ca_store(const unsigned char *pem, size_t len);

/** Binds the runtime token source. Required before dc_ws_start. */
void dc_ws_set_token_provider(DcWsTokenProvider provider);

/** Sets the intent bitmask sent in IDENTIFY. */
void dc_ws_set_intents(uint32_t intents);

/** Registers the bot layer's decoded-interaction sink. */
void dc_ws_set_interaction_handler(DcWsInteractionHandler handler);

/** Registers the bot layer's decoded-message sink. NULL silences delivery. */
void dc_ws_set_message_handler(DcWsMessageHandler handler);

/**
 * Registers the modal lookup: static {"type":9,...} per slash command.
 *
 * Called with the command name and its subcommand name (empty string when
 * the command has none). A command whose form belongs to one subcommand
 * only has to be able to say so, which is why the subcommand travels with
 * the lookup instead of being re-derived by the caller.
 */
void dc_ws_set_modal_for(const char *(*fn)(const char *command,
                                           const char *sub));

/**
 * Registers the ephemeral lookup: slash commands whose replies are private.
 *
 * Called with the command name when an ACK is about to defer. Returning
 * true sends the type-5 ACK as a deferred ephemeral one (data.flags 64),
 * so the message the handler later edits at @original stays visible to
 * the invoker only — the device equivalent of Node's ephemeral: true.
 * Consulted only when no modal opens: a modal sub never defers, and a
 * command that mixes both (modal for "open", deferring "status") can
 * carry the flag without leaking its modal path.
 */
void dc_ws_set_ephemeral_for(bool (*fn)(const char *command));

/**
 * Registers the dynamic modal builder: per-invocation {"type":9,...}.
 *
 * Called on the esp_websocket_client task at decode time with the parsed
 * interaction (command + option values already filled) and a scratch
 * buffer the decoder owns. Returns the buffer on success (kind 9 ACK
 * with it), NULL to fall back to the static modal_for lookup. The
 * returned document is sent as-is by the ACK task, so it must be a
 * complete {"type":9,...} modal. The scratch buffer is shared: the
 * builder must fully write it before returning and keep nothing.
 */
typedef const char *(*DcWsModalBuilder)(const DiscordInteraction *it,
                                        char *buf, size_t cap);
void dc_ws_set_modal_builder(DcWsModalBuilder fn);

/** Registers the guild member join/leave sink. NULL silences delivery. */
void dc_ws_set_member_handler(DcWsMemberHandler handler);

/**
 * Stages an activity ("playing X", "streaming Y") and validates it.
 *
 * The staged value is what dc_ws_push_activity sends live and what every
 * fresh IDENTIFY replays (RESUME keeps the previous presence). Types
 * 0/2/3/5 plain; 1 streams and requires a twitch/youtube URL
 * (Discord-enforced). 4 (custom) is user-only. Returns false on any
 * refused shape. Costs 160 B static, zero at runtime.
 */
bool dc_ws_set_activity(uint8_t type, const char *name, const char *url);

/**
 * Pushes the staged activity live as OP3 PRESENCE_UPDATE (READY only).
 *
 * discord.js setActivity parity: the presence changes immediately. False
 * when the session is not READY or the send failed — the staged value
 * stays queued for the next IDENTIFY. Single caller (the interaction
 * handler task); one static tx buffer, no lock (tx is serialized by the
 * IDF client itself).
 */
bool dc_ws_push_activity(void);

/** Binds the NVS store behind the IDENTIFY budget. Required before start. */
void dc_ws_set_identify_store(const DcIdentifyStore *store);

/**
 * Loads the budget, opens the queue and starts the client.
 *
 * Returns ESP_FAIL while the fatal latch is set, when the wiring is
 * incomplete, or when the persisted IDENTIFY budget is unreadable.
 * Idempotent: a second call while running returns ESP_OK.
 */
esp_err_t dc_ws_start(void);

/** Stops and destroys the client and clears the fatal latch. */
void dc_ws_stop(void);

/**
 * Gateway-task loop body: tick the state machine and act on the result.
 *
 * now_ms is epoch milliseconds. Sends a heartbeat when the state machine
 * asks for one and reconnects when a backoff elapses. Call this at a
 * period shorter than the heartbeat interval, for example every 100 ms.
 */
void dc_ws_gateway_tick(uint64_t now_ms);

/** The DcGateway this module owns. Telemetry and tick use it. */
DcGateway *dc_ws_gateway(void);

/** The DcAckq this module owns. The ack task pops from it. */
DcAckq *dc_ws_ackq(void);

/** The DcIdentify this module owns. Telemetry reads the budget from it. */
DcIdentify *dc_ws_identify(void);

#ifdef __cplusplus
}
#endif

#endif
