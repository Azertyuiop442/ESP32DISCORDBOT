#ifndef DISCORD_SECURITY_H
#define DISCORD_SECURITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Transport rules.
 *
 * Discord is reached over WSS and HTTPS only. There is no legitimate reason
 * for this firmware to open a plaintext connection to any host, including a
 * self-hosted one: the bot token travels in the Authorization header on
 * every REST call.
 */
typedef enum {
    DC_SCHEME_NONE = 0,
    DC_SCHEME_HTTP,
    DC_SCHEME_HTTPS,
    DC_SCHEME_WS,
    DC_SCHEME_WSS
} DcScheme;

typedef enum {
    DC_SEC_OK = 0,
    DC_SEC_ERR_SCHEME = -1,
    DC_SEC_ERR_HOST = -2,
    DC_SEC_ERR_NO_TOKEN = -3,
    DC_SEC_ERR_TOKEN_FORMAT = -4,
    DC_SEC_ERR_REDIRECT = -5,
    DC_SEC_ERR_INSECURE_CONFIG = -6
} DcSecStatus;

DcScheme dc_scheme_parse(const char *url);
const char *dc_scheme_name(DcScheme s);

/**
 * True only for https:// and wss://. Anything else, including http:// and
 * ws://, is rejected: both schemes carry the bot token.
 */
bool dc_scheme_is_secure(const char *url);

/** True for hosts Discord owns. Anything else is not an API endpoint. */
bool dc_host_is_discord(const char *host);

/**
 * Rejects a bot token that could not possibly be a real one.
 *
 * A Discord bot token has three dot-separated parts: a base64 identifier, a
 * timestamp, and an HMAC. Catching the obviously wrong shapes at boot turns a
 * confusing "401 then disconnect" into an actionable error.
 */
DcSecStatus dc_token_looks_valid(const char *token, size_t len);

/**
 * The redirect rule.
 *
 * esp_http_client follows redirects by default and replays request headers,
 * including Authorization, to whatever host the Location header names. A
 * malicious or misconfigured endpoint could therefore harvest the token.
 *
 * The framework refuses redirects outright and reports them instead.
 */
typedef struct {
    uint32_t seen;
    uint32_t refused;
    bool     allow_any;
} DcRedirectGuard;

void dc_redirect_guard_init(DcRedirectGuard *g);
bool dc_redirect_decide(DcRedirectGuard *g, const char *location);
bool dc_redirect_guard_needed(void);

/** Header allowlist. Anything not listed must not be sent. */
bool dc_header_allowed(const char *name);
bool dc_header_is_secret(const char *name);

/**
 * Redact a secret for logging: keeps first 4 chars, masks the rest.
 *
 * The first 4 characters of a Discord token encode part of the application
 * ID. That is not the secret half, but it is not zero leakage either.
 */
void dc_redact(const char *secret, size_t len, char *out, size_t out_cap);

/**
 * Stable 8-character fingerprint of a token, for identifying it in logs
 * without printing it. Two identical tokens always fingerprint the same.
 */
void dc_token_fingerprint(const char *token, size_t len, char *out, size_t out_cap);

/**
 * NOT YET WIRED.
 *
 * Nothing in this repository calls this function. It exists as a placeholder
 * for the point where the ESP-IDF transport is written; until then there is
 * no logging path in the framework at all, and therefore no leak either.
 *
 * Do not describe this as an active control. It does not redact, does not
 * filter, and must not be relied upon. See docs/12_security_audit.md.
 */
bool dc_log_allowed(const char *msg, size_t len);

#ifdef __cplusplus
}
#endif

#endif