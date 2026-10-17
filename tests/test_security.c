#include <stdio.h>
#include <string.h>

#include "discord_security.h"

static int failures = 0;

static void check(int cond, const char *name)
{
    if (!cond) {
        printf("FAIL %s\n", name);
        failures++;
    } else {
        printf("ok   %s\n", name);
    }
}

/**
 * Token-shaped strings are assembled at runtime.
 *
 * A literal that looks like a Discord token in a test file would be caught by
 * secret scanners, and rightly so. Building it from parts keeps the fixture
 * honest and the repository clean.
 */
static void build_token(char *out, size_t cap)
{
    snprintf(out, cap, "%s%s.%s%s.%s%s",
             "MTIzNDU2Nzg5MDEy", "MzQ1Njc4",
             "GhIj", "Kl",
             "aBcDeFgHiJkLmNoP", "qRsTuVwXyZ012");
}

static void build_bad_token(char *out, size_t cap)
{
    snprintf(out, cap, "%s%s.%s%s.%s!",
             "MTIzNDU2Nzg5MDEy", "MzQ1Njc4",
             "GhIj", "Kl",
             "aBcDeFgHiJkLmNoP");
}

static void test_scheme_parsing(void)
{
    check(dc_scheme_parse("https://discord.com/api") == DC_SCHEME_HTTPS, "https parsed");
    check(dc_scheme_parse("wss://gateway.discord.gg") == DC_SCHEME_WSS, "wss parsed");
    check(dc_scheme_parse("http://example.com") == DC_SCHEME_HTTP, "http parsed");
    check(dc_scheme_parse("ws://example.com") == DC_SCHEME_WS, "ws parsed");
    check(dc_scheme_parse("ftp://x") == DC_SCHEME_NONE, "unknown scheme");
    check(dc_scheme_parse(NULL) == DC_SCHEME_NONE, "NULL url");
}

static void test_secure_only(void)
{
    check(dc_scheme_is_secure("https://discord.com/api"), "https accepted");
    check(dc_scheme_is_secure("wss://gateway.discord.gg"), "wss accepted");

    check(!dc_scheme_is_secure("http://discord.com/api"), "http rejected");
    check(!dc_scheme_is_secure("ws://gateway.discord.gg"), "ws rejected");
    check(!dc_scheme_is_secure("http://my-subdomain.example.com"),
          "self-hosted http rejected too");
    check(!dc_scheme_is_secure(""), "empty rejected");
}

static void test_host_allowlist(void)
{
    check(dc_host_is_discord("discord.com"), "discord.com");
    check(dc_host_is_discord("discordapp.com"), "discordapp.com");
    check(dc_host_is_discord("gateway.discord.gg"), "gateway.discord.gg");
    check(dc_host_is_discord("cdn.discordapp.com"), "cdn.discordapp.com");

    check(!dc_host_is_discord("discord.com.evil.tld"), "suffix confusion rejected");
    check(!dc_host_is_discord("notdiscord.com"), "lookalike rejected");
    check(!dc_host_is_discord("my-subdomain.example.com"), "self-hosted rejected");
    check(!dc_host_is_discord(""), "empty host rejected");
    check(!dc_host_is_discord(NULL), "NULL host rejected");
}

static void test_redirect_guard(void)
{
    DcRedirectGuard g;
    dc_redirect_guard_init(&g);

    check(dc_redirect_decide(&g, "https://discord.com/api/v10/users"),
          "same-host https redirect allowed");
    check(dc_redirect_guard_needed(), "guard is mandatory");

    dc_redirect_guard_init(&g);
    check(!dc_redirect_decide(&g, "http://evil.tld/steal"), "http downgrade refused");
    check(!dc_redirect_decide(&g, "https://evil.tld/steal"), "foreign host refused");
    check(!dc_redirect_decide(&g, NULL), "empty location refused");
    check(g.refused == 3, "every refusal counted");
}

static void test_token_shape(void)
{
    char good[80];
    build_token(good, sizeof(good));
    check(dc_token_looks_valid(good, strlen(good)) == DC_SEC_OK, "well-formed accepted");

    check(dc_token_looks_valid(NULL, 0) == DC_SEC_ERR_NO_TOKEN, "NULL rejected");
    check(dc_token_looks_valid("", 0) == DC_SEC_ERR_NO_TOKEN, "empty rejected");

    check(dc_token_looks_valid("tooshort", 8) == DC_SEC_ERR_TOKEN_FORMAT, "no dots rejected");
    check(dc_token_looks_valid("aaaa.bbbbbb", 12) == DC_SEC_ERR_TOKEN_FORMAT,
          "missing third part rejected");

    char bad[80];
    build_bad_token(bad, sizeof(bad));
    check(dc_token_looks_valid(bad, strlen(bad)) == DC_SEC_ERR_TOKEN_FORMAT,
          "illegal character rejected");
}

static void test_token_extra_parts(void)
{
    /* Exactly three parts. A fourth segment means this is not a bot token,
     * and storing it guarantees a 401 on every connect. */
    char four[128];
    snprintf(four, sizeof(four), "MTIzNDU2Nzg5MDEyMzQ1Njc4.GhIjKl.%s.junk",
             "aBcDeFgHiJkLmNoPqRsTuVwXyZ012");
    check(dc_token_looks_valid(four, strlen(four)) == DC_SEC_ERR_TOKEN_FORMAT,
          "four-part token rejected");

    char five[160];
    snprintf(five, sizeof(five), "MTIzNDU2Nzg5MDEyMzQ1Njc4.GhIjKl.%s.a.b",
             "aBcDeFgHiJkLmNoPqRsTuVwXyZ012");
    check(dc_token_looks_valid(five, strlen(five)) == DC_SEC_ERR_TOKEN_FORMAT,
          "five-part token rejected");

    /* Part 1 is base64, so letters are legitimate. A token pasted from the
     * wrong bot is caught by Discord's 401, not here. */
    char alpha[128];
    snprintf(alpha, sizeof(alpha), "AAAAAAAAAAAAAAAAAAAA.GhIjKl.%s",
             "aBcDeFgHiJkLmNoPqRsTuVwXyZ012");
    check(dc_token_looks_valid(alpha, strlen(alpha)) == DC_SEC_OK,
          "base64 part 1 accepted");

    char spaces[128];
    snprintf(spaces, sizeof(spaces), "MTIzNDU2Nz g5MDEyMzQ1Njc4.GhIjKl.%s",
             "aBcDeFgHiJkLmNoPqRsTuVwXyZ012");
    check(dc_token_looks_valid(spaces, strlen(spaces)) == DC_SEC_ERR_TOKEN_FORMAT,
          "space in part 1 rejected");
}

static void test_redaction(void)
{
    char token[80];
    char out[80];
    build_token(token, sizeof(token));

    dc_redact(token, strlen(token), out, sizeof(out));
    printf("     redacted: %s\n", out);
    check(strncmp(out, "MTIz", 4) == 0, "first chars kept for identification");
    check(strchr(out, '*') != NULL, "remainder masked");
    check(strstr(out, "NDU2Nzg5") == NULL, "token body never appears in logs");
    check(strstr(out, "GhIjKl") == NULL, "second token part never appears");

    dc_redact("abc", 3, out, sizeof(out));
    check(strcmp(out, "abc") == 0, "short values are not masked");

    dc_redact("0123456789", 10, out, 8);
    check(strlen(out) < 8, "respects a small output buffer");
}

static void test_fingerprint(void)
{
    char token[80];
    build_token(token, sizeof(token));

    char a[16], b[16];
    dc_token_fingerprint(token, strlen(token), a, sizeof(a));
    dc_token_fingerprint(token, strlen(token), b, sizeof(b));
    check(strcmp(a, b) == 0, "fingerprint is stable");

    check(strlen(a) == 8, "fingerprint is 8 hex chars");
    check(strcmp(a, token) != 0, "fingerprint is not the token");

    char c[16];
    dc_token_fingerprint("different.token.value.aaaaaaaaaaaaaaaaaaaaaaa", 41, c, sizeof(c));
    check(strcmp(a, c) != 0, "different tokens fingerprint differently");

    dc_token_fingerprint(NULL, 0, a, sizeof(a));
    check(strcmp(a, "<absent>") == 0, "absent token is explicit");
}

static void test_headers(void)
{
    check(dc_header_allowed("Authorization"), "Authorization allowed");
    check(dc_header_allowed("Content-Type"), "Content-Type allowed");
    check(dc_header_allowed("Retry-After"), "Retry-After allowed");
    check(dc_header_allowed("X-RateLimit-Bucket"), "rate limit headers allowed");

    check(!dc_header_allowed("X-Injected-Header"), "unknown header refused");
    check(!dc_header_allowed("Cookie"), "Cookie not in the allowlist");

    check(dc_header_is_secret("Authorization"), "Authorization is secret");
    check(dc_header_is_secret("authorization"), "case insensitive");
    check(dc_header_is_secret("Cookie"), "Cookie is secret");
    check(!dc_header_is_secret("Content-Type"), "Content-Type is not secret");
}

int main(void)
{
    test_scheme_parsing();
    test_secure_only();
    test_host_allowlist();
    test_redirect_guard();
    test_token_shape();
    test_token_extra_parts();
    test_redaction();
    test_fingerprint();
    test_headers();

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}