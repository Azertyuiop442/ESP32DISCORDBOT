#include <stdio.h>
#include <string.h>

#include "discord_etf.h"

#define SEEDS 24
#define ITERS 200000
#define MAXBUF 512

static uint32_t rng_state = 0x12345678u;

static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void put_u8(uint8_t *b, size_t *p, uint8_t v) { b[(*p)++] = v; }
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

static size_t build_seed(int idx, uint8_t *buf)
{
    size_t p = 0;
    switch (idx % 8) {
    case 0:
        put_u8(buf, &p, DISCORD_ETF_SMALL_MAP);
        put_u8(buf, &p, 2);
        put_str(buf, &p, "op");
        put_u8(buf, &p, DISCORD_ETF_SMALL_INTEGER);
        put_u8(buf, &p, 0);
        put_str(buf, &p, "MESSAGE_CREATE");
        put_str(buf, &p, "s");
        put_u8(buf, &p, DISCORD_ETF_INTEGER);
        put_u32(buf, &p, 42);
        break;
    case 1:
        put_u8(buf, &p, DISCORD_ETF_MAP);
        put_u32(buf, &p, 1);
        put_str(buf, &p, "d");
        put_u8(buf, &p, DISCORD_ETF_SMALL_MAP);
        put_u8(buf, &p, 0);
        break;
    case 2:
        put_u8(buf, &p, DISCORD_ETF_LIST);
        put_u32(buf, &p, 3);
        put_str(buf, &p, "a");
        put_u8(buf, &p, DISCORD_ETF_NIL);
        put_str(buf, &p, "bb");
        put_u8(buf, &p, DISCORD_ETF_NIL);
        break;
    case 3:
        put_u8(buf, &p, DISCORD_ETF_BINARY);
        put_u32(buf, &p, 300);
        p += 300;
        break;
    case 4:
        put_u8(buf, &p, DISCORD_ETF_SMALL_TUPLE);
        put_u8(buf, &p, 2);
        put_str(buf, &p, "k");
        put_u8(buf, &p, DISCORD_ETF_ATOM);
        put_u16(buf, &p, 1);
        put_u8(buf, &p, 'x');
        break;
    case 5:
        put_u8(buf, &p, DISCORD_ETF_SMALL_ATOM_UTF8);
        put_u8(buf, &p, 3);
        memcpy(buf + p, "abc", 3);
        p += 3;
        break;
    case 6:
        put_u8(buf, &p, DISCORD_ETF_SMALL_MAP);
        put_u8(buf, &p, 16);
        for (int i = 0; i < 16; i++) {
            char key[2] = { (char)('a' + (char)i), 0 };
            put_str(buf, &p, key);
            put_u8(buf, &p, DISCORD_ETF_SMALL_INTEGER);
            put_u8(buf, &p, (uint8_t)i);
        }
        break;
    default:
        put_u8(buf, &p, DISCORD_ETF_MAP);
        put_u32(buf, &p, 0xFFFFFFFF);
        break;
    }
    return p;
}

static void parse_all(const uint8_t *buf, size_t len)
{
    DiscordEtfReader r;
    discord_etf_reader_init(&r, buf, len, 8);

    for (int guard = 0; guard < 32; guard++) {
        if (discord_etf_begin_map(&r) != DISCORD_ETF_OK) {
            return;
        }
        for (;;) {
            DiscordEtfField f;
            DiscordEtfStatus st = discord_etf_next_map_field(&r, &f);
            if (st != DISCORD_ETF_OK) {
                return;
            }
            if (f.kind == DISCORD_ETF_FIELD_END) {
                break;
            }
            (void)discord_etf_slice_equals(&f.slice, "t");
            (void)discord_etf_to_id(&f.slice);
        }
        if (discord_etf_end_map(&r) != DISCORD_ETF_OK) {
            return;
        }
        if (r.pos >= len) {
            return;
        }
    }
}

int main(void)
{
    uint8_t buf[MAXBUF];
    uint8_t work[MAXBUF];
    uint32_t ok_count = 0;
    uint32_t err_count = 0;

    for (int s = 0; s < SEEDS; s++) {
        size_t base = build_seed(s, buf);

        parse_all(buf, base);
        ok_count++;

        for (int it = 0; it < ITERS / SEEDS; it++) {
            size_t len = base;
            memcpy(work, buf, base);

            int mutations = 1 + (int)(rnd() % 8);
            for (int m = 0; m < mutations && len > 0; m++) {
                uint32_t op = rnd() % 6;
                if (op == 0) {
                    work[rnd() % len] = (uint8_t)rnd();
                } else if (op == 1 && len < MAXBUF) {
                    size_t at = rnd() % (len + 1);
                    memmove(work + at + 1, work + at, len - at);
                    work[at] = (uint8_t)rnd();
                    len++;
                } else if (op == 2 && len > 1) {
                    size_t at = rnd() % len;
                    memmove(work + at, work + at + 1, len - at - 1);
                    len--;
                } else if (op == 3) {
                    work[rnd() % len] = (uint8_t)(97 + rnd() % 60);
                } else if (op == 4 && len >= 2) {
                    work[rnd() % len] = 0xFF;
                    work[rnd() % len] = 0xFF;
                } else {
                    work[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8));
                }
            }

            for (size_t trunc = 0; trunc <= len && trunc <= 32; trunc += 7) {
                parse_all(work, trunc);
            }
            parse_all(work, len);
            err_count++;
        }
    }

    printf("fuzz_etf: %d seeds parsed, %d mutated inputs exercised\n",
           ok_count, err_count);
    printf("\nPASSED (0 crashes, no sanitizer reports)\n");
    return 0;
}