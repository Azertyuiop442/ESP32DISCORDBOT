#ifndef DISCORD_CORE_H
#define DISCORD_CORE_H

#include "discord_feature.h"
#include "discord_json.h"
#include "discord_slice.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISCORD_STATE_IDLE = 0,
    DISCORD_STATE_CONNECTING,
    DISCORD_STATE_HANDSHAKING,
    DISCORD_STATE_IDENTIFYING,
    DISCORD_STATE_READY,
    DISCORD_STATE_BACKOFF,
    DISCORD_STATE_FATAL
} DiscordConnectionState;

typedef struct {
    DiscordId guild_id;
    DiscordId log_channel_id;
    uint32_t  features;
    uint32_t  rate_limit_per_min;
    uint8_t   max_event_queue;
    uint8_t   captcha_ttl_minutes;
    uint8_t   captcha_max_attempts;
} DiscordBotConfig;

#ifdef __cplusplus
}
#endif

#endif