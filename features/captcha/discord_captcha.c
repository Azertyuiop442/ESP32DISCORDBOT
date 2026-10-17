#include "discord_captcha.h"

#include <stdio.h>
#include <string.h>

#define DC_ALPHABET_LEN 32

static uint32_t xorshift32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

void dc_rng_seed(DcCaptchaRng *rng, uint32_t seed)
{
    rng->state = seed ? seed : 0x2545F491u;
}

uint32_t dc_rng_next(DcCaptchaRng *rng)
{
    return xorshift32(&rng->state);
}

void dc_captcha_generate(DcCaptchaRng *rng, char *out, uint8_t out_cap, uint8_t *out_len)
{
    uint8_t len = DC_CAPTCHA_MAX_LEN;
    if (out_cap < len + 1) {
        len = (uint8_t)(out_cap - 1);
    }
    for (uint8_t i = 0; i < len; i++) {
        out[i] = DC_CAPTCHA_ALPHABET[dc_rng_next(rng) % DC_ALPHABET_LEN];
    }
    out[len] = '\0';
    if (out_len) {
        *out_len = len;
    }
}

static const char *const s_words[21] = {
    "zero", "one", "two", "three", "four", "five", "six", "seven",
    "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen",
    "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty",
};

uint8_t dc_captcha_generate_math(DcCaptchaRng *rng, char *qbuf, size_t qcap,
                                 char *answer, size_t acap)
{
    uint32_t a, b, res;
    char op;
    char sa[8], sb[8];
    int n, m;

    if (rng == NULL || qbuf == NULL || qcap < 16 || answer == NULL ||
        acap < 5) {
        return 0;
    }
    switch (dc_rng_next(rng) % 3u) {
    case 0u: /* addition 1..10 + 1..10 */
        a = 1u + dc_rng_next(rng) % 10u;
        b = 1u + dc_rng_next(rng) % 10u;
        res = a + b;
        op = '+';
        break;
    case 1u: /* subtraction, non-negative by construction */
        a = 1u + dc_rng_next(rng) % 10u;
        b = 1u + dc_rng_next(rng) % 10u;
        if (b > a) {
            uint32_t t = a;
            a = b;
            b = t;
        }
        res = a - b;
        op = '-';
        break;
    default: /* multiplication 2..9 x 2..9 (never trivial N x 1) */
        a = 2u + dc_rng_next(rng) % 8u;
        b = 2u + dc_rng_next(rng) % 8u;
        res = a * b;
        op = 'x';
        break;
    }
    /* Mixed rendering: each operand randomly word or digit. A bot cannot
     * assume one regex shape; a human reads either effortlessly. */
    if (a <= 20 && (dc_rng_next(rng) & 1u)) {
        snprintf(sa, sizeof(sa), "%s", s_words[a]);
    } else {
        snprintf(sa, sizeof(sa), "%u", a);
    }
    if (b <= 20 && (dc_rng_next(rng) & 1u)) {
        snprintf(sb, sizeof(sb), "%s", s_words[b]);
    } else {
        snprintf(sb, sizeof(sb), "%u", b);
    }
    n = snprintf(qbuf, qcap, "%s %c %s = ?", sa, op, sb);
    m = snprintf(answer, acap, "%u", res);
    if (n < 0 || (size_t)n >= qcap || m < 0 || (size_t)m >= acap) {
        return 0;
    }
    return (uint8_t)m;
}
