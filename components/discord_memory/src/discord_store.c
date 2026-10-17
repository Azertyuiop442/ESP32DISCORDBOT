#include "discord_store.h"

#include <string.h>

#define DC_STORE_MAGIC 0x44435354u

uint32_t dc_crc32(const void *data, size_t len, uint32_t seed)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

size_t dc_store_record_bytes(void)
{
    return sizeof(DcUserRecord);
}

size_t dc_store_flash_bytes(void)
{
    return sizeof(DcStoreHeader) +
           DC_STORE_SLOTS * (DC_STORE_RECORDS * sizeof(DcUserRecord));
}

static uint32_t record_crc(const DcUserRecord *rec)
{
    return dc_crc32(rec, DC_USER_RECORD_CRC_OFF, 0);
}

static bool record_valid(const DcUserRecord *rec)
{
    return rec->user_id != 0 && record_crc(rec) == rec->crc;
}

void dc_store_init(DcStore *s)
{
    memset(s, 0, sizeof(*s));
    s->count = 0;
    s->active_slot = 0;
}

static DcUserRecord *record_at(DcStore *s, uint16_t idx)
{
    return (DcUserRecord *)s->raw + idx;
}

static int find_index(const DcStore *s, DiscordId user_id)
{
    for (uint16_t i = 0; i < s->count; i++) {
        const DcUserRecord *r = (const DcUserRecord *)s->raw + i;
        if (record_valid(r) && r->user_id == user_id) {
            return (int)i;
        }
    }
    return -1;
}

DcStoreStatus dc_store_put(DcStore *s, const DcUserRecord *in)
{
    if (in->user_id == 0) {
        return DC_STORE_ERR_SLOT;
    }

    DcUserRecord rec = *in;
    rec.reserved = 0;
    rec.crc = record_crc(&rec);

    int idx = find_index(s, rec.user_id);
    if (idx < 0) {
        if (s->count >= DC_STORE_RECORDS) {
            return DC_STORE_ERR_FULL;
        }
        idx = (int)s->count;
        s->count++;
    }

    memcpy(record_at(s, (uint16_t)idx), &rec, sizeof(rec));
    return DC_STORE_OK;
}

DcStoreStatus dc_store_get(DcStore *s, DiscordId user_id, DcUserRecord *out)
{
    int idx = find_index(s, user_id);
    if (idx < 0) {
        return DC_STORE_ERR_NOT_FOUND;
    }
    memcpy(out, record_at(s, (uint16_t)idx), sizeof(*out));
    return DC_STORE_OK;
}

DcStoreStatus dc_store_delete(DcStore *s, DiscordId user_id)
{
    int idx = find_index(s, user_id);
    if (idx < 0) {
        return DC_STORE_ERR_NOT_FOUND;
    }
    for (uint16_t i = (uint16_t)idx; i + 1 < s->count; i++) {
        memcpy(record_at(s, i), record_at(s, i + 1), sizeof(DcUserRecord));
    }
    memset(record_at(s, s->count - 1), 0, sizeof(DcUserRecord));
    s->count--;
    return DC_STORE_OK;
}
