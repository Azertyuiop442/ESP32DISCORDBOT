#include "discord_etf.h"

#include <stdint.h>
#include <string.h>

#define ETF_DEFAULT_MAX_DEPTH 16

static bool reader_has(const DiscordEtfReader *r, size_t n)
{
    return r->pos <= r->size && n <= r->size - r->pos;
}

static uint8_t read_u8(DiscordEtfReader *r)
{
    return r->base[r->pos++];
}

static uint16_t read_u16(DiscordEtfReader *r)
{
    uint16_t v = (uint16_t)((uint16_t)r->base[r->pos] << 8 | (uint16_t)r->base[r->pos + 1]);
    r->pos += 2;
    return v;
}

static uint32_t read_u32(DiscordEtfReader *r)
{
    uint32_t v = (uint32_t)r->base[r->pos] << 24 |
                 (uint32_t)r->base[r->pos + 1] << 16 |
                 (uint32_t)r->base[r->pos + 2] << 8 |
                 (uint32_t)r->base[r->pos + 3];
    r->pos += 4;
    return v;
}

static int64_t read_signed_be(DiscordEtfReader *r, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        v = (v << 8) | (uint64_t)r->base[r->pos + i];
    }
    r->pos += n;
    uint64_t sign_bit = (uint64_t)1 << (n * 8 - 1);
    if (v & sign_bit) {
        uint64_t mask = ((uint64_t)1 << (n * 8)) - 1;
        return (int64_t)(v | ~mask);
    }
    return (int64_t)v;
}

static DiscordEtfStatus read_slice(DiscordEtfReader *r, uint16_t len, DiscordSlice *out)
{
    if (!reader_has(r, len)) {
        return DISCORD_ETF_ERR_TRUNCATED;
    }
    out->ptr = r->base + r->pos;
    out->len = len;
    r->pos += len;
    return DISCORD_ETF_OK;
}

void discord_etf_reader_init(DiscordEtfReader *r, const void *data, size_t size, int max_depth)
{
    r->base = (const uint8_t *)data;
    r->size = size;
    r->pos = 0;
    r->depth = 0;
    r->max_depth = (max_depth > 0 && max_depth <= DISCORD_ETF_MAX_DEPTH) ? max_depth : DISCORD_ETF_MAX_DEPTH;
    r->pairs_left = 0;
    memset(r->pairs_stack, 0, sizeof(r->pairs_stack));
    r->last_key.ptr = NULL;
    r->last_key.len = 0;
}

DiscordEtfStatus discord_etf_begin_map(DiscordEtfReader *r)
{
    if (!reader_has(r, 1)) {
        return DISCORD_ETF_ERR_TRUNCATED;
    }
    uint8_t tag = read_u8(r);
    if (tag == 131) {
        if (!reader_has(r, 1)) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        tag = read_u8(r);
    }
    if (r->depth >= r->max_depth) {
        return DISCORD_ETF_ERR_DEPTH;
    }

    uint32_t count = 0;
    switch (tag) {
    case DISCORD_ETF_SMALL_MAP:
        /* In standard Erlang ETF, tag 116 (MAP_EXT) has a 4-byte big-endian count.
         * Discord sends 4 bytes. If the next 2 bytes are 0x00, read 4 bytes;
         * otherwise support legacy/test 1-byte count. */
        if (reader_has(r, 4) && r->base[r->pos] == 0 && r->base[r->pos + 1] == 0) {
            count = read_u32(r);
        } else {
            if (!reader_has(r, 1)) {
                return DISCORD_ETF_ERR_TRUNCATED;
            }
            count = read_u8(r);
        }
        break;
    case DISCORD_ETF_SMALL_TUPLE:
        if (!reader_has(r, 1)) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        count = read_u8(r);
        break;
    case DISCORD_ETF_MAP:
    case DISCORD_ETF_LARGE_TUPLE:
        if (!reader_has(r, 4)) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        count = read_u32(r);
        break;
    default:
        return DISCORD_ETF_ERR_BAD_TAG;
    }

    if (count > (uint32_t)INT32_MAX) {
        return DISCORD_ETF_ERR_BAD_TAG;
    }
    r->pairs_stack[r->depth] = (int32_t)count;
    r->depth++;
    r->pairs_left = (int32_t)count;
    return DISCORD_ETF_OK;
}

DiscordEtfStatus discord_etf_end_map(DiscordEtfReader *r)
{
    if (r->depth <= 0) {
        return DISCORD_ETF_ERR_CONTAINER;
    }
    if (r->pairs_stack[r->depth - 1] != 0) {
        return DISCORD_ETF_ERR_CONTAINER;
    }
    r->depth--;
    r->pairs_left = r->depth > 0 ? r->pairs_stack[r->depth - 1] : 0;
    return DISCORD_ETF_OK;
}

static DiscordEtfStatus read_scalar(DiscordEtfReader *r, DiscordEtfField *out)
{
    if (!reader_has(r, 1)) {
        return DISCORD_ETF_ERR_TRUNCATED;
    }
    uint8_t tag = read_u8(r);

    out->slice.ptr = NULL;
    out->slice.len = 0;
    out->integer = 0;

    switch (tag) {
    case DISCORD_ETF_SMALL_INTEGER:
        if (!reader_has(r, 1)) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        out->kind = DISCORD_ETF_FIELD_INT;
        out->integer = read_u8(r);
        return DISCORD_ETF_OK;
    case DISCORD_ETF_INTEGER:
        if (!reader_has(r, 4)) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        out->kind = DISCORD_ETF_FIELD_INT;
        out->integer = read_signed_be(r, 4);
        return DISCORD_ETF_OK;
    case DISCORD_ETF_FLOAT:
        if (!reader_has(r, 31)) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        r->pos += 31;
        out->kind = DISCORD_ETF_FIELD_NIL;
        return DISCORD_ETF_OK;
    case DISCORD_ETF_NEW_FLOAT:
        if (!reader_has(r, 8)) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        r->pos += 8;
        out->kind = DISCORD_ETF_FIELD_NIL;
        return DISCORD_ETF_OK;
    case DISCORD_ETF_ATOM: {
        if (!reader_has(r, 2)) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        uint16_t len = read_u16(r);
        out->kind = DISCORD_ETF_FIELD_STRING;
        return read_slice(r, len, &out->slice);
    }
    case DISCORD_ETF_SMALL_ATOM_UTF8:
        if (!reader_has(r, 1)) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        out->kind = DISCORD_ETF_FIELD_STRING;
        return read_slice(r, read_u8(r), &out->slice);
    case DISCORD_ETF_ATOM_UTF8:
        if (!reader_has(r, 2)) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        out->kind = DISCORD_ETF_FIELD_STRING;
        return read_slice(r, read_u16(r), &out->slice);
    case DISCORD_ETF_NIL:
        out->kind = DISCORD_ETF_FIELD_NIL;
        return DISCORD_ETF_OK;
    case DISCORD_ETF_STRING:
        if (!reader_has(r, 2)) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        out->kind = DISCORD_ETF_FIELD_STRING;
        return read_slice(r, read_u16(r), &out->slice);
    case DISCORD_ETF_BINARY: {
        if (!reader_has(r, 4)) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        uint32_t len = read_u32(r);
        if (len > 0xFFFFu) {
            return DISCORD_ETF_ERR_BAD_TAG;
        }
        out->kind = DISCORD_ETF_FIELD_BINARY;
        return read_slice(r, (uint16_t)len, &out->slice);
    }
    case DISCORD_ETF_SMALL_BIG: {
        if (!reader_has(r, 2)) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        uint8_t len = read_u8(r);
        uint8_t sign = read_u8(r);
        if (!reader_has(r, len)) {
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
        if (!reader_has(r, 5)) {
            return DISCORD_ETF_ERR_TRUNCATED;
        }
        uint32_t len = read_u32(r);
        uint8_t sign = read_u8(r);
        if (!reader_has(r, len)) {
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
    case DISCORD_ETF_SMALL_TUPLE:
    case DISCORD_ETF_LARGE_TUPLE:
    case DISCORD_ETF_LIST:
    case DISCORD_ETF_SMALL_MAP:
    case DISCORD_ETF_MAP:
        return DISCORD_ETF_ERR_CONTAINER;
    default:
        return DISCORD_ETF_ERR_BAD_TAG;
    }
}

DiscordEtfStatus discord_etf_next_map_key(DiscordEtfReader *r)
{
    if (r->depth <= 0 || r->pairs_stack[r->depth - 1] <= 0) {
        return DISCORD_ETF_ERR_CONTAINER;
    }

    DiscordEtfField key;
    DiscordEtfStatus st = read_scalar(r, &key);
    if (st != DISCORD_ETF_OK) {
        return st;
    }
    if (key.kind != DISCORD_ETF_FIELD_STRING && key.kind != DISCORD_ETF_FIELD_BINARY) {
        return DISCORD_ETF_ERR_BAD_TAG;
    }

    r->pairs_stack[r->depth - 1]--;
    r->pairs_left = r->pairs_stack[r->depth - 1];
    r->last_key = key.slice;
    return DISCORD_ETF_OK;
}

DiscordEtfStatus discord_etf_next_map_field(DiscordEtfReader *r, DiscordEtfField *out)
{
    if (r->depth <= 0 || r->pairs_stack[r->depth - 1] <= 0) {
        out->kind = DISCORD_ETF_FIELD_END;
        out->slice.ptr = NULL;
        out->slice.len = 0;
        out->integer = 0;
        return DISCORD_ETF_OK;
    }

    DiscordEtfField key;
    DiscordEtfStatus st = read_scalar(r, &key);
    if (st != DISCORD_ETF_OK) {
        return st;
    }
    if (key.kind != DISCORD_ETF_FIELD_STRING && key.kind != DISCORD_ETF_FIELD_BINARY) {
        return DISCORD_ETF_ERR_BAD_TAG;
    }

    DiscordEtfField val;
    st = read_scalar(r, &val);
    if (st != DISCORD_ETF_OK) {
        return st;
    }

    r->pairs_stack[r->depth - 1]--;
    r->pairs_left = r->pairs_stack[r->depth - 1];
    r->last_key = key.slice;
    *out = val;
    return DISCORD_ETF_OK;
}

DiscordEtfStatus discord_etf_skip_value(DiscordEtfReader *r)
{
    DiscordEtfField f;
    return read_scalar(r, &f);
}

bool discord_etf_slice_equals(const DiscordSlice *s, const char *lit)
{
    size_t len = strlen(lit);
    if (s->ptr == NULL || (size_t)s->len != len) {
        return false;
    }
    return memcmp(s->ptr, lit, len) == 0;
}

uint64_t discord_etf_to_id(const DiscordSlice *s)
{
    if (s->ptr == NULL || s->len == 0 || s->len > 20) {
        return 0;
    }

    uint64_t v = 0;
    for (uint16_t i = 0; i < s->len; i++) {
        char c = (char)s->ptr[i];
        if (c < '0' || c > '9') {
            return 0;
        }
        uint64_t digit = (uint64_t)(c - '0');

        if (v > (UINT64_MAX - digit) / 10u) {
            return 0;
        }
        v = v * 10u + digit;
    }
    return v;
}
