#include <stdio.h>
#include <string.h>

#include "discord_etf.h"

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

static void put_u8(uint8_t *b, size_t *p, uint8_t v)
{
    b[(*p)++] = v;
}

static void put_u16(uint8_t *b, size_t *p, uint16_t v)
{
    b[(*p)++] = (uint8_t)(v >> 8);
    b[(*p)++] = (uint8_t)v;
}

static void put_u32(uint8_t *b, size_t *p, uint32_t v)
{
    b[(*p)++] = (uint8_t)(v >> 24);
    b[(*p)++] = (uint8_t)(v >> 16);
    b[(*p)++] = (uint8_t)(v >> 8);
    b[(*p)++] = (uint8_t)v;
}

static void put_str(uint8_t *b, size_t *p, const char *s)
{
    size_t n = strlen(s);
    put_u8(b, p, DISCORD_ETF_STRING);
    put_u16(b, p, (uint16_t)n);
    memcpy(b + *p, s, n);
    *p += n;
}

static void put_small_int(uint8_t *b, size_t *p, uint8_t v)
{
    put_u8(b, p, DISCORD_ETF_SMALL_INTEGER);
    put_u8(b, p, v);
}

static void put_int(uint8_t *b, size_t *p, int32_t v)
{
    put_u8(b, p, DISCORD_ETF_INTEGER);
    put_u32(b, p, (uint32_t)v);
}

static void put_int64(uint8_t *b, size_t *p, int64_t v)
{
    if (v >= (int64_t)INT32_MIN && v <= (int64_t)INT32_MAX) {
        put_int(b, p, (int32_t)v);
        return;
    }
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    put_u8(b, p, DISCORD_ETF_STRING);
    put_u16(b, p, (uint16_t)n);
    memcpy(b + *p, tmp, (size_t)n);
    *p += (size_t)n;
}

static void put_atom(uint8_t *b, size_t *p, const char *s)
{
    size_t n = strlen(s);
    put_u8(b, p, DISCORD_ETF_ATOM);
    put_u16(b, p, (uint16_t)n);
    memcpy(b + *p, s, n);
    *p += n;
}

static void put_small_atom(uint8_t *b, size_t *p, const char *s)
{
    size_t n = strlen(s);
    put_u8(b, p, DISCORD_ETF_SMALL_ATOM_UTF8);
    put_u8(b, p, (uint8_t)n);
    memcpy(b + *p, s, n);
    *p += n;
}

static void test_small_map(void)
{
    uint8_t buf[128];
    size_t p = 0;

    put_u8(buf, &p, DISCORD_ETF_SMALL_MAP);
    put_u8(buf, &p, 3);
    put_str(buf, &p, "op");
    put_small_int(buf, &p, 0);
    put_str(buf, &p, "t");
    put_str(buf, &p, "MESSAGE_CREATE");
    put_str(buf, &p, "s");
    put_int64(buf, &p, 42);

    DiscordEtfReader r;
    discord_etf_reader_init(&r, buf, p, 8);
    check(discord_etf_begin_map(&r) == DISCORD_ETF_OK, "begin_map small");

    DiscordEtfField f;
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              discord_etf_slice_equals(&r.last_key, "op") && f.integer == 0,
          "field op");
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              discord_etf_slice_equals(&r.last_key, "t") &&
              discord_etf_slice_equals(&f.slice, "MESSAGE_CREATE"),
          "field t");
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              discord_etf_slice_equals(&r.last_key, "s") && f.integer == 42,
          "field s (int snowflake)");
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              f.kind == DISCORD_ETF_FIELD_END,
          "map end");
    check(discord_etf_end_map(&r) == DISCORD_ETF_OK, "end_map");
    check(r.pos == p, "cursor consumed exactly the payload");
}

static void test_large_map(void)
{
    uint8_t buf[128];
    size_t p = 0;

    put_u8(buf, &p, DISCORD_ETF_MAP);
    put_u32(buf, &p, 2);
    put_str(buf, &p, "d");
    put_u8(buf, &p, DISCORD_ETF_SMALL_MAP);
    put_u8(buf, &p, 2);
    put_str(buf, &p, "123456789012345678");
    put_str(buf, &p, "guild_id");
    put_str(buf, &p, "987654321");
    put_int64(buf, &p, 987654321);

    DiscordEtfReader r;
    discord_etf_reader_init(&r, buf, p, 8);
    check(discord_etf_begin_map(&r) == DISCORD_ETF_OK, "begin_map large");

    DiscordEtfField f;
    check(discord_etf_next_map_key(&r) == DISCORD_ETF_OK &&
              discord_etf_slice_equals(&r.last_key, "d"),
          "outer key read");
    check(discord_etf_begin_map(&r) == DISCORD_ETF_OK, "nested map begin");
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              discord_etf_slice_equals(&r.last_key, "123456789012345678") &&
              discord_etf_slice_equals(&f.slice, "guild_id"),
          "nested key is snowflake-as-string");
    check(discord_etf_to_id(&r.last_key) == 123456789012345678ULL, "nested id to u64");
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              discord_etf_slice_equals(&r.last_key, "987654321") && f.integer == 987654321,
          "nested value int");
    check(discord_etf_end_map(&r) == DISCORD_ETF_OK, "nested end_map");
}

static void test_truncated(void)
{
    uint8_t buf[64] = {DISCORD_ETF_SMALL_MAP, 3, DISCORD_ETF_STRING, 0x00};

    DiscordEtfReader r;
    discord_etf_reader_init(&r, buf, 4, 8);
    check(discord_etf_begin_map(&r) == DISCORD_ETF_OK, "truncated: begin ok");

    DiscordEtfField f;
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_ERR_TRUNCATED,
          "truncated: detected");
}

static void test_bad_tag(void)
{
    uint8_t buf[1] = {42};

    DiscordEtfReader r;
    discord_etf_reader_init(&r, buf, sizeof(buf), 8);
    check(discord_etf_begin_map(&r) == DISCORD_ETF_ERR_BAD_TAG, "bad tag rejected");
}

static void test_depth_limit(void)
{
    uint8_t buf[32];
    size_t p = 0;
    for (int i = 0; i < 8; i++) {
        put_u8(buf, &p, DISCORD_ETF_SMALL_MAP);
        put_u8(buf, &p, 0);
    }

    DiscordEtfReader r;
    discord_etf_reader_init(&r, buf, p, 3);
    DiscordEtfStatus st = DISCORD_ETF_OK;
    for (int i = 0; i < 8 && st == DISCORD_ETF_OK; i++) {
        st = discord_etf_begin_map(&r);
    }
    check(st == DISCORD_ETF_ERR_DEPTH, "depth limit enforced");
}

static void test_atom_truncated(void)
{
    uint8_t buf[8] = {
        DISCORD_ETF_SMALL_MAP, 1,
        DISCORD_ETF_STRING, 0, 1, 'k',
        DISCORD_ETF_ATOM, 0x00
    };
    DiscordEtfReader r;
    discord_etf_reader_init(&r, buf, sizeof(buf), 8);
    check(discord_etf_begin_map(&r) == DISCORD_ETF_OK, "atom100: map opened");

    DiscordEtfField f;
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_ERR_TRUNCATED,
          "atom100: missing length byte detected");
    check(r.pos <= r.size, "atom100: cursor not left past the buffer");

    uint8_t buf2[7] = {
        DISCORD_ETF_SMALL_MAP, 1,
        DISCORD_ETF_STRING, 0, 1, 'd',
        DISCORD_ETF_SMALL_ATOM_UTF8
    };
    DiscordEtfReader r2;
    discord_etf_reader_init(&r2, buf2, sizeof(buf2), 8);
    check(discord_etf_begin_map(&r2) == DISCORD_ETF_OK, "atom118: map opened");
    check(discord_etf_next_map_field(&r2, &f) == DISCORD_ETF_ERR_TRUNCATED,
          "atom118: missing length byte detected");
    check(r2.pos <= r2.size, "atom118: cursor not left past the buffer");

    uint8_t buf3[8] = {
        DISCORD_ETF_SMALL_MAP, 1,
        DISCORD_ETF_STRING, 0, 1, 'e',
        DISCORD_ETF_ATOM_UTF8, 0x00
    };
    DiscordEtfReader r3;
    discord_etf_reader_init(&r3, buf3, sizeof(buf3), 8);
    check(discord_etf_begin_map(&r3) == DISCORD_ETF_OK, "atom119: map opened");
    check(discord_etf_next_map_field(&r3, &f) == DISCORD_ETF_ERR_TRUNCATED,
          "atom119: missing second length byte detected");
    check(r3.pos <= r3.size, "atom119: cursor not left past the buffer");

    uint8_t buf4[9] = {
        DISCORD_ETF_SMALL_MAP, 1,
        DISCORD_ETF_STRING, 0, 1, 'f',
        DISCORD_ETF_ATOM, 0x00, 0x04
    };
    DiscordEtfReader r4;
    discord_etf_reader_init(&r4, buf4, sizeof(buf4), 8);
    check(discord_etf_begin_map(&r4) == DISCORD_ETF_OK, "atom100b: map opened");
    check(discord_etf_next_map_field(&r4, &f) == DISCORD_ETF_ERR_TRUNCATED,
          "atom100b: declared text missing detected");
}

static void test_huge_map_count(void)
{
    uint8_t buf[13];
    size_t p = 0;
    put_u8(buf, &p, DISCORD_ETF_MAP);
    put_u32(buf, &p, 0xFFFFFFFFu);

    DiscordEtfReader r;
    discord_etf_reader_init(&r, buf, p, 8);
    check(discord_etf_begin_map(&r) == DISCORD_ETF_ERR_BAD_TAG,
          "absurd map count rejected");

    uint8_t buf2[13];
    size_t p2 = 0;
    put_u8(buf2, &p2, DISCORD_ETF_MAP);
    put_u32(buf2, &p2, 0x80000000u);
    discord_etf_reader_init(&r, buf2, p2, 8);
    check(discord_etf_begin_map(&r) == DISCORD_ETF_ERR_BAD_TAG,
          "count above INT32_MAX rejected");
}

static void test_id_overflow(void)
{
    uint8_t d2[] = "99999999999999999999";
    DiscordSlice s2 = { d2, 20 };
    check(discord_etf_to_id(&s2) == 0, "20-digit overflow rejected, not wrapped");

    uint8_t d5[] = "18446744073709551616";
    DiscordSlice s5 = { d5, 20 };
    check(discord_etf_to_id(&s5) == 0, "one past UINT64_MAX rejected");

    uint8_t d3[] = "18446744073709551615";
    DiscordSlice s3 = { d3, 20 };
    check(discord_etf_to_id(&s3) == UINT64_MAX, "exactly UINT64_MAX still accepted");

    uint8_t d4[] = "00000000000000000001";
    DiscordSlice s4 = { d4, 20 };
    check(discord_etf_to_id(&s4) == 1, "leading zeros are not an overflow");

    uint8_t d1[] = "184467440737095517";
    DiscordSlice s1 = { d1, 18 };
    check(discord_etf_to_id(&s1) == 184467440737095517ULL,
          "18-digit value still parses correctly");
}

static void test_id_conversion(void)
{
    DiscordSlice s;
    uint8_t digits[] = "123456789012345678";
    s.ptr = digits;
    s.len = 18;
    check(discord_etf_to_id(&s) == 123456789012345678ULL, "snowflake u64");

    uint8_t bad[] = "12a4";
    s.ptr = bad;
    s.len = 4;
    check(discord_etf_to_id(&s) == 0, "non-digit snowflake rejected");
}

static void test_standard_small_int(void)
{
    uint8_t buf[64];
    size_t p = 0;
    put_u8(buf, &p, DISCORD_ETF_SMALL_MAP);
    put_u8(buf, &p, 1);
    put_str(buf, &p, "op");
    put_small_int(buf, &p, 10);

    DiscordEtfReader r;
    discord_etf_reader_init(&r, buf, p, 8);
    check(discord_etf_begin_map(&r) == DISCORD_ETF_OK, "small int: map opened");

    DiscordEtfField f;
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              discord_etf_slice_equals(&r.last_key, "op") &&
              f.kind == DISCORD_ETF_FIELD_INT && f.integer == 10,
          "small int: op 10 encoded as [97, 10] reads back as 10");
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              f.kind == DISCORD_ETF_FIELD_END,
          "small int: map end");
    check(discord_etf_end_map(&r) == DISCORD_ETF_OK, "small int: end_map");
    check(r.pos == p, "small int: cursor consumed exactly the payload");
}

static void test_standard_integer(void)
{
    uint8_t buf[64];
    size_t p = 0;
    put_u8(buf, &p, DISCORD_ETF_SMALL_MAP);
    put_u8(buf, &p, 1);
    put_str(buf, &p, "heartbeat_interval");
    put_int(buf, &p, 41250);

    DiscordEtfReader r;
    discord_etf_reader_init(&r, buf, p, 8);
    check(discord_etf_begin_map(&r) == DISCORD_ETF_OK, "integer: map opened");

    DiscordEtfField f;
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              discord_etf_slice_equals(&r.last_key, "heartbeat_interval") &&
              f.kind == DISCORD_ETF_FIELD_INT && f.integer == 41250,
          "integer: heartbeat_interval 41250 encoded as [98, 0x00, 0x00, 0xA1, 0x22] reads back as 41250");
    check(discord_etf_end_map(&r) == DISCORD_ETF_OK, "integer: end_map");
    check(r.pos == p, "integer: cursor consumed exactly the payload");
}

static void test_negative_integer(void)
{
    uint8_t buf[64];
    size_t p = 0;
    put_u8(buf, &p, DISCORD_ETF_SMALL_MAP);
    put_u8(buf, &p, 1);
    put_str(buf, &p, "code");
    put_int(buf, &p, -1);

    DiscordEtfReader r;
    discord_etf_reader_init(&r, buf, p, 8);
    check(discord_etf_begin_map(&r) == DISCORD_ETF_OK, "negative integer: map opened");

    DiscordEtfField f;
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              f.kind == DISCORD_ETF_FIELD_INT && f.integer == -1,
          "negative integer: -1 encoded as [98, 0xFF, 0xFF, 0xFF, 0xFF] reads back signed");
    check(discord_etf_end_map(&r) == DISCORD_ETF_OK, "negative integer: end_map");
    check(r.pos == p, "negative integer: cursor consumed exactly the payload");
}

static void test_standard_atom(void)
{
    uint8_t buf[64];
    size_t p = 0;
    put_u8(buf, &p, DISCORD_ETF_SMALL_MAP);
    put_u8(buf, &p, 1);
    put_str(buf, &p, "op");
    put_atom(buf, &p, "true");

    DiscordEtfReader r;
    discord_etf_reader_init(&r, buf, p, 8);
    check(discord_etf_begin_map(&r) == DISCORD_ETF_OK, "atom: map opened");

    DiscordEtfField f;
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              discord_etf_slice_equals(&r.last_key, "op") &&
              f.kind == DISCORD_ETF_FIELD_STRING &&
              discord_etf_slice_equals(&f.slice, "true"),
          "atom: true encoded as [100, 0x00, 0x04, t, r, u, e] reads back as STRING true");
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              f.kind == DISCORD_ETF_FIELD_END,
          "atom: map end");
    check(discord_etf_end_map(&r) == DISCORD_ETF_OK, "atom: end_map");
    check(r.pos == p, "atom: cursor consumed exactly the payload");
}

static void test_standard_small_atom(void)
{
    uint8_t buf[64];
    size_t p = 0;
    put_u8(buf, &p, DISCORD_ETF_SMALL_MAP);
    put_u8(buf, &p, 1);
    put_str(buf, &p, "op");
    put_small_atom(buf, &p, "true");

    DiscordEtfReader r;
    discord_etf_reader_init(&r, buf, p, 8);
    check(discord_etf_begin_map(&r) == DISCORD_ETF_OK, "small atom: map opened");

    DiscordEtfField f;
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              discord_etf_slice_equals(&r.last_key, "op") &&
              f.kind == DISCORD_ETF_FIELD_STRING &&
              discord_etf_slice_equals(&f.slice, "true"),
          "small atom: true encoded as [118, 4, t, r, u, e] reads back as STRING true");
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              f.kind == DISCORD_ETF_FIELD_END,
          "small atom: map end");
    check(discord_etf_end_map(&r) == DISCORD_ETF_OK, "small atom: end_map");
    check(r.pos == p, "small atom: cursor consumed exactly the payload");
}

static int parse_pairs(DiscordEtfReader *r, int expected)
{
    DiscordEtfField f;
    for (int i = 0; i < expected; i++) {
        char key[2] = { (char)('a' + (char)i), 0 };
        DiscordEtfStatus st = discord_etf_next_map_field(r, &f);
        if (st != DISCORD_ETF_OK ||
            !discord_etf_slice_equals(&r->last_key, key) ||
            f.kind != DISCORD_ETF_FIELD_INT || f.integer != i) {
            return i;
        }
    }
    return expected;
}

static void test_small_map_arity_17(void)
{
    uint8_t buf[256];
    size_t p = 0;
    put_u8(buf, &p, DISCORD_ETF_SMALL_MAP);
    put_u8(buf, &p, 17);
    for (int i = 0; i < 17; i++) {
        char key[2] = { (char)('a' + (char)i), 0 };
        put_str(buf, &p, key);
        put_small_int(buf, &p, (uint8_t)i);
    }

    DiscordEtfReader r;
    discord_etf_reader_init(&r, buf, p, 8);
    check(discord_etf_begin_map(&r) == DISCORD_ETF_OK, "arity17: map opened");

    DiscordEtfField f;
    check(parse_pairs(&r, 17) == 17,
          "arity17: small map with 17 pairs parses with arity 17");
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              f.kind == DISCORD_ETF_FIELD_END,
          "arity17: map end after 17 pairs");
    check(discord_etf_end_map(&r) == DISCORD_ETF_OK, "arity17: end_map");
    check(r.pos == p, "arity17: cursor consumed exactly the payload");
}

static void test_small_map_arity_16(void)
{
    uint8_t buf[256];
    size_t p = 0;
    put_u8(buf, &p, DISCORD_ETF_SMALL_MAP);
    put_u8(buf, &p, 16);
    for (int i = 0; i < 16; i++) {
        char key[2] = { (char)('a' + (char)i), 0 };
        put_str(buf, &p, key);
        put_small_int(buf, &p, (uint8_t)i);
    }

    DiscordEtfReader r;
    discord_etf_reader_init(&r, buf, p, 8);
    check(discord_etf_begin_map(&r) == DISCORD_ETF_OK, "arity16: map opened");

    DiscordEtfField f;
    check(parse_pairs(&r, 16) == 16,
          "arity16: plain 1-byte count, no extended count escape at 16");
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              f.kind == DISCORD_ETF_FIELD_END,
          "arity16: map end after 16 pairs");
    check(discord_etf_end_map(&r) == DISCORD_ETF_OK, "arity16: end_map");
    check(r.pos == p, "arity16: cursor consumed exactly the payload");
}

static void test_large_binary_refused(void)
{
    uint8_t buf[32];
    size_t p = 0;
    put_u8(buf, &p, DISCORD_ETF_SMALL_MAP);
    put_u8(buf, &p, 1);
    put_str(buf, &p, "file");
    put_u8(buf, &p, DISCORD_ETF_BINARY);
    put_u32(buf, &p, 0x00010000u);
    put_u8(buf, &p, 'x');

    DiscordEtfReader r;
    discord_etf_reader_init(&r, buf, p, 8);
    check(discord_etf_begin_map(&r) == DISCORD_ETF_OK, "big binary: map opened");

    DiscordEtfField f;
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_ERR_BAD_TAG,
          "big binary: 4-byte length above 65535 refused, not short-read");
}

static void test_floats_consumed(void)
{
    uint8_t buf[128];
    size_t p = 0;
    put_u8(buf, &p, DISCORD_ETF_SMALL_MAP);
    put_u8(buf, &p, 3);
    put_str(buf, &p, "x");
    put_u8(buf, &p, DISCORD_ETF_NEW_FLOAT);
    for (int i = 0; i < 8; i++) {
        put_u8(buf, &p, (uint8_t)(0x40 + i));
    }
    put_str(buf, &p, "y");
    put_u8(buf, &p, DISCORD_ETF_FLOAT);
    for (int i = 0; i < 31; i++) {
        put_u8(buf, &p, (uint8_t)('0' + i % 10));
    }
    put_str(buf, &p, "z");
    put_str(buf, &p, "ok");

    DiscordEtfReader r;
    discord_etf_reader_init(&r, buf, p, 8);
    check(discord_etf_begin_map(&r) == DISCORD_ETF_OK, "float: map opened");

    DiscordEtfField f;
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              f.kind == DISCORD_ETF_FIELD_NIL,
          "float: NEW_FLOAT 8-byte double consumed and surfaced as NIL");
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              f.kind == DISCORD_ETF_FIELD_NIL,
          "float: legacy FLOAT 31-byte ASCII consumed and surfaced as NIL");
    check(discord_etf_next_map_field(&r, &f) == DISCORD_ETF_OK &&
              f.kind == DISCORD_ETF_FIELD_STRING &&
              discord_etf_slice_equals(&f.slice, "ok"),
          "float: stream stays in sync after both float widths");
    check(discord_etf_end_map(&r) == DISCORD_ETF_OK, "float: end_map");
    check(r.pos == p, "float: cursor consumed exactly the payload");
}

int main(void)
{
    test_small_map();
    test_large_map();
    test_truncated();
    test_bad_tag();
    test_depth_limit();
    test_atom_truncated();
    test_huge_map_count();
    test_id_overflow();
    test_id_conversion();
    test_standard_small_int();
    test_standard_integer();
    test_negative_integer();
    test_standard_atom();
    test_standard_small_atom();
    test_small_map_arity_17();
    test_small_map_arity_16();
    test_large_binary_refused();
    test_floats_consumed();

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
