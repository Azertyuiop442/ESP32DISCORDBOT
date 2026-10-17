/* discord_v2.c — Components-V2 message bodies (see discord_v2.h).
 *
 * The template around the escaped content, for flags digits d1 and
 * accent digits d2, costs 93 + d1 + d2 bytes plus the NUL; that exact
 * number is the truncation budget's fixed part, computed at runtime so
 * the boundary is never one byte off. dc_json_valid() stays the safety
 * net behind it: the budget math decides truncation, the writer decides
 * what actually fits.
 *
 * MAX LINES: 700 (AGENT.md §1). Current: ~110.
 */
#include "discord_v2.h"

#include <string.h>

#include "discord_json.h"

static size_t u32_digits(uint32_t v)
{
    size_t d = 1u;

    while (v >= 10u) {
        v /= 10u;
        d++;
    }
    return d;
}

/* Truncates content[0..len) to at most esc_budget escaped bytes, never
 * splitting a UTF-8 sequence; returns the kept raw length. The cost
 * model mirrors dc_json_escape_len / put_escaped (discord_json.c). */
static size_t truncate_utf8(const char *s, size_t len, size_t esc_budget)
{
    size_t kept = 0u;
    size_t esc = 0u;

    while (kept < len) {
        unsigned char c = (unsigned char)s[kept];
        size_t cost;

        if (c == '"' || c == '\\' || c == '\n' || c == '\r' ||
            c == '\t' || c == '\b' || c == '\f') {
            cost = 2u;
        } else if (c < 0x20u) {
            cost = 6u;
        } else {
            cost = 1u;
        }
        if (esc + cost > esc_budget) {
            break;
        }
        esc += cost;
        kept++;
    }
    /* Back off over UTF-8 continuation bytes (10xxxxxx) only when the
     * cut really happened at kept < len. */
    while (kept > 0u && kept < len &&
           ((unsigned char)s[kept] & 0xC0u) == 0x80u) {
        kept--;
    }
    return kept;
}

DcV2Status dc_v2_text_body(char *buf, size_t cap, uint32_t flags,
                           uint32_t accent, const char *content,
                           size_t content_len, size_t *kept)
{
    /* 94 = fixed template (93) + NUL. */
    const size_t overhead = 94u + u32_digits(flags) + u32_digits(accent);
    DcJson w;
    size_t keep;

    if (buf == NULL || cap == 0u || content == NULL) {
        return DC_V2_ERR_ARG;
    }
    if (cap < overhead) {
        return DC_V2_ERR_SIZE;
    }

    keep = content_len;
    if (dc_json_escape_len(content, content_len) + overhead > cap) {
        keep = truncate_utf8(content, content_len, cap - overhead);
    }

    dc_json_init(&w, buf, cap);
    dc_json_obj_begin(&w);
    dc_json_u64(&w, "flags", (uint64_t)flags);
    dc_json_key(&w, "components");
    dc_json_arr_begin(&w);
    dc_json_obj_begin(&w);
    dc_json_u64(&w, "type", 17u);
    dc_json_u64(&w, "accent_color", (uint64_t)accent);
    dc_json_key(&w, "components");
    dc_json_arr_begin(&w);
    dc_json_obj_begin(&w);
    dc_json_u64(&w, "type", 10u);
    dc_json_strn(&w, "content", content, keep);
    dc_json_obj_end(&w);
    dc_json_arr_end(&w);
    dc_json_obj_end(&w);
    dc_json_arr_end(&w);
    dc_json_obj_end(&w);
    if (!dc_json_valid(&w)) {
        return DC_V2_ERR_SIZE;
    }
    if (kept != NULL) {
        *kept = keep;
    }
    return DC_V2_OK;
}
