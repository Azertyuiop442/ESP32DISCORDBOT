#include <stdio.h>
#include <string.h>

#include "discord_store.h"

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

static DcUserRecord mk(DiscordId id, uint32_t xp)
{
    DcUserRecord r;
    memset(&r, 0, sizeof(r));
    r.user_id = id;
    r.xp = xp;
    r.last_msg = 1234567;
    r.day_key = 296;
    r.level = 7;
    r.flags = DC_USER_FLAG_VERIFIED;
    r.mute_count = 2;
    return r;
}

static void test_sizes(void)
{
    printf("     record = %zu bytes\n", dc_store_record_bytes());
    printf("     256 records x 2 slots (A/B) + header = %zu bytes (%.2f KiB)\n",
           dc_store_flash_bytes(), (double)dc_store_flash_bytes() / 1024.0);
    check(dc_store_record_bytes() == 32, "record is exactly 32 bytes");
    check(dc_store_flash_bytes() < 20000, "whole store under 20 KiB");
}

static void test_crc(void)
{
    check(dc_crc32("123456789", 9, 0) == 0xCBF43926u, "crc32 matches known vector");
    check(dc_crc32("", 0, 0) == 0u, "crc32 of empty is zero");
}

static void test_put_get(void)
{
    DcStore s;
    dc_store_init(&s);

    check(dc_store_put(&s, (DcUserRecord[]) { { .user_id = 111, .xp = 500 } }) ==
              DC_STORE_OK, "put first record");
    check(dc_store_put(&s, (DcUserRecord[]) { { .user_id = 222, .xp = 900 } }) ==
              DC_STORE_OK, "put second record");

    DcUserRecord out;
    check(dc_store_get(&s, 111, &out) == DC_STORE_OK && out.xp == 500, "get first");
    check(dc_store_get(&s, 222, &out) == DC_STORE_OK && out.xp == 900, "get second");
    check(dc_store_get(&s, 333, &out) == DC_STORE_ERR_NOT_FOUND, "missing key");
}

static void test_update(void)
{
    DcStore s;
    dc_store_init(&s);

    DcUserRecord a = mk(111, 100);
    dc_store_put(&s, &a);

    a.xp = 250;
    a.level = 3;
    check(dc_store_put(&s, &a) == DC_STORE_OK, "update existing");

    DcUserRecord out;
    check(dc_store_get(&s, 111, &out) == DC_STORE_OK, "still found after update");
    check(out.xp == 250 && out.level == 3, "update applied");
    check(out.day_key == 296 && out.mute_count == 2, "other fields preserved");
    check(s.count == 1, "update did not add a record");
}

static void test_capacity(void)
{
    DcStore s;
    dc_store_init(&s);

    for (uint32_t i = 1; i <= DC_STORE_RECORDS; i++) {
        DcUserRecord r = mk(i, i * 10);
        if (dc_store_put(&s, &r) != DC_STORE_OK) {
            check(0, "store filled before capacity");
            return;
        }
    }
    check(s.count == DC_STORE_RECORDS, "store holds full capacity");

    DcUserRecord overflow = mk(9999, 1);
    check(dc_store_put(&s, &overflow) == DC_STORE_ERR_FULL, "rejects overflow");

    DcUserRecord out;
    check(dc_store_get(&s, DC_STORE_RECORDS, &out) == DC_STORE_OK &&
              out.xp == DC_STORE_RECORDS * 10,
          "last record intact after failed overflow");
}

static void test_delete(void)
{
    DcStore s;
    dc_store_init(&s);

    for (int i = 1; i <= 5; i++) {
        DcUserRecord r = mk((DiscordId)i, (uint32_t)(i * 100));
        dc_store_put(&s, &r);
    }

    check(dc_store_delete(&s, 3) == DC_STORE_OK, "delete middle record");
    check(s.count == 4, "count decremented");

    DcUserRecord out;
    check(dc_store_get(&s, 3, &out) == DC_STORE_ERR_NOT_FOUND, "deleted key gone");
    check(dc_store_get(&s, 4, &out) == DC_STORE_OK && out.xp == 400, "record 4 shifted");
    check(dc_store_get(&s, 5, &out) == DC_STORE_OK && out.xp == 500, "record 5 intact");
    check(dc_store_delete(&s, 99) == DC_STORE_ERR_NOT_FOUND, "delete missing key");
}

static void test_corruption_detected(void)
{
    DcStore s;
    dc_store_init(&s);

    DcUserRecord r = mk(111, 500);
    dc_store_put(&s, &r);

    DcUserRecord *raw = (DcUserRecord *)s.raw;
    raw[0].xp = 999999;

    DcUserRecord out;
    check(dc_store_get(&s, 111, &out) == DC_STORE_ERR_NOT_FOUND,
          "corrupted record is not returned");
}

static void test_zero_id_rejected(void)
{
    DcStore s;
    dc_store_init(&s);

    DcUserRecord z = mk(0, 10);
    check(dc_store_put(&s, &z) == DC_STORE_ERR_SLOT, "zero user_id rejected");
    check(s.count == 0, "nothing stored for zero id");
}

int main(void)
{
    test_sizes();
    test_crc();
    test_put_get();
    test_update();
    test_capacity();
    test_delete();
    test_corruption_detected();
    test_zero_id_rejected();

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
