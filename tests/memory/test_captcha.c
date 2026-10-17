#include <stdio.h>
#include <string.h>

#include "discord_captcha.h"

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

static int all_in_alphabet(const char *s)
{
    for (const char *p = s; *p; p++) {
        if (!strchr(DC_CAPTCHA_ALPHABET, *p)) {
            return 0;
        }
    }
    return 1;
}

static void test_generation(void)
{
    DcCaptchaRng rng;
    dc_rng_seed(&rng, 12345);

    char code[16];
    uint8_t len = 0;
    dc_captcha_generate(&rng, code, sizeof(code), &len);

    printf("     generated: %s (len=%u)\n", code, len);
    check(len == DC_CAPTCHA_MAX_LEN, "code has requested length");
    check(strlen(code) == len, "code is null terminated");
    check(all_in_alphabet(code), "code uses only unambiguous glyphs");
    check(!strchr(code, 'O') && !strchr(code, 'I') && !strchr(code, '0') &&
              !strchr(code, '1'),
          "no confusable characters (O/I/0/1)");
}

static void test_variety(void)
{
    DcCaptchaRng rng;
    dc_rng_seed(&rng, 999);

    char codes[32][16];
    int distinct = 0;
    for (int i = 0; i < 32; i++) {
        uint8_t len = 0;
        dc_captcha_generate(&rng, codes[i], sizeof(codes[i]), &len);
        int dup = 0;
        for (int j = 0; j < i; j++) {
            if (strcmp(codes[i], codes[j]) == 0) {
                dup = 1;
            }
        }
        if (!dup) {
            distinct++;
        }
    }
    check(distinct >= 30, "32 generated codes are nearly all distinct");
}

static void test_deterministic_seed(void)
{
    DcCaptchaRng a, b;
    dc_rng_seed(&a, 4242);
    dc_rng_seed(&b, 4242);

    char ca[16], cb[16];
    uint8_t la = 0, lb = 0;
    dc_captcha_generate(&a, ca, sizeof(ca), &la);
    dc_captcha_generate(&b, cb, sizeof(cb), &lb);
    check(strcmp(ca, cb) == 0, "same seed yields same code");
}

static void test_small_buffer(void)
{
    DcCaptchaRng rng;
    dc_rng_seed(&rng, 7);

    char small[4];
    uint8_t len = 0;
    dc_captcha_generate(&rng, small, sizeof(small), &len);
    check(len == 3, "respects small output buffer");
    check(strlen(small) == 3, "no overflow on small buffer");
}

static void test_full_flow(void)
{
    DcBotState state;
    memset(&state, 0, sizeof(state));

    DcCaptchaRng rng;
    dc_rng_seed(&rng, 20261003);

    char code[16];
    uint8_t len = 0;
    dc_captcha_generate(&rng, code, sizeof(code), &len);

    DcCaptcha *c = dc_state_open_captcha(&state, 777, code, len, 1000, 600000);
    check(c != NULL, "challenge stored");
    check(c->answer_len == len, "expected answer length stored");
    check(memcmp(c->answer, code, len) == 0, "expected answer stored verbatim");

    check(!dc_state_verify_captcha(&state, 777, "ZZZZZZ", 6, 3), "wrong answer refused");
    check(dc_state_verify_captcha(&state, 777, code, len, 3), "correct answer accepted");
    check(!dc_state_verify_captcha(&state, 777, code, len, 3), "cannot replay");
}

static void test_retry_budget(void)
{
    DcBotState state;
    memset(&state, 0, sizeof(state));

    DcCaptchaRng rng;
    dc_rng_seed(&rng, 555);
    char code[16];
    uint8_t len = 0;
    dc_captcha_generate(&rng, code, sizeof(code), &len);

    dc_state_open_captcha(&state, 888, code, len, 1000, 600000);
    check(!dc_state_verify_captcha(&state, 888, "AAAAAA", 6, 3), "attempt 1 wrong");
    check(!dc_state_verify_captcha(&state, 888, "BBBBBB", 6, 3), "attempt 2 wrong");
    check(dc_state_verify_captcha(&state, 888, code, len, 3),
          "correct answer still works after 2 typos");

    dc_state_open_captcha(&state, 889, code, len, 2000, 600000);
    check(!dc_state_verify_captcha(&state, 889, "AAAAAA", 6, 3), "attempt 1 wrong");
    check(!dc_state_verify_captcha(&state, 889, "BBBBBB", 6, 3), "attempt 2 wrong");
    check(!dc_state_verify_captcha(&state, 889, "CCCCCC", 6, 3), "attempt 3 wrong");
    check(!dc_state_verify_captcha(&state, 889, code, len, 3),
          "challenge burned after max attempts");
}

static void test_math(void)
{
    DcCaptchaRng rng;
    dc_rng_seed(&rng, 777);

    char q[48], a[16];
    uint8_t n = dc_captcha_generate_math(&rng, q, sizeof(q), a, sizeof(a));
    check(strcmp(q, "3 + eight = ?") == 0, "math golden question (seed 777)");
    check(strcmp(a, "11") == 0 && n == 2, "math golden answer (seed 777)");

    /* 200 samples: shape + range invariants, no crash, no overflow. */
    int saw_op[3] = { 0, 0, 0 };
    int saw_word = 0, saw_digit = 0;
    for (int i = 0; i < 200; i++) {
        char qq[48], aa[16];
        uint8_t m = dc_captcha_generate_math(&rng, qq, sizeof(qq), aa,
                                             sizeof(aa));
        int v = 0;
        check(m > 0 && m < sizeof(aa), "math answer fits");
        check(strlen(qq) < sizeof(qq) && strstr(qq, "= ?") != NULL,
              "math question shape");
        for (const char *p = aa; *p; p++) {
            if (*p < '0' || *p > '9') {
                check(0, "math answer digits only");
                break;
            }
            v = v * 10 + (*p - '0');
        }
        check(v >= 0 && v <= 100, "math answer in range");
        if (strchr(qq, '+')) saw_op[0] = 1;
        if (strchr(qq, '-')) saw_op[1] = 1;
        if (strchr(qq, 'x')) saw_op[2] = 1;
        if (strstr(qq, "seven") || strstr(qq, "eight") || strstr(qq, "two") ||
            strstr(qq, "five") || strstr(qq, "three")) {
            saw_word = 1;
        }
        if (strchr(qq, '3') || strchr(qq, '5') || strchr(qq, '8') ||
            strchr(qq, '9')) {
            saw_digit = 1;
        }
        (void)m;
    }
    check(saw_op[0] && saw_op[1] && saw_op[2], "math rotates + - x");
    check(saw_word && saw_digit, "math mixes word and digit operands");
    check(dc_captcha_generate_math(NULL, q, sizeof(q), a, sizeof(a)) == 0,
          "math NULL rng refused");
    check(dc_captcha_generate_math(&rng, q, 4, a, sizeof(a)) == 0,
          "math tiny question buffer refused");

    /* Math answer round-trips through the one-shot slot store. */
    {
        DcBotState state;
        DcCaptchaRng r2;
        char mq[48], ma[16];
        uint8_t ml;
        memset(&state, 0, sizeof(state));
        dc_rng_seed(&r2, 4242);
        ml = dc_captcha_generate_math(&r2, mq, sizeof(mq), ma, sizeof(ma));
        check(ml > 0, "math challenge generatable");
        check(dc_state_open_captcha(&state, 555, ma, ml, 1000, 300000) != NULL,
              "math challenge stored");
        check(dc_state_verify_captcha(&state, 555, ma, ml, 3),
              "math correct answer accepted");
        check(!dc_state_verify_captcha(&state, 555, ma, ml, 3),
              "math answer single-use");
    }
}

int main(void)
{
    test_generation();
    test_variety();
    test_deterministic_seed();
    test_small_buffer();
    test_full_flow();
    test_retry_budget();
    test_math();

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
