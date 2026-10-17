#include <stdio.h>
#include <string.h>

#include "discord_json.h"

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

static bool eq(char *buf, const char *want)
{
    return strcmp(buf, want) == 0;
}

static void test_simple_object(void)
{
    char buf[128];
    DcJson w;
    dc_json_init(&w, buf, sizeof(buf));

    dc_json_obj_begin(&w);
    dc_json_str(&w, "content", "hello");
    dc_json_i64(&w, "id", 1234567890123456789LL);
    dc_json_bool(&w, "tts", false);
    dc_json_obj_end(&w);

    check(eq(buf, "{\"content\":\"hello\",\"id\":1234567890123456789,\"tts\":false}"),
          "flat object serialises exactly");
    check(dc_json_valid(&w), "writer reports valid");
    check(dc_json_len(&w) == strlen(buf), "length matches buffer");
}

static void test_nested(void)
{
    char buf[128];
    DcJson w;
    dc_json_init(&w, buf, sizeof(buf));

    dc_json_obj_begin(&w);
    dc_json_str(&w, "content", "hi");
    dc_json_key(&w, "tags");
    dc_json_arr_begin(&w);
    dc_json_arr_str(&w, "a");
    dc_json_arr_str(&w, "b");
    dc_json_arr_end(&w);
    dc_json_obj_end(&w);

    check(eq(buf, "{\"content\":\"hi\",\"tags\":[\"a\",\"b\"]}"),
          "nested array inside object");
}

static void test_escaping(void)
{
    char buf[128];
    DcJson w;
    dc_json_init(&w, buf, sizeof(buf));

    dc_json_obj_begin(&w);
    dc_json_str(&w, "content", "a\"b\\c\nd\te");
    dc_json_obj_end(&w);

    check(eq(buf, "{\"content\":\"a\\\"b\\\\c\\nd\\te\"}"), "special chars escaped");
}

static void test_control_chars(void)
{
    char buf[128];
    DcJson w;
    dc_json_init(&w, buf, sizeof(buf));

    char raw[5] = { 'a', 0x01, 'b', 0x1F, 0 };
    dc_json_obj_begin(&w);
    dc_json_str(&w, "content", raw);
    dc_json_obj_end(&w);

    check(strstr(buf, "\\u0001") != NULL, "control char 0x01 -> \\u0001");
    check(strstr(buf, "\\u001f") != NULL, "control char 0x1F -> \\u001f");
}

static void test_injection_attempt(void)
{
    char buf[160];
    DcJson w;
    dc_json_init(&w, buf, sizeof(buf));

    dc_json_obj_begin(&w);
    dc_json_str(&w, "content", "\",\"admin\":true,\"x\":\"");
    dc_json_obj_end(&w);

    printf("     %s\n", buf);
    check(strstr(buf, "admin") != NULL, "payload text is present but escaped");
    check(strstr(buf, "\",\"admin\"") == NULL, "cannot break out of the JSON string");
}

static void test_escape_len_matches(void)
{
    const char *s = "a\"b\\c\nd\te\x01";
    size_t len = strlen(s);

    char buf[128];
    DcJson w;
    dc_json_init(&w, buf, sizeof(buf));
    dc_json_obj_begin(&w);
    dc_json_str(&w, "k", s);
    dc_json_obj_end(&w);

    size_t quoted = dc_json_escape_len(s, len) + 2;
    size_t overhead = strlen("{\"k\":\"\"}") - 2;
    check(dc_json_len(&w) == quoted + overhead, "escape_len predicts actual size");
}

static void test_overflow_is_error(void)
{
    char buf[16];
    DcJson w;
    dc_json_init(&w, buf, sizeof(buf));

    dc_json_obj_begin(&w);
    DcJsonStatus st = dc_json_str(&w, "content", "0123456789abcdefghijklmnop");
    check(st == DC_JSON_ERR_NOMEM, "overflow returns NOMEM, not silent truncation");
    check(w.overflowed, "overflow flag set");
    check(!dc_json_valid(&w), "overflowed document is not valid");
    check(strlen(buf) < sizeof(buf), "never writes past the buffer");
}

static void test_tight_overflow(void)
{
    char buf[8];
    DcJson w;
    dc_json_init(&w, buf, sizeof(buf));

    dc_json_obj_begin(&w);
    dc_json_str(&w, "k", "v");
    dc_json_obj_end(&w);

    check(!dc_json_valid(&w), "exact-fit document still detected");
}

static void test_exact_fit(void)
{
    char buf[16];
    DcJson w;
    dc_json_init(&w, buf, sizeof(buf));

    dc_json_obj_begin(&w);
    dc_json_str(&w, "k", "v");
    dc_json_obj_end(&w);

    check(eq(buf, "{\"k\":\"v\"}"), "exact fit succeeds");
    check(dc_json_valid(&w), "exact fit is valid");
}

static void test_depth_limit(void)
{
    char buf[256];
    DcJson w;
    dc_json_init(&w, buf, sizeof(buf));

    DcJsonStatus st = DC_JSON_OK;
    for (int i = 0; i < 20; i++) {
        st = dc_json_obj_begin(&w);
        if (st != DC_JSON_OK) {
            break;
        }
    }
    check(st == DC_JSON_ERR_DEPTH, "depth limit enforced");
    check(w.depth <= 8, "depth capped at 8");
}

static void test_negative_and_extremes(void)
{
    char buf[128];
    DcJson w;
    dc_json_init(&w, buf, sizeof(buf));

    dc_json_obj_begin(&w);
    dc_json_i64(&w, "min", INT64_MIN);
    dc_json_i64(&w, "max", INT64_MAX);
    dc_json_i64(&w, "zero", 0);
    dc_json_i64(&w, "neg", -1);
    dc_json_obj_end(&w);

    printf("     %s\n", buf);
    check(strstr(buf, "-9223372036854775808") != NULL, "INT64_MIN correct");
    check(strstr(buf, "9223372036854775807") != NULL, "INT64_MAX correct");
    check(strstr(buf, "\"zero\":0") != NULL, "zero correct");
    check(strstr(buf, "\"neg\":-1") != NULL, "negative correct");
}

static void test_unbalanced_end(void)
{
    char buf[32];
    DcJson w;
    dc_json_init(&w, buf, sizeof(buf));

    check(dc_json_obj_end(&w) == DC_JSON_ERR_TYPE, "closing an unopened object errors");
    check(dc_json_arr_end(&w) == DC_JSON_ERR_TYPE, "closing an unopened array errors");
}

static void test_escaped_key(void)
{
    char buf[64];
    DcJson w;
    dc_json_init(&w, buf, sizeof(buf));

    dc_json_obj_begin(&w);
    dc_json_str(&w, "ke\"y", "v");
    dc_json_obj_end(&w);

    check(eq(buf, "{\"ke\\\"y\":\"v\"}"), "keys are escaped too");
}

static void test_empty_string(void)
{
    char buf[32];
    DcJson w;
    dc_json_init(&w, buf, sizeof(buf));

    dc_json_obj_begin(&w);
    dc_json_str(&w, "content", "");
    dc_json_null(&w, "nothing");
    dc_json_obj_end(&w);

    check(eq(buf, "{\"content\":\"\",\"nothing\":null}"), "empty string and null");
}

int main(void)
{
    test_simple_object();
    test_nested();
    test_escaping();
    test_control_chars();
    test_injection_attempt();
    test_escape_len_matches();
    test_overflow_is_error();
    test_tight_overflow();
    test_exact_fit();
    test_depth_limit();
    test_negative_and_extremes();
    test_unbalanced_end();
    test_escaped_key();
    test_empty_string();

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}