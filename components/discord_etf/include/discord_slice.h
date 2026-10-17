#ifndef DISCORD_SLICE_H
#define DISCORD_SLICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *ptr;
    uint16_t len;
} DiscordSlice;

typedef uint64_t DiscordId;

#ifdef __cplusplus
}
#endif

#endif
