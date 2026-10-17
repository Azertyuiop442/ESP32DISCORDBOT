#ifndef DISCORD_REST_CLIENT_H
#define DISCORD_REST_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#include "discord_ackq.h"
#include "discord_ratelimit.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * REST transport over esp_http_client, with the redirect guard enforced.
 *
 * ROLE: the only place in the firmware that opens an HTTP connection. It
 * sends interaction ACKs, the boot-time TLS pre-warm and every other REST
 * call, and it enforces the controls docs/12_security_audit.md recorded as
 * tested but unwired.
 *
 * REDIRECT GUARD, THE CONTROL THAT MATTERS MOST:
 *
 * Every esp_http_client_init() in this module sets disable_auto_redirect =
 * true. Without it esp_http_client follows a 3xx on its own and replays the
 * Authorization header, bot token included, to whatever host the Location
 * header names. With it, a 3xx is returned to this module, which reads
 * Location and consults dc_redirect_decide() before following anything.
 * Only a redirect the guard approves, https and a Discord-owned host, is
 * followed, and only on the generic request path; the interaction ACK path
 * never follows, because one extra round trip is time the 3 second deadline
 * does not have. Every request header is gated by dc_header_allowed(), and
 * a request is refused outright unless all four headers this module sends
 * are allowlisted, so a policy change can never narrow a request into an
 * unauthenticated one. The Authorization header is never logged: no call
 * site in this module prints it, the request URL, or the interaction
 * token, which is the only enforcement a secret needs when there is no
 * path to a log.
 *
 * RATE LIMITING AND THE BINDING OBLIGATION:
 *
 * Interaction ACKs go through dc_ratelimit_interaction(), never
 * dc_ratelimit_rest(), because interaction endpoints are exempt from the
 * 50 req/s global cap while still bound by the per-route bucket. The
 * limiter's own header states an obligation this module must enforce: a
 * global 429 MUST gate interaction ACKs, and the enforcement point is the
 * transport, because dc_ratelimit_interaction() deliberately ignores
 * global_blocked. dc_rest_post_interaction() therefore reads
 * dc_ratelimit_global_blocked() itself and holds every ACK back while a
 * global block is armed. A throttled ACK holds up to DC_REST_ACK_MAX_HOLD_MS
 * for its route bucket and is then refused: an ACK that waits out a long
 * bucket arrives after its deadline and is an interaction Discord never
 * received. A throttled generic request is refused outright and the caller
 * retries, which the limiter paces.
 *
 * RESPONSE MAPPING:
 *
 * 2xx succeeds. 401 is FATAL: it is logged with dc_token_fingerprint()
 * only, never the token, never retried, and dc_rest_last_status() exposes
 * the 401 to the transport state machine. 403 and 429 count against the
 * 10 000 per 10 minute invalid-request breaker through
 * dc_ratelimit_note_invalid(); a 429 additionally records its Retry-After,
 * or X-RateLimit-Reset-After in milliseconds, through dc_ratelimit_on_429(),
 * never a constant, and the recorded block paces every later request on
 * that route. 5xx is retried with exponential backoff, bounded by
 * DC_REST_5XX_ATTEMPTS, and the interaction ACK path, which cannot afford
 * a retry inside its deadline, is single-shot. When the breaker is tripped
 * every request is shed before it reaches the wire.
 *
 * THREADING:
 *
 * The ACK task and the REST task call this module concurrently. The rate
 * limiter carries no synchronization of its own, its contract is one owner,
 * so this module is that owner: every limiter call, every redirect-guard
 * decision and the response-header capture run under one static FreeRTOS
 * mutex, held only for the duration of a single HTTP round trip. Backoff
 * and ACK-hold sleeps happen outside the lock, so a sleeping request never
 * blocks the other path. dc_rest_deinit() must be called only after the
 * tasks that call this module have been deleted.
 *
 * CLOCK:
 *
 * now_ms comes from the FreeRTOS tick counter, a boot-relative millisecond
 * clock. The limiter's invalid-request window is therefore aligned to boot
 * rather than the Unix epoch, which its contract permits: every invalid
 * still lands in exactly one 10 minute window, only the window's origin
 * differs.
 *
 * CERTIFICATES:
 *
 * Server verification rides the global CA store (esp_tls_set_global_ca_store)
 * that the transport wiring installs once at boot. This module sets
 * cert_pem to NULL and attaches no bundle of its own, because a per-client
 * bundle costs 64-128 KB of flash against a 1500 KB slot, which spec
 * section 11.2 rejects in favour of the 1-2 KB pinned root.
 *
 * PRE-WARM:
 *
 * dc_rest_prewarm() makes one authenticated GET /users/@me through the fully
 * guarded generic path before READY, so DNS, TCP and the mbedTLS handshake
 * are paid at boot instead of under the first ACK's 3 second deadline.
 * Whether the next request reuses the warmed socket is ESP-IDF's transport
 * decision; the effect must be measured on hardware, and no 3 second claim
 * is made until then (spec section 4.1).
 */

#define DC_REST_API_BASE "https://discord.com/api/v10"

#define DC_REST_TOKEN_MAX       256u
#define DC_REST_ROUTE_MAX       384u
#define DC_REST_URL_MAX         512u
#define DC_REST_BODY_MAX        128u
#define DC_REST_TIMEOUT_MS      8000u
#define DC_REST_ACK_TIMEOUT_MS  2500u
#define DC_REST_5XX_ATTEMPTS    3u
#define DC_REST_5XX_BASE_MS     500u
#define DC_REST_5XX_MAX_MS      8000u
#define DC_REST_MAX_REDIRECTS   4u
#define DC_REST_ACK_MAX_HOLD_MS 2500u

/**
 * HTTP method for dc_rest_request(). Mapped one-to-one onto
 * http_method_t, so a value outside the enum is refused before it can
 * index the method table.
 */
typedef enum {
    DC_REST_GET = 0,
    DC_REST_POST,
    DC_REST_PUT,
    DC_REST_PATCH,
    DC_REST_DELETE
} DcRestMethod;

/**
 * Stores the provisioned bot token and the shared rate limiter.
 *
 * The token is shape-checked with dc_token_looks_valid() and copied into
 * module-static storage; the limiter pointer is borrowed, not owned.
 * Returns ESP_ERR_INVALID_ARG for a NULL argument, an empty or oversized
 * token, or a token that fails the shape check. Call once at boot, before
 * any request.
 */
esp_err_t dc_rest_init(const char *token, DcRateLimiter *rl);

/**
 * Clears the token from RAM and refuses further requests.
 *
 * Call only after the tasks that call this module have been deleted; the
 * limiter and the guard are not synchronized for concurrent deinit.
 */
void dc_rest_deinit(void);

/**
 * Boot-time TLS pre-warm: one authenticated GET /users/@me before READY.
 *
 * Goes through the fully guarded generic path, so a bad token surfaces as
 * a 401 here rather than under the first interaction. Best-effort: a
 * failure, typically Wi-Fi not yet up, is returned to the caller, which
 * decides whether to retry.
 */
esp_err_t dc_rest_prewarm(void);

/**
 * Periodically sends a lightweight GET /gateway over the persistent
 * HTTPS connection to keep the TLS session warm and prevent idle timeout.
 */
esp_err_t dc_rest_keepalive(void);

/**
 * Sends one interaction ACK to POST /interactions/{id}/{token}.
 *
 * Rate-limited through dc_ratelimit_interaction() and gated on
 * dc_ratelimit_global_blocked(), single-shot, with a tight
 * DC_REST_ACK_TIMEOUT_MS. A NULL json_body builds the body from the ack
 * with the dc_json writer ({"type":kind}, pong data for kind 4,
 * data.flags 64 when ack->ephemeral); a non-NULL json_body is sent as-is
 * and must be a complete JSON document. The response body is not
 * captured.
 */
esp_err_t dc_rest_post_interaction(const DiscordAck *ack,
                                       const char *json_body);

void dc_rest_warmup(void);

/**
 * Generic authenticated request to DC_REST_API_BASE + route.
 *
 * Rate-limited through dc_ratelimit_rest(), with redirect following,
 * 5xx backoff and response-body capture. When out is not NULL the body is
 * copied into it, at most out_cap bytes, and NUL-terminated when space
 * permits; *out_len receives the number of bytes stored. Returns ESP_OK
 * for 2xx, ESP_FAIL for every other outcome, ESP_ERR_INVALID_ARG for a
 * NULL or over-long route, an unknown method, or an out/out_cap mismatch.
 */
esp_err_t dc_rest_request(DcRestMethod method, const char *route,
                              const char *body, char *out, size_t out_cap,
                              size_t *out_len);

/**
 * Builds the JSON body of an interaction response, {"type":kind}, with
 * the zero-alloc dc_json writer.
 *
 * kind 4 carries the pong payload. kind 5 with ephemeral true carries
 * {"data":{"flags":64}}: the deferred-ephemeral form, whose placeholder
 * message is created invoker-only, a visibility every later edit at
 * @original inherits. Any other combination builds {"type":kind} alone —
 * notably kind 9, whose caller passes the full modal document instead.
 *
 * Returns false when out is NULL, cap is zero, or the document does not
 * fit; no partial document is left behind that dc_json_valid() would
 * accept. *len receives the document length.
 */
bool dc_rest_build_interaction_body(uint8_t kind, bool ephemeral, char *out,
                                        size_t cap, size_t *len);

/**
 * The status code of the most recent response, 0 after a transport error
 * or before any request. This is how the transport state machine learns
 * that a 401, the FATAL auth failure, occurred.
 */
uint16_t dc_rest_last_status(void);

/**
 * Static RAM owned by this module: the token copy, the redirect guard,
 * the mutex and the bookkeeping words. The per-request capture and URL
 * buffers live on the caller's stack.
 */
size_t dc_rest_footprint(void);

#ifdef __cplusplus
}
#endif

#endif
