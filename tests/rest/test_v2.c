#include <stdio.h>
#include <string.h>

#include "discord_v2.h"

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

static void test_exact_document(void)
{
    char buf[256];
    size_t kept = 999u;
    DcV2Status st = dc_v2_text_body(buf, sizeof(buf), DC_V2_FLAG,
                                    DC_V2_ACCENT_DEFAULT, "hi", 2u, &kept);

    check(st == DC_V2_OK, "simple content builds");
    check(strcmp(buf,
        "{\"flags\":32768,\"components\":[{\"type\":17,"
        "\"accent_color\":5793266,\"components\":[{\"type\":10,"
        "\"content\":\"hi\"}]}]}") == 0,
        "document matches the v2 container shape exactly");
    check(kept == 2u, "kept equals content_len without truncation");
    check(strlen(buf) == 107u, "length matches the computed template");
}

static void test_escaping(void)
{
    char buf[256];
    const char *in = "a\"b\nc";

    check(dc_v2_text_body(buf, sizeof(buf), 32832u, 0x2ECC71u, in,
                          strlen(in), NULL) == DC_V2_OK,
          "content with quotes and newline builds");
    check(strstr(buf, "\"content\":\"a\\\"b\\nc\"") != NULL,
          "quotes and newlines escape");
    check(strstr(buf, "\"flags\":32832") != NULL,
          "ephemeral-flag value kept");
    check(strstr(buf, "\"accent_color\":3066993") != NULL,
          "green accent printed raw");
}

static void test_truncation_utf8(void)
{
    char buf[256];
    char in[200];
    size_t kept = 0u;

    /* 100 x 'é' (0xC3 0xA9): 200 raw bytes, 200 escaped bytes. */
    for (size_t i = 0u; i < 100u; i++) {
        in[2u * i] = '\xC3';
        in[2u * i + 1u] = '\xA9';
    }
    /* cap 255 -> overhead (32768 -> 5, blurple -> 7) = 106, budget 149:
     * the 149th kept byte would be a lone continuation byte, so the
     * builder must back off to 148. */
    check(dc_v2_text_body(buf, 255u, DC_V2_FLAG, DC_V2_ACCENT_DEFAULT,
                          in, sizeof(in), &kept) == DC_V2_OK,
          "overlong content builds by truncating");
    check(kept == 148u, "truncation backs off the split UTF-8 sequence");
    check(strlen(buf) == 253u, "truncated document uses the full budget");
    check(strstr(buf, "\xC3\xA9" "\"}]}]}") != NULL,
          "document closes on a complete multi-byte char");
}

static void test_edge_sizes(void)
{
    char buf[256];

    check(dc_v2_text_body(NULL, 10u, DC_V2_FLAG, 0u, "x", 1u, NULL)
              == DC_V2_ERR_ARG,
          "NULL buffer refused");
    check(dc_v2_text_body(buf, sizeof(buf), DC_V2_FLAG, 0u, NULL, 0u, NULL)
              == DC_V2_ERR_ARG,
          "NULL content refused");
    /* overhead = 94 + 5 + 7 = 106 exactly. */
    check(dc_v2_text_body(buf, 105u, DC_V2_FLAG, DC_V2_ACCENT_DEFAULT,
                          "x", 1u, NULL) == DC_V2_ERR_SIZE,
          "cap under the template refused");
    {
        size_t kept = 9u;
        check(dc_v2_text_body(buf, 106u, DC_V2_FLAG, DC_V2_ACCENT_DEFAULT,
                              "x", 1u, &kept) == DC_V2_OK,
              "cap at the template keeps zero content bytes");
        check(kept == 0u, "kept is zero at the template boundary");
        check(strlen(buf) == 105u, "document without content fits");
    }
    {
        size_t kept = 9u;
        check(dc_v2_text_body(buf, 107u, DC_V2_FLAG, DC_V2_ACCENT_DEFAULT,
                              "x", 1u, &kept) == DC_V2_OK,
              "one byte over the template keeps one content byte");
        check(kept == 1u, "kept counts the single byte");
    }
}

static void test_max_overhead(void)
{
    char buf[DC_V2_TEXT_MAX_OVERHEAD];

    check(dc_v2_text_body(buf, sizeof(buf), 4294967295u, 4294967295u,
                          "", 0u, NULL) == DC_V2_OK,
          "empty content fits the documented worst-case overhead");
}

static void test_length_delimited(void)
{
    /* Job payloads are length-delimited, not NUL-terminated: content
     * bytes after content_len must never leak into the document. */
    char buf[256];
    static const char in[] = "abcXYZ";

    check(dc_v2_text_body(buf, sizeof(buf), DC_V2_FLAG,
                          DC_V2_ACCENT_DEFAULT, in, 3u, NULL) == DC_V2_OK,
          "non-NUL-terminated slice builds");
    check(strstr(buf, "\"content\":\"abc\"") != NULL,
          "only content_len bytes are serialised");
    check(strstr(buf, "XYZ") == NULL, "trailing bytes stay out");
}

int main(void)
{
    test_exact_document();
    test_escaping();
    test_truncation_utf8();
    test_edge_sizes();
    test_max_overhead();
    test_length_delimited();
    if (failures != 0) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("all v2 body tests passed\n");
    return 0;
}
