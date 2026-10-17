#include "discord_security.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/* A Discord bot token: <id>.<timestamp>.<hmac>, three parts of 24, 6 and 27
 * characters. Exact lengths are not contractual, but the shape is stable. */
#define DC_TOKEN_PART1_MIN 20
#define DC_TOKEN_PART2_LEN 6
#define DC_TOKEN_PART3_MIN 27

static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

DcScheme dc_scheme_parse(const char *url)
{
    if (!url) {
        return DC_SCHEME_NONE;
    }
    if (starts_with(url, "https://")) {
        return DC_SCHEME_HTTPS;
    }
    if (starts_with(url, "wss://")) {
        return DC_SCHEME_WSS;
    }
    if (starts_with(url, "http://")) {
        return DC_SCHEME_HTTP;
    }
    if (starts_with(url, "ws://")) {
        return DC_SCHEME_WS;
    }
    return DC_SCHEME_NONE;
}

const char *dc_scheme_name(DcScheme s)
{
    switch (s) {
    case DC_SCHEME_HTTPS: return "https";
    case DC_SCHEME_WSS:   return "wss";
    case DC_SCHEME_HTTP:  return "http";
    case DC_SCHEME_WS:    return "ws";
    case DC_SCHEME_NONE:  break;
    }
    return "none";
}

bool dc_scheme_is_secure(const char *url)
{
    DcScheme s = dc_scheme_parse(url);
    return s == DC_SCHEME_HTTPS || s == DC_SCHEME_WSS;
}

static void host_from_url(const char *url, char *out, size_t cap)
{
    const char *p = url;
    const char *sep = strstr(url, "://");
    if (sep) {
        p = sep + 3;
    }

    const char *end = p;
    while (*end && *end != '/' && *end != ':' && *end != '?' && *end != '#') {
        end++;
    }
    size_t n = (size_t)(end - p);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

bool dc_host_is_discord(const char *host)
{
    if (!host || host[0] == '\0') {
        return false;
    }
    if (strcmp(host, "discord.com") == 0 || strcmp(host, "discordapp.com") == 0) {
        return true;
    }

    size_t len = strlen(host);
    static const char *suffixes[] = { ".discord.com", ".discordapp.com",
                                      ".discord.gg",  ".discordapp.gg" };
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        size_t sl = strlen(suffixes[i]);
        if (len > sl && strcmp(host + len - sl, suffixes[i]) == 0) {
            return true;
        }
    }
    return false;
}

static size_t span_of_part(const char *token, size_t len, size_t from, size_t *next)
{
    size_t i = from;
    while (i < len && token[i] != '.') {
        i++;
    }
    *next = (i < len) ? i + 1 : len;
    return i - from;
}

DcSecStatus dc_token_looks_valid(const char *token, size_t len)
{
    if (!token || len == 0) {
        return DC_SEC_ERR_NO_TOKEN;
    }

    size_t next = 0;
    size_t p1 = span_of_part(token, len, 0, &next);
    size_t p2 = span_of_part(token, len, next, &next);
    size_t p3 = span_of_part(token, len, next, &next);

    if (p1 < DC_TOKEN_PART1_MIN || p2 != DC_TOKEN_PART2_LEN ||
        p3 < DC_TOKEN_PART3_MIN) {
        return DC_SEC_ERR_TOKEN_FORMAT;
    }

    /* Exactly three parts. span_of_part stops at the third dot, so a fourth or
     * fifth segment would otherwise pass unnoticed and be stored as a token
     * that can never authenticate. */
    if (next < len) {
        return DC_SEC_ERR_TOKEN_FORMAT;
    }

    /* Part 1 is base64, not decimal digits, so only the charset is checked
     * here. Whether it is the right bot's token is Discord's answer, not ours:
     * that arrives as a 401, which is handled as FATAL. */
    for (size_t i = 0; i < len; i++) {
        char c = token[i];
        if (c == '.') {
            continue;
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return DC_SEC_ERR_TOKEN_FORMAT;
        }
    }
    return DC_SEC_OK;
}

void dc_redirect_guard_init(DcRedirectGuard *g)
{
    memset(g, 0, sizeof(*g));
    g->allow_any = false;
}

bool dc_redirect_decide(DcRedirectGuard *g, const char *location)
{
    g->seen++;
    if (!location || location[0] == '\0') {
        g->refused++;
        return false;
    }
    if (!dc_scheme_is_secure(location)) {
        g->refused++;
        return false;
    }
    if (!g->allow_any) {
        char host[64];
        host_from_url(location, host, sizeof(host));
        if (!dc_host_is_discord(host)) {
            g->refused++;
            return false;
        }
    }
    return true;
}

bool dc_redirect_guard_needed(void)
{
    return true;
}

bool dc_header_is_secret(const char *name)
{
    if (!name) {
        return false;
    }
    return strcasecmp(name, "Authorization") == 0 ||
           strcasecmp(name, "X-Signature-Ed25519") == 0 ||
           strcasecmp(name, "Cookie") == 0 ||
           strcasecmp(name, "Set-Cookie") == 0 ||
           strcasecmp(name, "X-Auth-Token") == 0;
}

bool dc_header_allowed(const char *name)
{
    if (!name) {
        return false;
    }
    static const char *allowed[] = {
        "Authorization", "Content-Type", "User-Agent", "Accept",
        "X-RateLimit-Remaining", "X-RateLimit-Limit", "X-RateLimit-Reset",
        "X-RateLimit-Reset-After", "X-RateLimit-Bucket", "X-RateLimit-Global",
        "X-RateLimit-Scope", "Retry-After", "Location"
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strcasecmp(name, allowed[i]) == 0) {
            return true;
        }
    }
    return false;
}

void dc_redact(const char *secret, size_t len, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) {
        return;
    }
    size_t keep = 4;
    size_t n = 0;

    for (size_t i = 0; i < keep && i < len && n + 1 < out_cap; i++) {
        out[n++] = secret[i];
    }
    for (size_t i = keep; i < len; i++) {
        if (n + 1 < out_cap) {
            out[n++] = '*';
        }
    }
    if (n + 1 < out_cap) {
        out[n++] = '\0';
    } else if (out_cap > 0) {
        out[out_cap - 1] = '\0';
    }
}

void dc_token_fingerprint(const char *token, size_t len, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) {
        return;
    }
    if (!token || len == 0) {
        snprintf(out, out_cap, "<absent>");
        return;
    }

    static const char hex[] = "0123456789abcdef";
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)token[i];
        h *= 16777619u;
    }

    size_t n = 0;
    for (int i = 7; i >= 0 && n + 1 < out_cap; i--) {
        out[n++] = hex[(h >> (i * 4)) & 0xF];
    }
    if (n + 1 < out_cap) {
        out[n++] = '\0';
    }
}

/* Placeholder only. See the header: nothing calls this yet, and it performs
 * no filtering. Kept so the transport layer has a single place to add it. */
bool dc_log_allowed(const char *msg, size_t len)
{
    (void)msg;
    (void)len;
    return true;
}