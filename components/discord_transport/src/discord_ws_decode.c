#include "discord_ws_decode.h"

#include <string.h>

#include "discord_interaction_parse.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DC_WS_TAG "discord_ws"
#define DC_WS_ETF_DEPTH 32
#define DC_WS_TOKEN_MAX 256
#define DC_WS_ACK_PONG 1u

static inline bool is_string_field(DiscordEtfFieldKind k)
{
    return k == DISCORD_ETF_FIELD_STRING || k == DISCORD_ETF_FIELD_BINARY;
}
#define DC_WS_ACK_MESSAGE 4u
#define DC_WS_ACK_DEFERRED_UPDATE 6u
#define DC_WS_TYPE_PING 1u
#define DC_WS_TYPE_COMMAND 2u
#define DC_WS_TYPE_COMPONENT 3u
#define DC_WS_TYPE_MODAL_SUBMIT 5u

typedef enum {
    DC_WS_KEY_OP = 0,
    DC_WS_KEY_S,
    DC_WS_KEY_T,
    DC_WS_KEY_D,
    DC_WS_KEY_ID,
    DC_WS_KEY_APPLICATION_ID,
    DC_WS_KEY_TYPE,
    DC_WS_KEY_TOKEN,
    DC_WS_KEY_CHANNEL_ID,
    DC_WS_KEY_GUILD_ID,
    DC_WS_KEY_MESSAGE,
    DC_WS_KEY_MEMBER,
    DC_WS_KEY_USER,
    DC_WS_KEY_DATA,
    DC_WS_KEY_NAME,
    DC_WS_KEY_CUSTOM_ID,
    DC_WS_KEY_OPTIONS,
    DC_WS_KEY_VALUE,
    DC_WS_KEY_SESSION_ID,
    DC_WS_KEY_RESUME_GATEWAY_URL,
    DC_WS_KEY_HEARTBEAT_INTERVAL,
    DC_WS_KEY_AUTHOR,
    DC_WS_KEY_CONTENT,
    DC_WS_KEY_BOT,
    DC_WS_KEY_COMPONENTS,
    DC_WS_KEY_ATTACHMENTS,
    DC_WS_KEY_CONTENT_TYPE,
    DC_WS_KEY_COUNT
} DcWsKey;

typedef struct {
    DiscordInteraction it;
    char token[DC_WS_TOKEN_MAX];
    size_t token_len;
    uint32_t value_index;
} DcWsInteraction;

/* Decode scratch: ~1.9 KiB (5x256 values + 256 token). Lives static,
 * not stacked — the ws task (8 KiB) already hosts the ETF walk, and
 * the single producer (decode on the ws task) makes sharing race-free.
 * Cost: +~1.9 KiB static. */
static DcWsInteraction s_parsed;

static DcAckq *sink_ackq;
static DcWsInteractionHandler sink_handler;
static DcWsMessageHandler sink_message;
static DcWsMemberHandler sink_member;
static const char *(*sink_modal_for)(const char *command, const char *sub);
static const char *(*sink_modal_build)(const DiscordInteraction *it,
                                       char *buf, size_t cap);
static bool (*sink_ephemeral_for)(const char *command);
/* Scratch ring for dynamic modals. Single producer (the ws task runs
 * enqueue_ack alone); the ACK slot borrows the pointer until the ACK
 * task sends it (~10 ms later). A ring of 2 is safe for up to 2 pending
 * dynamic modals — modal commands are human-paced, so depth stays 0 in
 * practice; a deeper queue logs a warning at the push site. Static
 * (flash) modals never touch this ring. Cost: +2304 B static. The slot
 * must hold the whole per-invocation form: announce renders ~1.0 KiB
 * (5 rows with placeholders), so the old 512 cap silently degraded it
 * to the typed-channel-ID fallback modal. */
#define DC_WS_DYN_MODAL_MAX 1152u
#define DC_WS_DYN_MODAL_RING 2u
static char s_dyn_modal[DC_WS_DYN_MODAL_RING][DC_WS_DYN_MODAL_MAX];
static uint32_t s_dyn_next;

static const char *const key_names[DC_WS_KEY_COUNT] = {
    "op", "s", "t", "d", "id", "application_id", "type", "token",
    "channel_id", "guild_id", "message", "member", "user", "data",
    "name", "custom_id", "options", "value", "session_id",
    "resume_gateway_url", "heartbeat_interval", "author", "content", "bot",
    "components", "attachments", "content_type"
};

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static int64_t be32_signed(const uint8_t *p)
{
    return (int64_t)(int32_t)be32(p);
}

static void copy_bounded(char *dst, size_t cap, const uint8_t *src,
                         size_t len)
{
    if (len > cap - 1u) {
        len = cap - 1u;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static size_t format_int64(char *buf, size_t cap, int64_t value)
{
    char tmp[24];
    uint64_t mag;
    bool neg = value < 0;
    size_t n = 0u;
    size_t i;

    mag = neg ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
    if (mag == 0u) {
        tmp[n++] = '0';
    }
    while (mag > 0u) {
        tmp[n++] = (char)('0' + (mag % 10u));
        mag /= 10u;
    }
    if (neg) {
        tmp[n++] = '-';
    }
    if (n > cap - 1u) {
        n = cap - 1u;
    }
    for (i = 0; i < n; i++) {
        buf[i] = tmp[n - 1u - i];
    }
    buf[n] = '\0';
    return n;
}

static uint8_t peek_tag(DiscordEtfReader *r)
{
    if (r->pos >= r->size) {
        return 0u;
    }
    return r->base[r->pos];
}

static bool is_map_tag(uint8_t tag)
{
    return tag == DISCORD_ETF_SMALL_MAP || tag == DISCORD_ETF_MAP ||
           tag == DISCORD_ETF_SMALL_TUPLE ||
           tag == DISCORD_ETF_LARGE_TUPLE ||
           tag == 131;
}

static bool is_container_tag(uint8_t tag)
{
    return is_map_tag(tag) || tag == DISCORD_ETF_LIST;
}

static DcWsKey match_key(const DiscordSlice *key)
{
    size_t i;

    for (i = 0; i < DC_WS_KEY_COUNT; i++) {
        if (discord_etf_slice_equals(key, key_names[i])) {
            return (DcWsKey)i;
        }
    }
    return DC_WS_KEY_COUNT;
}

static DiscordEtfStatus read_string(DiscordEtfReader *r, int width,
                                    DiscordEtfFieldKind kind,
                                    DiscordEtfField *out)
{
    size_t len = 0u;

    if (width == 1) {
        if (r->pos >= r->size) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        len = r->base[r->pos];
    } else if (width == 2) {
        if (r->pos + 2u > r->size) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        len = be16(r->base + r->pos);
    } else {
        if (r->pos + 4u > r->size) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        len = be32(r->base + r->pos);
    }
    r->pos += (size_t)width;
    if (width == 4 && len > 0xFFFFu) {
        return DISCORD_ETF_ERR_BAD_TAG;
    }
    if (len > r->size - r->pos) {
        return DISCORD_ETF_ERR_TRUNCATED;
    }
    out->kind = kind;
    out->slice.ptr = r->base + r->pos;
    out->slice.len = (uint16_t)len;
    r->pos += len;
    return DISCORD_ETF_OK;
}

static DiscordEtfStatus skip_fixed(DiscordEtfReader *r, size_t width,
                                       DiscordEtfField *out)
{
    if (r->pos + width > r->size) {
        return DISCORD_ETF_ERR_TRUNCATED;
    }
    r->pos += width;
    out->kind = DISCORD_ETF_FIELD_NIL;
    return DISCORD_ETF_OK;
}

static DiscordEtfStatus read_integer(DiscordEtfReader *r, size_t width,
                                         DiscordEtfField *out)
{
    if (width == 1u) {
        if (r->pos >= r->size) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        out->kind = DISCORD_ETF_FIELD_INT;
        out->integer = r->base[r->pos++];
        return DISCORD_ETF_OK;
    }
    if (r->pos + 4u > r->size) {
        return DISCORD_ETF_ERR_TRUNCATED;
    }
    out->kind = DISCORD_ETF_FIELD_INT;
    out->integer = be32_signed(r->base + r->pos);
    r->pos += 4u;
    return DISCORD_ETF_OK;
}

static DiscordEtfStatus read_scalar(DiscordEtfReader *r,
                                    DiscordEtfField *out)
{
    uint8_t tag;

    if (r->pos >= r->size) {
        return DISCORD_ETF_ERR_TRUNCATED;
    }
    tag = r->base[r->pos++];

    out->slice.ptr = NULL;
    out->slice.len = 0u;
    out->integer = 0;

    switch (tag) {
    case DISCORD_ETF_SMALL_INTEGER:
        return read_integer(r, 1u, out);
    case DISCORD_ETF_INTEGER:
        return read_integer(r, 4u, out);
    case DISCORD_ETF_FLOAT:
        return skip_fixed(r, 31u, out);
    case DISCORD_ETF_NEW_FLOAT:
        return skip_fixed(r, 8u, out);
    case DISCORD_ETF_ATOM:
        return read_string(r, 2, DISCORD_ETF_FIELD_STRING, out);
    case DISCORD_ETF_SMALL_ATOM_UTF8:
        return read_string(r, 1, DISCORD_ETF_FIELD_STRING, out);
    case DISCORD_ETF_ATOM_UTF8:
        return read_string(r, 2, DISCORD_ETF_FIELD_STRING, out);
    case DISCORD_ETF_NIL:
        out->kind = DISCORD_ETF_FIELD_NIL;
        return DISCORD_ETF_OK;
    case DISCORD_ETF_STRING:
        return read_string(r, 2, DISCORD_ETF_FIELD_STRING, out);
    case DISCORD_ETF_BINARY:
        return read_string(r, 4, DISCORD_ETF_FIELD_BINARY, out);
    case DISCORD_ETF_SMALL_BIG: {
        if (r->pos + 2u > r->size) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        uint8_t len = r->base[r->pos++];
        uint8_t sign = r->base[r->pos++];
        if (r->pos + len > r->size) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        uint64_t val = 0;
        for (size_t i = 0; i < len && i < 8; i++) {
            val |= ((uint64_t)r->base[r->pos + i]) << (i * 8);
        }
        r->pos += len;
        out->kind = DISCORD_ETF_FIELD_INT;
        out->integer = sign ? -(int64_t)val : (int64_t)val;
        return DISCORD_ETF_OK;
    }
    case DISCORD_ETF_LARGE_BIG: {
        if (r->pos + 5u > r->size) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        uint32_t len = be32(r->base + r->pos);
        r->pos += 4u;
        uint8_t sign = r->base[r->pos++];
        if (r->pos + len > r->size) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        uint64_t val = 0;
        for (size_t i = 0; i < len && i < 8; i++) {
            val |= ((uint64_t)r->base[r->pos + i]) << (i * 8);
        }
        r->pos += len;
        out->kind = DISCORD_ETF_FIELD_INT;
        out->integer = sign ? -(int64_t)val : (int64_t)val;
        return DISCORD_ETF_OK;
    }
    default:
        return DISCORD_ETF_ERR_BAD_TAG;
    }
}

static DiscordEtfStatus begin_list(DiscordEtfReader *r, uint32_t *count)
{
    if (r->pos + 5u > r->size) {
        return DISCORD_ETF_ERR_TRUNCATED;
    }
    if (r->base[r->pos] != DISCORD_ETF_LIST) {
        return DISCORD_ETF_ERR_BAD_TAG;
    }
    r->pos += 1u;
    *count = be32(r->base + r->pos);
    r->pos += 4u;
    return DISCORD_ETF_OK;
}

static void end_list(DiscordEtfReader *r)
{
    if (r->pos < r->size && r->base[r->pos] == DISCORD_ETF_NIL) {
        r->pos += 1u;
    }
}

static DiscordEtfStatus skip_any(DiscordEtfReader *r, int depth)
{
    DiscordEtfStatus st;
    DiscordEtfField f;

    if (depth > DC_WS_ETF_DEPTH) {
        return DISCORD_ETF_ERR_DEPTH;
    }
    if (r->pos >= r->size) {
        return DISCORD_ETF_ERR_TRUNCATED;
    }
    if (peek_tag(r) == DISCORD_ETF_LIST) {
        uint32_t count;

        st = begin_list(r, &count);
        if (st != DISCORD_ETF_OK) {
            return st;
        }
        for (uint32_t i = 0u; i < count; i++) {
            st = skip_any(r, depth + 1);
            if (st != DISCORD_ETF_OK) {
                return st;
            }
        }
        end_list(r);
        return DISCORD_ETF_OK;
    }
    if (is_map_tag(peek_tag(r))) {
        st = discord_etf_begin_map(r);
        if (st != DISCORD_ETF_OK) {
            return st;
        }
        while (discord_etf_next_map_key(r) == DISCORD_ETF_OK) {
            st = skip_any(r, depth + 1);
            if (st != DISCORD_ETF_OK) {
                return st;
            }
        }
        return discord_etf_end_map(r);
    }
    st = read_scalar(r, &f);
    if (st != DISCORD_ETF_OK) {
        ESP_LOGW(DC_WS_TAG, "skip_any: read_scalar failed %d at pos %u (tag %u)",
                 st, (unsigned)r->pos, peek_tag(r));
    }
    return st;
}

static bool read_boolean(DiscordEtfReader *r)
{
    DiscordEtfField f;

    if (read_scalar(r, &f) != DISCORD_ETF_OK) {
        return false;
    }
    return f.kind == DISCORD_ETF_FIELD_STRING &&
           discord_etf_slice_equals(&f.slice, "true");
}

static void apply_scalar(DcWsKey key, const DiscordEtfField *f,
                         DcWsFrame *frame)
{
    switch (key) {
    case DC_WS_KEY_OP:
        if (f->kind == DISCORD_ETF_FIELD_INT) {
            frame->op = (uint32_t)f->integer;
        }
        break;
    case DC_WS_KEY_S:
        if (f->kind == DISCORD_ETF_FIELD_INT) {
            frame->seq = (uint32_t)f->integer;
            frame->have_seq = true;
        }
        break;
    case DC_WS_KEY_T:
        if (is_string_field(f->kind) &&
            !discord_etf_slice_equals(&f->slice, "nil")) {
            frame->type = f->slice;
        }
        break;
    default:
        break;
    }
}

bool discord_ws_decode_frame(const uint8_t *data, size_t len,
                             DcWsFrame *frame, const uint8_t **d_data,
                             size_t *d_len)
{
    DiscordEtfReader r;
    size_t d_off = 0u;

    *d_data = NULL;
    *d_len = 0u;
    discord_etf_reader_init(&r, data, len, DC_WS_ETF_DEPTH);
    DiscordEtfStatus st = discord_etf_begin_map(&r);
    if (st != DISCORD_ETF_OK) {
        ESP_LOGW(DC_WS_TAG, "decode_frame: begin_map failed with %d at pos %u (tag %u)",
                 st, (unsigned)r.pos, peek_tag(&r));
        return false;
    }
    while (discord_etf_next_map_key(&r) == DISCORD_ETF_OK) {
        if (match_key(&r.last_key) == DC_WS_KEY_D) {
            d_off = r.pos;
            st = skip_any(&r, 0);
            if (st != DISCORD_ETF_OK) {
                ESP_LOGW(DC_WS_TAG, "decode_frame: skip_any(d) failed with %d at pos %u (tag %u)",
                         st, (unsigned)r.pos, peek_tag(&r));
                return false;
            }
            *d_data = data + d_off;
            *d_len = r.pos - d_off;
        } else if (is_container_tag(peek_tag(&r))) {
            st = skip_any(&r, 0);
            if (st != DISCORD_ETF_OK) {
                ESP_LOGW(DC_WS_TAG, "decode_frame: skip container '%.*s' failed with %d at pos %u (tag %u)",
                         (int)r.last_key.len, r.last_key.ptr ? (const char *)r.last_key.ptr : "",
                         st, (unsigned)r.pos, peek_tag(&r));
                return false;
            }
        } else {
            DiscordEtfField f;

            if (read_scalar(&r, &f) == DISCORD_ETF_OK) {
                apply_scalar(match_key(&r.last_key), &f, frame);
            }
        }
    }
    st = discord_etf_end_map(&r);
    if (st != DISCORD_ETF_OK) {
        ESP_LOGW(DC_WS_TAG, "decode_frame: end_map failed with %d at pos %u, pairs_left %d",
                 st, (unsigned)r.pos, (int)r.pairs_left);
        return false;
    }
    return true;
}

static DiscordId field_id(const DiscordEtfField *f)
{
    if (f->kind == DISCORD_ETF_FIELD_INT) {
        return (uint64_t)f->integer;
    }
    if (is_string_field(f->kind)) {
        return discord_etf_to_id(&f->slice);
    }
    return 0u;
}

static DiscordId read_id(DiscordEtfReader *r)
{
    DiscordEtfField f;

    if (read_scalar(r, &f) != DISCORD_ETF_OK) {
        return 0u;
    }
    return field_id(&f);
}

static void parse_id_map(DiscordEtfReader *r, DiscordId *out)
{
    while (discord_etf_next_map_key(r) == DISCORD_ETF_OK) {
        if (match_key(&r->last_key) == DC_WS_KEY_ID &&
            !is_container_tag(peek_tag(r))) {
            *out = read_id(r);
        } else if (is_container_tag(peek_tag(r))) {
            skip_any(r, 0);
        } else {
            DiscordEtfField f;

            read_scalar(r, &f);
        }
    }
}

static void parse_member(DiscordEtfReader *r, DiscordId *user_id)
{
    while (discord_etf_next_map_key(r) == DISCORD_ETF_OK) {
        if (match_key(&r->last_key) == DC_WS_KEY_USER &&
            is_map_tag(peek_tag(r))) {
            if (discord_etf_begin_map(r) == DISCORD_ETF_OK) {
                parse_id_map(r, user_id);
            }
            discord_etf_end_map(r);
        } else if (is_container_tag(peek_tag(r))) {
            skip_any(r, 0);
        } else {
            DiscordEtfField f;

            read_scalar(r, &f);
        }
    }
}

static void parse_option_value(DiscordEtfReader *r, DiscordInteraction *it,
                               uint32_t *index)
{
    DiscordEtfField f;
    char num[24];

    if (read_scalar(r, &f) != DISCORD_ETF_OK) {
        return;
    }
    if (is_string_field(f.kind)) {
        if (dc_interaction_add_value(it, *index, (const char *)f.slice.ptr,
                                     f.slice.len)) {
            *index += 1u;
        }
        return;
    }
    if (f.kind == DISCORD_ETF_FIELD_INT) {
        size_t len = format_int64(num, sizeof(num), f.integer);

        if (dc_interaction_add_value(it, *index, num, len)) {
            *index += 1u;
        }
    }
}

static void parse_options(DiscordEtfReader *r, DiscordInteraction *it,
                          uint32_t *index, int depth)
{
    uint32_t count;

    if (begin_list(r, &count) != DISCORD_ETF_OK) {
        return;
    }
    for (uint32_t i = 0u; i < count; i++) {
        char name[DC_INTERACTION_SUB_MAX];
        size_t name_len;
        bool nests_options;
        int64_t map_type;

        if (!is_map_tag(peek_tag(r))) {
            skip_any(r, depth + 1);
            continue;
        }
        if (discord_etf_begin_map(r) != DISCORD_ETF_OK) {
            continue;
        }
        name[0] = '\0';
        name_len = 0u;
        nests_options = false;
        map_type = -1;
        while (discord_etf_next_map_key(r) == DISCORD_ETF_OK) {
            DcWsKey key = match_key(&r->last_key);

            if (key == DC_WS_KEY_VALUE && !is_container_tag(peek_tag(r))) {
                parse_option_value(r, it, index);
            } else if (key == DC_WS_KEY_NAME && !is_container_tag(peek_tag(r))) {
                /* An option map carries its own "name". Kept here rather
                 * than written immediately because the subcommand test
                 * below needs the whole map: "name" can arrive either
                 * before or after "type"/"options". */
                DiscordEtfField f;

                if (read_scalar(r, &f) == DISCORD_ETF_OK &&
                    is_string_field(f.kind) &&
                    f.slice.len < sizeof(name)) {
                    memcpy(name, f.slice.ptr, f.slice.len);
                    name[f.slice.len] = '\0';
                    name_len = f.slice.len;
                }
            } else if (key == DC_WS_KEY_TYPE && !is_container_tag(peek_tag(r))) {
                /* A bare subcommand (/bug open) carries no options list,
                 * only {name, type:1}; without this the sub stays empty
                 * and the modal cannot match. Action rows are type 1 too
                 * but carry no "name", so the name_len test still excludes
                 * them. */
                DiscordEtfField f;

                if (read_scalar(r, &f) == DISCORD_ETF_OK &&
                    f.kind == DISCORD_ETF_FIELD_INT) {
                    map_type = f.integer;
                }
            } else if (key == DC_WS_KEY_OPTIONS &&
                       peek_tag(r) == DISCORD_ETF_LIST) {
                nests_options = true;
                parse_options(r, it, index, depth + 1);
            } else if (key == DC_WS_KEY_COMPONENTS &&
                       peek_tag(r) == DISCORD_ETF_LIST) {
                /* "options" (slash/subcommand) and "components" (modal
                 * submit action rows) both nest value-carrying maps. */
                parse_options(r, it, index, depth + 1);
            } else if (is_container_tag(peek_tag(r))) {
                skip_any(r, depth + 1);
            } else {
                DiscordEtfField f;

                read_scalar(r, &f);
            }
        }
        discord_etf_end_map(r);
        /* First writer wins, and the innermost map closes first, so a
         * group/subcommand chain leaves the actual subcommand behind, which
         * is the one name a handler branches on. A subcommand is either a
         * map that nests its own options list or a bare SUB_COMMAND
         * (map_type 1); a plain value option (types 3..11) is never one. */
        if (name_len > 0u && it->sub[0] == '\0' &&
            (nests_options || map_type == 1)) {
            dc_interaction_set_sub(it, name, name_len);
        }
    }
    end_list(r);
}

static void parse_data(DiscordEtfReader *r, DiscordInteraction *it,
                       uint32_t *index)
{
    while (discord_etf_next_map_key(r) == DISCORD_ETF_OK) {
        DcWsKey key = match_key(&r->last_key);

        if (key == DC_WS_KEY_NAME && !is_container_tag(peek_tag(r))) {
            DiscordEtfField f;

            if (read_scalar(r, &f) == DISCORD_ETF_OK &&
                is_string_field(f.kind)) {
                dc_interaction_set_command(it, (const char *)f.slice.ptr,
                                           f.slice.len);
            }
        } else if (key == DC_WS_KEY_CUSTOM_ID &&
                   !is_container_tag(peek_tag(r))) {
            DiscordEtfField f;

            if (read_scalar(r, &f) == DISCORD_ETF_OK &&
                is_string_field(f.kind)) {
                dc_interaction_set_custom_id(it, (const char *)f.slice.ptr,
                                             f.slice.len);
            }
        } else if (key == DC_WS_KEY_OPTIONS &&
                   peek_tag(r) == DISCORD_ETF_LIST) {
            parse_options(r, it, index, 0);
        } else if (key == DC_WS_KEY_COMPONENTS &&
                   peek_tag(r) == DISCORD_ETF_LIST) {
            /* Modal submits carry no "options": the answers ride in
             * data.components (action rows of text inputs), same
             * value-carrying shape — without this, every form submit
             * arrived with value_count 0. */
            parse_options(r, it, index, 0);
        } else if (is_container_tag(peek_tag(r))) {
            skip_any(r, 0);
        } else {
            DiscordEtfField f;

            read_scalar(r, &f);
        }
    }
}

static bool apply_interaction_scalar(const DiscordSlice *key,
                                     DiscordEtfReader *r,
                                     DiscordInteraction *it, char *token,
                                     size_t token_cap, size_t *token_len)
{
    DcWsKey matched = match_key(key);
    DiscordEtfField f;

    if (matched < DC_WS_KEY_ID || matched > DC_WS_KEY_GUILD_ID) {
        return false;
    }
    if (is_container_tag(peek_tag(r))) {
        skip_any(r, 0);
        return true;
    }
    if (read_scalar(r, &f) != DISCORD_ETF_OK) {
        return true;
    }
    switch (matched) {
    case DC_WS_KEY_ID:
        it->id = field_id(&f);
        break;
    case DC_WS_KEY_APPLICATION_ID:
        it->application_id = field_id(&f);
        break;
    case DC_WS_KEY_TYPE:
        if (f.kind == DISCORD_ETF_FIELD_INT) {
            it->type = (uint32_t)f.integer;
        }
        break;
    case DC_WS_KEY_TOKEN:
        if (is_string_field(f.kind)) {
            copy_bounded(token, token_cap, f.slice.ptr, f.slice.len);
            *token_len = strlen(token);
        }
        break;
    case DC_WS_KEY_CHANNEL_ID:
        it->channel_id = field_id(&f);
        break;
    default:
        it->guild_id = field_id(&f);
        break;
    }
    return true;
}

static bool apply_interaction_container(const DiscordSlice *key,
                                        DiscordEtfReader *r,
                                        DiscordInteraction *it,
                                        uint32_t *index)
{
    if (!is_map_tag(peek_tag(r))) {
        return false;
    }
    switch (match_key(key)) {
    case DC_WS_KEY_MESSAGE:
        if (discord_etf_begin_map(r) == DISCORD_ETF_OK) {
            parse_id_map(r, &it->message_id);
        }
        break;
    case DC_WS_KEY_MEMBER:
        if (discord_etf_begin_map(r) == DISCORD_ETF_OK) {
            parse_member(r, &it->user_id);
        }
        break;
    case DC_WS_KEY_USER:
        if (discord_etf_begin_map(r) == DISCORD_ETF_OK) {
            parse_id_map(r, &it->user_id);
        }
        break;
    case DC_WS_KEY_DATA:
        if (discord_etf_begin_map(r) == DISCORD_ETF_OK) {
            parse_data(r, it, index);
        }
        break;
    default:
        return false;
    }
    discord_etf_end_map(r);
    return true;
}

static void apply_interaction_field(const DiscordSlice *key,
                                    DiscordEtfReader *r,
                                    DiscordInteraction *it, char *token,
                                    size_t token_cap, size_t *token_len,
                                    uint32_t *index)
{
    if (apply_interaction_scalar(key, r, it, token, token_cap,
                                 token_len)) {
        return;
    }
    if (apply_interaction_container(key, r, it, index)) {
        return;
    }
    if (is_container_tag(peek_tag(r))) {
        skip_any(r, 0);
        return;
    }
    DiscordEtfField f;

    read_scalar(r, &f);
}

static void enqueue_ack(const DcWsInteraction *parsed)
{
    DiscordAck ack;
    uint8_t kind;

    if (sink_ackq == NULL) {
        return;
    }
    memset(&ack, 0, sizeof(ack));
    ack.id = parsed->it.id;
    if (parsed->it.type == DC_WS_TYPE_PING) {
        kind = DC_WS_ACK_PONG;
    } else if (parsed->it.type == DC_WS_TYPE_COMMAND) {
        /* Modal-first commands: the ACK itself carries {"type":9,...},
         * so the form opens instantly inside the 3 s deadline. Anything
         * else defers (type 5) for the 15-minute followup window. The
         * dynamic builder (per-invocation data such as a picked channel)
         * runs first; its document is copied into the ACK slot. */
        const char *modal = NULL;
        if (sink_modal_build != NULL) {
            char *slot = s_dyn_modal[s_dyn_next];
            modal = sink_modal_build(&parsed->it, slot,
                                     DC_WS_DYN_MODAL_MAX);
            if (modal != NULL &&
                (modal < slot || modal >= slot + DC_WS_DYN_MODAL_MAX)) {
                ESP_LOGW(DC_WS_TAG, "dynamic modal outside scratch, ignored");
                modal = NULL;
            }
            if (modal != NULL) {
                s_dyn_next ^= 1u;
                if (sink_ackq != NULL &&
                    dc_ackq_depth(sink_ackq) > 0u) {
                    ESP_LOGW(DC_WS_TAG,
                             "dynamic modal queued behind %u ack(s)",
                             (unsigned)dc_ackq_depth(sink_ackq));
                }
            }
        }
        if (modal == NULL && sink_modal_for != NULL) {
            modal = sink_modal_for(parsed->it.command, parsed->it.sub);
        }
        if (modal != NULL) {
            kind = 9u;
            ack.modal_json = modal;
        } else {
            kind = 5u;
            /* Node parity for its ephemeral: true commands: visibility is
             * fixed when the deferred message is created, not when the
             * handler later edits @original, so the ACK itself has to be
             * ephemeral. data.flags 64 (the lone flag a type-5 callback
             * accepts) does exactly that. */
            ack.ephemeral = sink_ephemeral_for != NULL &&
                            sink_ephemeral_for(parsed->it.command);
        }
    } else {
        kind = DC_WS_ACK_DEFERRED_UPDATE;
    }
    ack.kind = kind;
    if (kind == 9u) {
        ESP_LOGI(DC_WS_TAG, "interaction id %llu cmd '%s': modal ACK",
                 (unsigned long long)parsed->it.id,
                 parsed->it.command);
    }
    if (parsed->token_len > sizeof(ack.token) - 1u) {
        ESP_LOGW(DC_WS_TAG, "interaction token truncated to %u bytes",
                 (unsigned)(sizeof(ack.token) - 1u));
    }
    copy_bounded(ack.token, sizeof(ack.token), (const uint8_t *)parsed->token,
                 parsed->token_len);
    if (dc_ackq_push(sink_ackq, &ack) == DC_ACK_OK) {
        vTaskDelay(0);
    }
}

static void deliver_interaction(const DcWsInteraction *parsed)
{
    if (parsed->it.id == 0u || parsed->token_len == 0u) {
        ESP_LOGW(DC_WS_TAG, "drop: interaction id %llu token_len %u",
                 (unsigned long long)parsed->it.id,
                 (unsigned)parsed->token_len);
        return;
    }
    ESP_LOGI(DC_WS_TAG, "decoded interaction id %llu token len %u cmd '%s'",
             (unsigned long long)parsed->it.id, (unsigned)parsed->token_len,
             parsed->it.command);
    if (parsed->it.type == DC_WS_TYPE_PING ||
        parsed->it.type == DC_WS_TYPE_COMMAND ||
        parsed->it.type == DC_WS_TYPE_COMPONENT ||
        parsed->it.type == DC_WS_TYPE_MODAL_SUBMIT) {
        enqueue_ack(parsed);
    }
    if (sink_handler != NULL) {
        sink_handler(&parsed->it);
    }
}

static void parse_interaction(DiscordEtfReader *r)
{
    DcWsInteraction *parsed = &s_parsed;

    memset(parsed, 0, sizeof(*parsed));
    while (discord_etf_next_map_key(r) == DISCORD_ETF_OK) {
        apply_interaction_field(&r->last_key, r, &parsed->it, parsed->token,
                                sizeof(parsed->token), &parsed->token_len,
                                &parsed->value_index);
    }
    /* Surface the token to feature handlers through DiscordInteraction. */
    if (parsed->token_len > 0u) {
        copy_bounded(parsed->it.token, sizeof(parsed->it.token),
                     (const uint8_t *)parsed->token, parsed->token_len);
    }
    deliver_interaction(parsed);
}

static uint32_t walk_hello(DiscordEtfReader *r)
{
    uint32_t interval = 0u;

    while (discord_etf_next_map_key(r) == DISCORD_ETF_OK) {
        if (match_key(&r->last_key) == DC_WS_KEY_HEARTBEAT_INTERVAL &&
            !is_container_tag(peek_tag(r))) {
            DiscordEtfField f;

            if (read_scalar(r, &f) == DISCORD_ETF_OK &&
                f.kind == DISCORD_ETF_FIELD_INT) {
                interval = (uint32_t)f.integer;
            }
        } else if (is_container_tag(peek_tag(r))) {
            skip_any(r, 0);
        } else {
            DiscordEtfField f;

            read_scalar(r, &f);
        }
    }
    return interval;
}

static void parse_ready(DcWsFrame *frame, DiscordEtfReader *r)
{
    while (discord_etf_next_map_key(r) == DISCORD_ETF_OK) {
        if (is_container_tag(peek_tag(r))) {
            skip_any(r, 0);
        } else {
            DiscordEtfField f;

            if (read_scalar(r, &f) == DISCORD_ETF_OK &&
                is_string_field(f.kind)) {
                DcWsKey key = match_key(&r->last_key);

                if (key == DC_WS_KEY_SESSION_ID) {
                    frame->have_session_id = true;
                    copy_bounded(frame->session_id,
                                 sizeof(frame->session_id),
                                 f.slice.ptr, f.slice.len);
                } else if (key == DC_WS_KEY_RESUME_GATEWAY_URL) {
                    frame->have_resume_url = true;
                    copy_bounded(frame->resume_url,
                                 sizeof(frame->resume_url),
                                 f.slice.ptr, f.slice.len);
                }
            }
        }
    }
}

/* MESSAGE_CREATE walk: fills a DiscordMessage with zero copies. The content
 * slice borrows the RX frame; the handler runs synchronously on the
 * connection task, so it never outlives the dispatch. Anything missing
 * reads as zero and drops the message (author and id are mandatory). */
static void parse_message_author(DiscordEtfReader *r, DiscordMessage *msg)
{
    while (discord_etf_next_map_key(r) == DISCORD_ETF_OK) {
        DcWsKey key = match_key(&r->last_key);

        if (key == DC_WS_KEY_ID && !is_container_tag(peek_tag(r))) {
            msg->author_id = read_id(r);
        } else if (key == DC_WS_KEY_BOT && !is_container_tag(peek_tag(r))) {
            DiscordEtfField f;

            if (read_scalar(r, &f) == DISCORD_ETF_OK &&
                is_string_field(f.kind) &&
                discord_etf_slice_equals(&f.slice, "true")) {
                msg->flags |= DC_MSG_FLAG_BOT;
            }
        } else if (is_container_tag(peek_tag(r))) {
            skip_any(r, 0);
        } else {
            DiscordEtfField f;

            read_scalar(r, &f);
        }
    }
}

/* Attachments walk: sets DC_MSG_FLAG_IMAGE when any attachment has
 * an image/ content_type prefix. Zero-copy (prefix compare on the slice),
 * no storage: the gate only needs the bit. Unknown shapes skipped. */
static void parse_attachments(DiscordEtfReader *r, DiscordMessage *msg)
{
    static const char img[] = "image/";
    uint32_t count;

    if (begin_list(r, &count) != DISCORD_ETF_OK) {
        return;
    }
    for (uint32_t i = 0u; i < count; i++) {
        if (!is_map_tag(peek_tag(r))) {
            skip_any(r, 1);
            continue;
        }
        if (discord_etf_begin_map(r) != DISCORD_ETF_OK) {
            continue;
        }
        while (discord_etf_next_map_key(r) == DISCORD_ETF_OK) {
            if (match_key(&r->last_key) == DC_WS_KEY_CONTENT_TYPE &&
                !is_container_tag(peek_tag(r))) {
                DiscordEtfField f;

                if (read_scalar(r, &f) == DISCORD_ETF_OK &&
                    is_string_field(f.kind) &&
                    f.slice.len >= sizeof(img) - 1u &&
                    memcmp(f.slice.ptr, img, sizeof(img) - 1u) == 0) {
                    msg->flags |= DC_MSG_FLAG_IMAGE;
                }
            } else if (is_container_tag(peek_tag(r))) {
                skip_any(r, 1);
            } else {
                DiscordEtfField f;

                read_scalar(r, &f);
            }
        }
        discord_etf_end_map(r);
    }
    end_list(r);
}

static void parse_message(DiscordEtfReader *r)
{
    DiscordMessage msg;

    memset(&msg, 0, sizeof(msg));
    while (discord_etf_next_map_key(r) == DISCORD_ETF_OK) {
        DcWsKey key = match_key(&r->last_key);

        if (key == DC_WS_KEY_ID && !is_container_tag(peek_tag(r))) {
            msg.message_id = read_id(r);
        } else if (key == DC_WS_KEY_CHANNEL_ID &&
                   !is_container_tag(peek_tag(r))) {
            msg.channel_id = read_id(r);
        } else if (key == DC_WS_KEY_GUILD_ID &&
                   !is_container_tag(peek_tag(r))) {
            msg.guild_id = read_id(r);
        } else if (key == DC_WS_KEY_AUTHOR && is_map_tag(peek_tag(r))) {
            if (discord_etf_begin_map(r) == DISCORD_ETF_OK) {
                parse_message_author(r, &msg);
            }
            discord_etf_end_map(r);
        } else if (key == DC_WS_KEY_CONTENT &&
                   !is_container_tag(peek_tag(r))) {
            DiscordEtfField f;

            if (read_scalar(r, &f) == DISCORD_ETF_OK &&
                is_string_field(f.kind)) {
                msg.content.ptr = f.slice.ptr;
                msg.content.len = f.slice.len;
            }
        } else if (key == DC_WS_KEY_ATTACHMENTS &&
                   peek_tag(r) == DISCORD_ETF_LIST) {
            parse_attachments(r, &msg);
        } else if (is_container_tag(peek_tag(r))) {
            skip_any(r, 0);
        } else {
            DiscordEtfField f;

            read_scalar(r, &f);
        }
    }
    if (sink_message != NULL && msg.message_id != 0u &&
        msg.author_id != 0u) {
        sink_message(&msg);
    }
}

/* GUILD_MEMBER_ADD / REMOVE: {guild_id, user:{id}}. Needs the privileged
 * GUILD_MEMBERS intent or Discord never sends these. */
static void parse_member_event(DiscordEtfReader *r, bool joined)
{
    DiscordId guild = 0u;
    DiscordId user = 0u;

    while (discord_etf_next_map_key(r) == DISCORD_ETF_OK) {
        DcWsKey key = match_key(&r->last_key);

        if (key == DC_WS_KEY_GUILD_ID && !is_container_tag(peek_tag(r))) {
            guild = read_id(r);
        } else if (key == DC_WS_KEY_USER && is_map_tag(peek_tag(r))) {
            if (discord_etf_begin_map(r) == DISCORD_ETF_OK) {
                parse_id_map(r, &user);
            }
            discord_etf_end_map(r);
        } else if (is_container_tag(peek_tag(r))) {
            skip_any(r, 0);
        } else {
            DiscordEtfField f;

            read_scalar(r, &f);
        }
    }
    if (sink_member != NULL && guild != 0u && user != 0u) {
        sink_member((uint64_t)guild, (uint64_t)user, joined);
    }
}

static void walk_dispatch(DcWsFrame *frame, DiscordEtfReader *r)
{
    if (frame->type.len == 0u) {
        skip_any(r, 0);
        return;
    }
    if (discord_etf_slice_equals(&frame->type, "INTERACTION_CREATE")) {
        parse_interaction(r);
        return;
    }
    if (discord_etf_slice_equals(&frame->type, "MESSAGE_CREATE")) {
        parse_message(r);
        return;
    }
    if (discord_etf_slice_equals(&frame->type, "GUILD_MEMBER_ADD")) {
        parse_member_event(r, true);
        return;
    }
    if (discord_etf_slice_equals(&frame->type, "GUILD_MEMBER_REMOVE")) {
        parse_member_event(r, false);
        return;
    }
    if (discord_etf_slice_equals(&frame->type, "READY")) {
        parse_ready(frame, r);
        return;
    }
    skip_any(r, 0);
}

void discord_ws_decode_wire(const DcWsDecodeSinks *sinks)
{
    if (sinks == NULL) {
        return;
    }
    sink_ackq = sinks->ackq;
    sink_handler = sinks->interaction_handler;
    sink_message = sinks->message_handler;
    sink_member = sinks->member_handler;
    sink_modal_for = sinks->modal_for;
    sink_modal_build = sinks->modal_build;
    sink_ephemeral_for = sinks->ephemeral_for;
}

uint32_t discord_ws_decode_hello(const uint8_t *d_data, size_t d_len)
{
    DiscordEtfReader r;

    discord_etf_reader_init(&r, d_data, d_len, DC_WS_ETF_DEPTH);
    if (!is_map_tag(peek_tag(&r)) ||
        discord_etf_begin_map(&r) != DISCORD_ETF_OK) {
        return 0u;
    }
    return walk_hello(&r);
}

bool discord_ws_decode_invalid_session(const uint8_t *d_data,
                                       size_t d_len)
{
    DiscordEtfReader r;

    discord_etf_reader_init(&r, d_data, d_len, DC_WS_ETF_DEPTH);
    return read_boolean(&r);
}

void discord_ws_decode_ready(DcWsFrame *frame, const uint8_t *d_data,
                             size_t d_len)
{
    DiscordEtfReader r;

    discord_etf_reader_init(&r, d_data, d_len, DC_WS_ETF_DEPTH);
    if (!is_map_tag(peek_tag(&r)) ||
        discord_etf_begin_map(&r) != DISCORD_ETF_OK) {
        return;
    }
    parse_ready(frame, &r);
}

void discord_ws_decode_dispatch(DcWsFrame *frame, const uint8_t *d_data,
                                size_t d_len)
{
    DiscordEtfReader r;

    discord_etf_reader_init(&r, d_data, d_len, DC_WS_ETF_DEPTH);
    if (!is_map_tag(peek_tag(&r))) {
        skip_any(&r, 0);
        return;
    }
    if (discord_etf_begin_map(&r) != DISCORD_ETF_OK) {
        return;
    }
    walk_dispatch(frame, &r);
    discord_etf_end_map(&r);
}
