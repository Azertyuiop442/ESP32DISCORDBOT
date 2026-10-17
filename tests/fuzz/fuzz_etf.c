#define DC_FUZZ_BUILD

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "discord_etf.h"

static void walk(DiscordEtfReader *r)
{
    for (;;) {
        DiscordEtfStatus st = discord_etf_begin_map(r);
        if (st == DISCORD_ETF_OK) {
            for (;;) {
                DiscordEtfField f;
                st = discord_etf_next_map_field(r, &f);
                if (st != DISCORD_ETF_OK || f.kind == DISCORD_ETF_FIELD_END) {
                    break;
                }
                if (f.kind == DISCORD_ETF_FIELD_STRING) {
                    (void)discord_etf_slice_equals(&f.slice, "t");
                } else if (f.kind == DISCORD_ETF_FIELD_INT) {
                    (void)discord_etf_to_id(&f.slice);
                }
            }
            discord_etf_end_map(r);
            return;
        }
        if (st == DISCORD_ETF_ERR_TRUNCATED || st == DISCORD_ETF_ERR_BAD_TAG ||
            st == DISCORD_ETF_ERR_CONTAINER) {
            return;
        }
        if (st == DISCORD_ETF_ERR_DEPTH) {
            return;
        }
        return;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > 4096) {
        return 0;
    }

    DiscordEtfReader r;
    discord_etf_reader_init(&r, data, size, 16);
    walk(&r);

    for (size_t len = 0; len <= size && len <= 64; len++) {
        DiscordEtfReader r2;
        discord_etf_reader_init(&r2, data, len, 4);
        walk(&r2);
    }

    return 0;
}