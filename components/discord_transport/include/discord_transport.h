#ifndef DISCORD_TRANSPORT_H
#define DISCORD_TRANSPORT_H

#include <esp_err.h>

#include "discord_feature.h"
#include "discord_jobq.h"
#include "discord_state.h"
#include "discord_ws.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Firmware wiring layer: the only place that knows every module
 * exists.
 *
 * ROLE: app_main calls discord_init, registers features through
 * discord_registry, then calls discord_start. Everything below
 * this line is module code that knows nothing about its
 * neighbours; everything above it is the application. The wiring
 * is deliberately boring: no policy lives here, only ownership,
 * ordering and lifecycle.
 *
 * LIFECYCLE: IDLE -> STARTING -> READY, with FATAL latching when
 * the gateway state machine reports DC_GW_FATAL (close 4004,
 * 4013 or 4014). discord_init wires the modules but starts
 * nothing. discord_start creates the five tasks, registers them
 * with the watchdog, prewarms TLS and starts the gateway.
 * discord_stop tears the tasks down and returns to IDLE; a new
 * start cycle then needs a fresh discord_init, because stop
 * deinitializes the REST client. The state is readable at any
 * time through discord_state.
 *
 * THE FIVE TASKS, all below CONFIG_LWIP_TCPIP_TASK_PRIO (18):
 *
 *   dc_ack      prio 16, stack 4096, polls every 10 ms. Pops
 *                 each DiscordAck from the gateway's DcAckq and
 *                 sends it with dc_rest_post_interaction, the
 *                 single-shot path with the 3 second deadline.
 *                 A failed ACK is dropped, never retried: an
 *                 answer that arrives after the deadline is an
 *                 interaction Discord never received.
 *   dc_gateway  prio 15, stack 6144, polls every 100 ms. Runs
 *                 dc_ws_gateway_tick, the whole time-driven
 *                 half of the gateway state machine.
 *   dc_rest     prio 12, stack 4096, polls every 10 ms. Drains
 *                 the DcJobQueue: each job is executed as one
 *                 dc_rest_request and then reported through
 *                 dc_jobq_note_executed, which is the call that
 *                 consumes the slot.
 *   dc_bot      prio 10, stack 4096, 1 Hz. Owns the DcBotState
 *                 the features reach through discord_bot_state,
 *                 pets the watchdog and logs compact telemetry
 *                 (free heap, ACK and job queue depths) every
 *                 60 s. Feature callbacks do not run here: they
 *                 run on the esp_websocket_client task through
 *                 the interaction handler, so this loop never
 *                 blocks.
 *   dc_monitor  prio 10, stack 3072, 1 Hz. Scans the watchdog
 *                 every second and reboots the device when it is
 *                 unhealthy, logging each overdue task first.
 *                 Every 60 s it logs the five stack high-water
 *                 marks and the heap telemetry. It also moves
 *                 the transport state to READY when the gateway
 *                 reaches DC_GW_READY, logging the milestone
 *                 heap numbers, and to FATAL on DC_GW_FATAL.
 *
 * esp_websocket_client runs its own sixth task; only the five
 * above are registered with the watchdog. DC_WD_TASK_SLOTS is 8,
 * leaving three slots of headroom before registration would
 * fail at boot.
 *
 * WATCHDOG: tolerance is 2 missed periods, the smallest
 * multiplier that still absorbs one bad scan, because a trip
 * also needs two consecutive overdue scans at the monitor's 1 Hz.
 * Each task's registered period is not its poll interval but its
 * worst-case legitimate silence: dc_ack covers the ACK hold plus
 * the ACK round trip (DC_REST_ACK_MAX_HOLD_MS +
 * DC_REST_ACK_TIMEOUT_MS), dc_gateway covers the transport's 2 s
 * send timeout, dc_rest covers one full guarded request with 5xx
 * backoff (DC_REST_TIMEOUT_MS + DC_REST_ACK_TIMEOUT_MS for the
 * mutex the ACK path can hold + the inter-attempt backoff), and
 * the 1 Hz tasks register their own period. A wedged task is
 * therefore detected within 2 x period + 1 s while no honest
 * blocking call can trip the reboot. A backwards SNTP step reads
 * as now < last_seen, which the scan skips, so clock steps
 * cannot false-trip either.
 *
 * Registration failure (a negative slot) is a boot-time error:
 * discord_start fails, deletes the tasks it created and leaves
 * the state IDLE so the caller can retry. It is never a runtime
 * -1 that a task would have to survive.
 *
 * THREADING: the interaction handler runs on the
 * esp_websocket_client task and must return quickly, which the
 * registry router honours by only copying values and calling the
 * owning feature's callback. The ACK queue is single producer
 * (the transport, on the client task) and single consumer (the
 * ACK task), with the producer's vTaskDelay(0) yield as the
 * happens-before edge, per the SPSC contract in discord_ackq.h.
 * The REST client serializes its own limiter and redirect guard
 * under one mutex, so the ACK and REST tasks share it without
 * any lock here. The registry and the bot state carry no locks:
 * the registry is written once, before discord_start, and read
 * only afterwards, and the bot state is owned by the framework's
 * single-writer rule that every feature follows.
 *
 * CLOCK: two domains, on purpose. Every now_ms this module reads
 * for the watchdog, the gateway tick and the telemetry is epoch
 * milliseconds, time(NULL) * 1000, the domain the gateway state
 * machine and the IDENTIFY budget require, which is why
 * discord_start never runs before SNTP has synced. The rate
 * limiter is seeded with the boot-relative millisecond clock
 * instead, because every limiter call inside the REST client
 * reads that clock and the limiter's contract requires all
 * callers and the seed to share one origin.
 *
 * INTENTS: dc_ws_set_intents is wired in discord_init and
 * re-applied in discord_start, after features have been
 * registered, so the mask IDENTIFY carries is the union of every
 * registered feature's requirement. Registering between the two
 * calls is the supported flow; discord_registry returns the
 * module-static registry for exactly that window.
 *
 * STOP SHAPE: discord_stop deletes the five tasks from the
 * caller's context rather than using a stop flag. Deletion is
 * synchronous, which is what the REST client's contract needs:
 * dc_rest_deinit may only run once every task that can call it
 * is gone, and only deletion guarantees that without waiting out
 * a task that could be mid round trip for tens of seconds. The
 * cost, documented rather than hidden: if a task is deleted
 * during an HTTP round trip, the REST client's static mutex is
 * left held, so a later discord_init in the same boot would
 * deadlock on its first request. Stop is therefore terminal in
 * the current firmware; a restart-without-reboot flow needs the
 * mutex re-created before it can be supported.
 *
 * SECRETS: the token is read from NVS once into module-static
 * storage that stays populated for the process lifetime, and no
 * path in this module prints, logs or redacts it. The REST
 * client logs a 401 as a token fingerprint only.
 */

/** Transport lifecycle, mirrored from the gateway state machine. */
typedef enum {
    DC_TR_IDLE = 0,
    DC_TR_STARTING,
    DC_TR_READY,
    DC_TR_FATAL
} DcTransportState;

/**
 * Wires the registry, the rate limiter, the watchdog, the REST
 * client and the WebSocket transport.
 *
 * Reads the bot token from NVS (DC_NVS_NAMESPACE, "bot_token")
 * and keeps it in module-static storage for the process
 * lifetime. Idempotent: a second call returns ESP_OK. Returns
 * the failing esp_err_t when the token is unreadable or the
 * REST client rejects it, with the reason already logged.
 * Features are registered after this call, through
 * discord_registry, and before discord_start.
 */
esp_err_t discord_init(void);

/**
 * Creates the five tasks, registers them with the watchdog,
 * prewarms TLS and starts the gateway.
 *
 * A watchdog registration failure, a 401 from the prewarm or a
 * failed dc_ws_start all fail the call: the tasks are deleted,
 * the state stays IDLE and the caller may retry. The prewarm
 * 401 is fatal on purpose: starting the gateway with a dead
 * token would spend an IDENTIFY against a token that cannot
 * work, and the REST module has already logged the fingerprint.
 * Idempotent while STARTING or READY.
 */
esp_err_t discord_start(void);

/**
 * Stops the gateway, deletes the five tasks and deinitializes
 * the REST client, returning the state to IDLE.
 *
 * See the STOP SHAPE note above: deletion is synchronous and
 * terminal. Idempotent when already IDLE.
 */
void discord_stop(void);

/** The current lifecycle state, readable from any task. */
DcTransportState discord_state(void);

/**
 * The module-static feature registry, for registering features
 * between discord_init and discord_start.
 *
 * The registry borrows the DiscordFeature pointers it is given:
 * a registered feature must outlive the registry, the same
 * ownership ruling discord_feature.h states for
 * dc_registry_find_command. The pointer stays valid for the
 * process lifetime; there is no unregister.
 */
DiscordFeatureRegistry *discord_registry(void);

/**
 * The module-static bot state, so features reach tickets,
 * captchas, spam tracking and ranks without touching statics.
 *
 * Valid after discord_init. The dc_bot task owns the state's
 * lifecycle; features read and write it under the framework's
 * single-writer rule, the same contract discord_state.h's
 * accessors carry.
 */
DcBotState *discord_bot_state(void);

/**
 * Queues one outbound action for the rest task (FIFO, DROP_OLDEST at 16).
 *
 * The only way for a feature to act (mute, role, message, delete): no
 * feature touches HTTP directly, so all outbound traffic stays serialized
 * under the REST mutex. Validates kind + payload_len; rejects anything
 * else. A DcJob carries IDs and a 112-byte payload only — the bot token
 * cannot fit, so queued work structurally cannot leak it.
 */
bool discord_job_push(const DcJob *job);

/**
 * Applies an activity: pushed live (OP3 presence update) when the session
 * is READY — discord.js setActivity parity — and always staged so every
 * fresh IDENTIFY replays it. `live` (optional) reports which happened.
 * Never forces a reconnect — that would spend IDENTIFY quota.
 */
bool discord_set_activity(uint8_t type, const char *name, const char *url,
                          bool *live);

/**
 * Outbound queue load snapshot for /stats and ops.
 *
 * depth = current jobs waiting, peak = max depth since boot,
 * dropped = jobs shed under overload (never silent: every shed increments
 * it). Writers are features, the reader is the rest task; all three are
 * plain loads, no locks (same discipline as the queue itself).
 */
void discord_job_stats(uint32_t *depth, uint32_t *peak, uint64_t *dropped);

/**
 * Registers the app-level guild member handler (join/leave).
 *
 * Called between discord_init and discord_start, like feature
 * registration. The framework owns the socket; the app owns the meaning.
 */
void discord_on_member(DcWsMemberHandler handler);

/**
 * Registers the dynamic modal builder (per-invocation modal documents).
 *
 * Called between discord_init and discord_start. See DcWsModalBuilder:
 * the builder receives the parsed slash interaction (option values in
 * values[]) and writes one {"type":9,...} document into the scratch
 * buffer. NULL (or a NULL return per command) falls back to the static
 * modal_for lookup. May be NULL to disable.
 */
void discord_set_modal_builder(DcWsModalBuilder fn);

/**
 * Edits the deferred interaction response with the given text content,
 * wrapped in a Components-V2 container (one text display).
 *
 * Default accent: blurple (DC_V2_ACCENT_DEFAULT, discord_v2.h). Sends
 * PATCH /webhooks/{application_id}/{token}/messages/@original.  Call
 * this from an on_interaction handler after the framework has sent a
 * deferred ACK (type 5).  The handler runs on the esp_websocket_client
 * task; the underlying dc_rest_request holds the REST mutex for one
 * round trip (~100 ms warm, ~3 s cold).  Returns ESP_OK on a 2xx
 * response, ESP_FAIL otherwise.  A NULL or empty content is refused.
 *
 * Modal submits (type 5) own no @original to edit: the same content
 * goes out as a followup POST /webhooks/{application_id}/{token}.
 */
esp_err_t discord_interaction_reply(const DiscordInteraction *it,
                                    const char *content);

/**
 * discord_interaction_reply with an explicit accent color (raw 24-bit
 * RGB): the bot's palette decides the colour, the framework only
 * carries it.
 */
esp_err_t discord_interaction_reply_accent(const DiscordInteraction *it,
                                           const char *content,
                                           uint32_t accent);

/**
 * Posts a prebuilt JSON document to a channel.
 *
 * One-shot rich sends (the announce container) that cannot fit the
 * 112 B job payload. Serialized under the REST mutex like everything
 * else; the caller blocks one round trip. Human-paced use only.
 */
esp_err_t discord_channel_send(uint64_t channel_id, const char *json_body);

#ifdef __cplusplus
}
#endif

#endif
