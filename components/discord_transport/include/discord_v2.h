#ifndef DISCORD_V2_H
#define DISCORD_V2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* discord_v2.h — Components-V2 message bodies, one place.
 *
 * A v2 message carries only `components` plus the IS_COMPONENTS_V2 flag;
 * plain "content" renders legacy. The bot's whole look is one container
 * (type 17) with an accent color wrapping one text display (type 10).
 * This module owns that document once, so the interaction reply path
 * (discord_reply.c) and the REST job path (discord_transport.c) cannot
 * drift apart, and colours stay a caller parameter — the framework
 * defaults to blurple, bots name their palette.
 *
 * HOST-SAFE: no ESP-IDF header, own status codes, so the host suite
 * compiles it against discord_json.c alone (same rule as
 * discord_identify.h).
 */

/** IS_COMPONENTS_V2 message flag. */
#define DC_V2_FLAG 32768u

/** Blurple, the default accent (Node announce.js:58, poll.js:55). */
#define DC_V2_ACCENT_DEFAULT 0x5865F2u

/* Worst-case bytes a document spends outside the escaped content, NUL
 * included: 94 B of template + 10-digit flags + 10-digit accent. A
 * buffer with cap >= this can never come back DC_V2_ERR_SIZE. */
#define DC_V2_TEXT_MAX_OVERHEAD 114u

typedef enum {
    DC_V2_OK = 0,
    DC_V2_ERR_ARG = -1,  /**< NULL buffer or content. */
    DC_V2_ERR_SIZE = -2  /**< cap too small, even for empty content. */
} DcV2Status;

/**
 * Builds {"flags":F,"components":[{"type":17,"accent_color":A,
 * "components":[{"type":10,"content":"<escaped>"}]}]}.
 *
 * `content` need not be NUL-terminated: content_len is authoritative
 * (job payloads are length-delimited, not strings). When the escaped
 * document would overflow `cap`, the content is truncated as far as
 * fits — UTF-8-safe, never splitting a multi-byte sequence — and *kept
 * (optional) receives the raw byte count that made it; without
 * truncation *kept equals content_len. Accent is the raw 24-bit colour;
 * DC_V2_ACCENT_DEFAULT for the framework's blurple.
 */
DcV2Status dc_v2_text_body(char *buf, size_t cap, uint32_t flags,
                           uint32_t accent, const char *content,
                           size_t content_len, size_t *kept);

#ifdef __cplusplus
}
#endif

#endif
