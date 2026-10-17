#ifndef DISCORD_STORE_H
#define DISCORD_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "discord_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DC_STORE_RECORDS 256
#define DC_STORE_SLOTS 2

typedef struct {
    DiscordId user_id;
    uint32_t xp;
    uint32_t last_msg;
    uint16_t day_key;
    uint8_t level;
    uint8_t flags;
    uint8_t mute_count;
    uint8_t reserved;
    uint32_t crc;
} DcUserRecord;

#define DC_USER_RECORD_CRC_OFF offsetof(DcUserRecord, crc)

#define DC_USER_FLAG_VERIFIED 0x01
#define DC_USER_FLAG_MUTED 0x02
#define DC_USER_FLAG_SPAM 0x04

typedef enum {
    DC_STORE_OK = 0,
    DC_STORE_ERR_FULL = -1,
    DC_STORE_ERR_NOT_FOUND = -2,
    DC_STORE_ERR_CORRUPT = -3,
    DC_STORE_ERR_SLOT = -4
} DcStoreStatus;

typedef struct {
    uint32_t magic;
    uint16_t count;
    uint16_t reserved;
    uint32_t crc;
} DcStoreHeader;

typedef struct {
    uint8_t raw[DC_STORE_RECORDS * sizeof(DcUserRecord)];
    uint16_t count;
    uint8_t active_slot;
} DcStore;

uint32_t dc_crc32(const void *data, size_t len, uint32_t seed);

void dc_store_init(DcStore *s);
DcStoreStatus dc_store_put(DcStore *s, const DcUserRecord *rec);
DcStoreStatus dc_store_get(DcStore *s, DiscordId user_id, DcUserRecord *out);
DcStoreStatus dc_store_delete(DcStore *s, DiscordId user_id);
size_t dc_store_flash_bytes(void);
size_t dc_store_record_bytes(void);

#ifdef __cplusplus
}
#endif

#endif
