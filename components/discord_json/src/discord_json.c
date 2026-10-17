#include "discord_json.h"

#include <string.h>

#define DC_JSON_MAX_DEPTH 8

static DcJsonStatus put_i64(DcJson *w, int64_t v);

void dc_json_init(DcJson *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->depth = 0;
    w->need_comma = false;
    w->after_key = false;
    w->overflowed = false;
    if (buf && cap > 0) {
        buf[0] = '\0';
    }
}

static DcJsonStatus put_char(DcJson *w, char c)
{
    if (w->len + 1 >= w->cap) {
        w->overflowed = true;
        return DC_JSON_ERR_NOMEM;
    }
    w->buf[w->len++] = c;
    w->buf[w->len] = '\0';
    return DC_JSON_OK;
}

static DcJsonStatus put_raw(DcJson *w, const char *s, size_t n)
{
    if (w->len + n >= w->cap) {
        w->overflowed = true;
        return DC_JSON_ERR_NOMEM;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
    return DC_JSON_OK;
}

size_t dc_json_escape_len(const char *s, size_t len)
{
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':
        case '\\':
        case '\n':
        case '\r':
        case '\t':
        case '\b':
        case '\f':
            out += 2;
            break;
        default:
            out += (c < 0x20) ? 6 : 1;
            break;
        }
    }
    return out;
}

static DcJsonStatus put_escaped(DcJson *w, const char *s, size_t len)
{
    DcJsonStatus st = put_char(w, '"');
    if (st != DC_JSON_OK) {
        return st;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':
            st = put_raw(w, "\\\"", 2);
            break;
        case '\\':
            st = put_raw(w, "\\\\", 2);
            break;
        case '\n':
            st = put_raw(w, "\\n", 2);
            break;
        case '\r':
            st = put_raw(w, "\\r", 2);
            break;
        case '\t':
            st = put_raw(w, "\\t", 2);
            break;
        case '\b':
            st = put_raw(w, "\\b", 2);
            break;
        case '\f':
            st = put_raw(w, "\\f", 2);
            break;
        default:
            if (c < 0x20) {
                static const char hex[] = "0123456789abcdef";
                char esc[6] = { '\\', 'u', '0', '0', 0, 0 };
                esc[4] = hex[(c >> 4) & 0xF];
                esc[5] = hex[c & 0xF];
                st = put_raw(w, esc, 6);
            } else {
                st = put_char(w, (char)c);
            }
            break;
        }
        if (st != DC_JSON_OK) {
            return st;
        }
    }
    return put_char(w, '"');
}

static DcJsonStatus separator(DcJson *w)
{
    if (w->need_comma) {
        return put_char(w, ',');
    }
    w->need_comma = true;
    return DC_JSON_OK;
}

static DcJsonStatus enter(DcJson *w)
{
    if (w->depth >= DC_JSON_MAX_DEPTH) {
        return DC_JSON_ERR_DEPTH;
    }
    w->depth++;
    return DC_JSON_OK;
}

DcJsonStatus dc_json_obj_begin(DcJson *w)
{
    if (!w->after_key) {
        DcJsonStatus st = separator(w);
        if (st != DC_JSON_OK) {
            return st;
        }
    }
    w->after_key = false;
    DcJsonStatus st = enter(w);
    if (st != DC_JSON_OK) {
        return st;
    }
    w->need_comma = false;
    return put_char(w, '{');
}

DcJsonStatus dc_json_obj_end(DcJson *w)
{
    if (w->depth <= 0) {
        return DC_JSON_ERR_TYPE;
    }
    w->depth--;
    DcJsonStatus st = put_char(w, '}');
    w->need_comma = true;
    return st;
}

DcJsonStatus dc_json_arr_begin(DcJson *w)
{
    if (!w->after_key) {
        DcJsonStatus st = separator(w);
        if (st != DC_JSON_OK) {
            return st;
        }
    }
    w->after_key = false;
    DcJsonStatus st = enter(w);
    if (st != DC_JSON_OK) {
        return st;
    }
    w->need_comma = false;
    return put_char(w, '[');
}

DcJsonStatus dc_json_arr_end(DcJson *w)
{
    if (w->depth <= 0) {
        return DC_JSON_ERR_TYPE;
    }
    w->depth--;
    DcJsonStatus st = put_char(w, ']');
    w->need_comma = true;
    return st;
}

DcJsonStatus dc_json_key(DcJson *w, const char *key)
{
    DcJsonStatus st = separator(w);
    if (st != DC_JSON_OK) {
        return st;
    }
    size_t klen = strlen(key);
    st = put_escaped(w, key, klen);
    if (st != DC_JSON_OK) {
        return st;
    }
    st = put_char(w, ':');
    if (st != DC_JSON_OK) {
        return st;
    }
    w->after_key = true;
    return DC_JSON_OK;
}

DcJsonStatus dc_json_strn(DcJson *w, const char *key, const char *value, size_t len)
{
    DcJsonStatus st = dc_json_key(w, key);
    if (st != DC_JSON_OK) {
        return st;
    }
    w->after_key = false;
    return put_escaped(w, value, len);
}

DcJsonStatus dc_json_str(DcJson *w, const char *key, const char *value)
{
    return dc_json_strn(w, key, value, strlen(value));
}

DcJsonStatus dc_json_arr_str(DcJson *w, const char *value)
{
    DcJsonStatus st = separator(w);
    if (st != DC_JSON_OK) {
        return st;
    }
    return put_escaped(w, value, strlen(value));
}

DcJsonStatus dc_json_arr_i64(DcJson *w, int64_t value)
{
    DcJsonStatus st = separator(w);
    if (st != DC_JSON_OK) {
        return st;
    }
    return put_i64(w, value);
}

static DcJsonStatus put_i64(DcJson *w, int64_t v)
{
    char tmp[24];
    size_t n = 0;
    bool neg = v < 0;

    uint64_t mag = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    if (mag == 0) {
        tmp[n++] = '0';
    }
    while (mag > 0) {
        tmp[n++] = (char)('0' + (mag % 10));
        mag /= 10;
    }
    if (neg) {
        tmp[n++] = '-';
    }

    char out[24];
    for (size_t i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    return put_raw(w, out, n);
}

DcJsonStatus dc_json_i64(DcJson *w, const char *key, int64_t value)
{
    DcJsonStatus st = dc_json_key(w, key);
    if (st != DC_JSON_OK) {
        return st;
    }
    w->after_key = false;
    return put_i64(w, value);
}

DcJsonStatus dc_json_u64(DcJson *w, const char *key, uint64_t value)
{
    if (value > (uint64_t)INT64_MAX) {
        return DC_JSON_ERR_NUMBER;
    }
    return dc_json_i64(w, key, (int64_t)value);
}

DcJsonStatus dc_json_bool(DcJson *w, const char *key, bool value)
{
    DcJsonStatus st = dc_json_key(w, key);
    if (st != DC_JSON_OK) {
        return st;
    }
    w->after_key = false;
    if (value) {
        return put_raw(w, "true", 4);
    }
    return put_raw(w, "false", 5);
}

DcJsonStatus dc_json_null(DcJson *w, const char *key)
{
    DcJsonStatus st = dc_json_key(w, key);
    if (st != DC_JSON_OK) {
        return st;
    }
    w->after_key = false;
    return put_raw(w, "null", 4);
}

size_t dc_json_len(const DcJson *w)
{
    return w->len;
}

const char *dc_json_cstr(const DcJson *w)
{
    return w->buf ? w->buf : "";
}

bool dc_json_valid(const DcJson *w)
{
    return !w->overflowed && w->depth == 0 && w->len > 0;
}