/* discord_reply.c — deferred interaction followup (PATCH webhook).
 *
 * Split out of discord_transport.c (controller / sub-logic split).
 * The transport stays the controller (tasks, registry, routes); this
 * file owns the single followup path every bot command feature uses
 * after the framework's deferred ACK (type 5).
 *
 * Three bugs fixed here, all load-bearing for "The application did not respond":
 *   1. Body 384 B was smaller than every real reply (rank 528, profile
 *      576, leaderboard 768): snprintf failed with ESP_ERR_INVALID_ARG
 *      before any HTTP, so the deferred "thinking..." was never edited.
 *      Body is now 1024 B; oversized content is truncated WITH a log,
 *      never silently dropped.
 *   2. Body was built with raw snprintf "{\"content\":\"%s\"}": every
 *      multi-line reply (all of /rank, /stats, /setup, ...) produced
 *      invalid JSON (literal 0x0A in a string) and Discord answered 400.
 *      Body is now built with the zero-alloc DcJson writer, which escapes
 *      (" \\ \n \r \t \b \f \u00XX) per discord_json.c.
 *   3. The handler runs on the esp_websocket_client task and PATCHed
 *      while the ACK task concurrently POSTed the deferred ACK through
 *      the same REST mutex: the followup could win the race and hit
 *      Discord before the ACK (404 unknown interaction) while delaying
 *      the ACK past its 3 s deadline. The reply now waits for the ACK
 *      queue to drain (bounded 2000 ms, polled WITHOUT the lock) before
 *      PATCHing, so ACK-then-edit order is guaranteed without deadlock.
 *
 * Body shape (0.17.0): one Components-V2 container (type 17) with an
 * accent color around one text display, built by dc_v2_text_body —
 * default blurple, or the caller's palette via the _accent variant.
 *
 * MAX LINES: 700 (AGENT.md §1). Current: ~150.
 */

#include <stdio.h>
#include <string.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "bot_config.h"
#include "discord_ackq.h"
#include "discord_feature.h"
#include "discord_rest_client.h"
#include "discord_transport.h"
#include "discord_v2.h"
#include "discord_ws.h"

#define REPLY_TAG "dc_reply"

/* Largest followup we build: leaderboard (768) + JSON wrapper + escape
 * growth, rounded up. STATIC, not stacked: this runs on the task that hosts
 * the interaction handlers, which already carries the ETF decode (~1 KiB) and
 * a whole DiscordInteraction (~1.6 KiB) — 1.4 KiB more on that stack
 * overflowed it on /rank (panic log 2026-10-13). Single caller context (one
 * task, dc_bot, runs every on_interaction handler serially), so one shared
 * pair is race-free. Cost: +1408 B static. */
#define DC_REPLY_BODY_MAX 1024u

static char s_route[DC_REST_ROUTE_MAX];
static char s_body[DC_REPLY_BODY_MAX];

/* Upper bound waiting for the deferred ACK to leave before PATCHing.
 * Polled without the REST lock (depth read + vTaskDelay only), so the
 * ACK task holding the lock is never blocked by this wait. */
#define DC_REPLY_ACK_WAIT_MS 2000u
#define DC_REPLY_ACK_POLL_MS 10u

/* dc_v2_text_body (discord_v2.c) owns the template, the escaping and
 * the UTF-8-safe truncation; this file only logs what it reports. */
esp_err_t discord_interaction_reply_accent(const DiscordInteraction *it,
                                           const char *content,
                                           uint32_t accent)
{
    /* s_route/s_body: shared statics, see note above. Only dc_bot ever calls
     * here (dispatch_pump runs every on_interaction handler serially on it),
     * so no lock is needed and none is used. The WebSocket task no longer
     * reaches this path: it only queues the interaction. */
    char *route = s_route;
    char *body = s_body;
    size_t raw_len;
    size_t keep;
    int n;

    if (it == NULL || content == NULL || content[0] == '\0') {
        ESP_LOGW(REPLY_TAG, "reply refused: null/empty (it=%p content=%p)",
                 (const void *)it, (const void *)content);
        return ESP_ERR_INVALID_ARG;
    }
    if (it->application_id == 0u || it->token[0] == '\0') {
        ESP_LOGW(REPLY_TAG, "reply refused: missing app_id/token (id=%llu)",
                 (unsigned long long)it->id);
        return ESP_ERR_INVALID_ARG;
    }

    /* 3. ACK-then-edit ordering: the deferred ACK (type 5) was queued on
     * the ws task just before the handler ran; the ACK task sends it
     * through the same REST mutex this PATCH needs. Wait for the drain
     * WITHOUT the lock so the ACK is always first on the wire. */
    {
        uint32_t waited = 0u;

        while (dc_ackq_depth(dc_ws_ackq()) > 0u &&
               waited < DC_REPLY_ACK_WAIT_MS) {
            vTaskDelay(pdMS_TO_TICKS(DC_REPLY_ACK_POLL_MS));
            waited += DC_REPLY_ACK_POLL_MS;
        }
        if (dc_ackq_depth(dc_ws_ackq()) > 0u) {
            ESP_LOGW(REPLY_TAG,
                     "id %llu: ackq still %u deep after %u ms, PATCHing anyway",
                     (unsigned long long)it->id,
                     (unsigned)dc_ackq_depth(dc_ws_ackq()),
                     (unsigned)waited);
        } else if (waited > 0u) {
            ESP_LOGI(REPLY_TAG, "id %llu: ack flushed after %u ms",
                     (unsigned long long)it->id, (unsigned)waited);
        }
    }

    /* Modal submits (type 5) have no source message: their deferred
     * ACK (kind 6) owns no @original, so PATCHing it can only 404.
     * Confirmations there are followups: POST /webhooks/{app}/{token}
     * creates a new message. Deferred slash commands (type 2/3) keep
     * the PATCH-@original edit. */
    DcRestMethod method = DC_REST_PATCH;
    uint32_t flags = DC_V2_FLAG;
    if (it->type == 5u) {
        /* Ephemeral confirmation (Node parity): only the invoker sees
         * it, and it never pollutes channel history (a public "Nuke
         * started" would be the newest message — and the nuke's next
         * victim). */
        flags = DC_V2_FLAG | 64u;
        method = DC_REST_POST;
        n = snprintf(route, sizeof(s_route), "/webhooks/%llu/%s",
                     (unsigned long long)it->application_id, it->token);
    } else {
        n = snprintf(route, sizeof(s_route),
                     "/webhooks/%llu/%s/messages/@original",
                     (unsigned long long)it->application_id, it->token);
    }
    if (n < 0 || (size_t)n >= sizeof(s_route)) {
        ESP_LOGW(REPLY_TAG, "id %llu: route overflow (token len %u)",
                 (unsigned long long)it->id,
                 (unsigned)strlen(it->token));
        return ESP_ERR_INVALID_ARG;
    }

    /* Body as a Components-V2 container (type 17, accent color) around
     * one text display (type 10): the embedded look Discord renders for
     * v2 messages — plain "content" stays legacy. dc_v2_text_body
     * escapes and truncates UTF-8-safe when the content overflows. */
    raw_len = strlen(content);
    {
        DcV2Status st = dc_v2_text_body(body, sizeof(s_body), flags,
                                        accent, content, raw_len, &keep);

        if (st != DC_V2_OK) {
            ESP_LOGW(REPLY_TAG, "id %llu: body build failed (%d, %u B)",
                     (unsigned long long)it->id, (int)st, (unsigned)raw_len);
            return (st == DC_V2_ERR_SIZE) ? ESP_FAIL : ESP_ERR_INVALID_ARG;
        }
        if (keep < raw_len) {
            ESP_LOGW(REPLY_TAG, "id %llu: content %u B truncated to %u B",
                     (unsigned long long)it->id, (unsigned)raw_len,
                     (unsigned)keep);
        }
    }

    {
        esp_err_t err = dc_rest_request(method, route, body, NULL,
                                        0u, NULL);

        ESP_LOGI(REPLY_TAG, "id %llu: %s %s (status %u, body %u B)",
                 (unsigned long long)it->id,
                 method == DC_REST_POST ? "FOLLOWUP" : "PATCH",
                 err == ESP_OK ? "ok" : "FAILED",
                 (unsigned)dc_rest_last_status(),
                 (unsigned)strlen(body));
        return err;
    }
}

/* Default accent: the framework blurple (discord_v2.h). The coloured
 * variant exists for callers with a custom palette. */
esp_err_t discord_interaction_reply(const DiscordInteraction *it,
                                    const char *content)
{
    return discord_interaction_reply_accent(it, content,
                                            DC_V2_ACCENT_DEFAULT);
}

/* Direct channel post for one-shot rich documents (announce container).
 *
 * The 48 B job queue cannot carry a ~600 B Components-V2 document, so
 * the submit handler builds it into its own static buffer and posts it
 * here: same REST mutex as everything else (serialized, rate-gated),
 * no new task, caller blocks one round trip like a reply. Human-paced
 * dev commands only — bulk traffic stays on the job queue. */
esp_err_t discord_channel_send(uint64_t channel_id, const char *json_body)
{
    char route[96];
    int n;

    if (channel_id == 0u || json_body == NULL || json_body[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    n = snprintf(route, sizeof(route), "/channels/%llu/messages",
                 (unsigned long long)channel_id);
    if (n < 0 || (size_t)n >= sizeof(route)) {
        return ESP_ERR_INVALID_ARG;
    }
    {
        esp_err_t err = dc_rest_request(DC_REST_POST, route, json_body,
                                        NULL, 0u, NULL);

        ESP_LOGI(REPLY_TAG, "channel %llu: POST %s (status %u, body %u B)",
                 (unsigned long long)channel_id,
                 err == ESP_OK ? "ok" : "FAILED",
                 (unsigned)dc_rest_last_status(),
                 (unsigned)strlen(json_body));
        return err;
    }
}
