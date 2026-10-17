#ifndef DISCORD_CAPTCHA_H
#define DISCORD_CAPTCHA_H

#include <stdint.h>

#include "discord_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DC_CAPTCHA_ALPHABET "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
#define DC_CAPTCHA_MAX_LEN 6

typedef struct {
    uint32_t state;
} DcCaptchaRng;

void dc_rng_seed(DcCaptchaRng *rng, uint32_t seed);
uint32_t dc_rng_next(DcCaptchaRng *rng);
void dc_captcha_generate(DcCaptchaRng *rng, char *out, uint8_t out_cap, uint8_t *out_len);

/**
 * Arithmetic challenge ("seven + 3 = ?"), answer as decimal ASCII.
 *
 * LAYERED DEFENSE, not a vault: text math stops rigid raid bots (regex-proof
 * via mixed word/digit operands and rotating +/-,*), never a targeted AI
 * attacker. Real protection comes from the layers around it: 8 bounded
 * slots, 3-attempt burn, 5-minute TTL, one-shot consume, cooldowns.
 * Zero-alloc: question into qbuf (48 B fits every output), answer into
 * answer (16 B fits "100" and more). Returns answer length, 0 on bad args.
 */
uint8_t dc_captcha_generate_math(DcCaptchaRng *rng, char *qbuf, size_t qcap,
                                 char *answer, size_t acap);

#ifdef __cplusplus
}
#endif

#endif
