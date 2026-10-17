#include "discord_ratelimit.h"

#include <string.h>

void dc_ratelimit_init(DcRateLimiter *rl, uint64_t now_ms)
{
    memset(rl, 0, sizeof(*rl));
    rl->second_window_start_ms = (uint32_t)now_ms;
    rl->minute_window_start_ms = (uint32_t)now_ms;
    rl->global_until_ms = 0;
    rl->invalid_window_index = (uint32_t)(now_ms / DC_RATELIMIT_INVALID_WINDOW_MS);
    rl->invalid_count = 0;
}

static uint32_t invalid_window_index_of(uint64_t now_ms)
{
    return (uint32_t)(now_ms / DC_RATELIMIT_INVALID_WINDOW_MS);
}

static bool invalid_window_current(const DcRateLimiter *rl, uint64_t now_ms)
{
    return rl->invalid_window_index == invalid_window_index_of(now_ms);
}

static DcRouteBucket *route_for(DcRateLimiter *rl, uint32_t route_hash)
{
    return &rl->routes[route_hash % DC_ROUTE_BUCKETS];
}

static bool route_blocked(const DcRouteBucket *b, uint64_t now_ms)
{
    if (!b->info.present || b->info.remaining != 0) {
        return false;
    }
    return now_ms < b->observed_at_ms + b->info.reset_after_ms;
}

bool dc_ratelimit_global_blocked(const DcRateLimiter *rl, uint64_t now_ms)
{
    return rl->global_blocked && now_ms < rl->global_until_ms;
}

DcWaitResult dc_ratelimit_rest(DcRateLimiter *rl, uint32_t route_hash, uint64_t now_ms)
{
    if (dc_ratelimit_global_blocked(rl, now_ms)) {
        return DC_WAIT_THROTTLED;
    }

    if ((uint32_t)(now_ms - rl->second_window_start_ms) >= 1000u) {
        rl->second_window_start_ms = (uint32_t)now_ms;
        rl->second_count = 0;
    }

    DcRouteBucket *b = route_for(rl, route_hash);
    if (route_blocked(b, now_ms)) {
        return DC_WAIT_THROTTLED;
    }

    if (rl->second_count >= DC_GLOBAL_LIMIT_PER_SEC) {
        return DC_WAIT_THROTTLED;
    }

    rl->second_count++;
    if (b->info.present && b->info.remaining != UINT32_MAX && b->info.remaining > 0) {
        b->info.remaining--;
    }
    return DC_WAIT_OK;
}

DcWaitResult dc_ratelimit_interaction(DcRateLimiter *rl, uint32_t route_hash,
                                      uint64_t now_ms)
{
    DcRouteBucket *b = route_for(rl, route_hash);
    if (route_blocked(b, now_ms)) {
        return DC_WAIT_THROTTLED;
    }

    if (b->info.present && b->info.remaining != UINT32_MAX && b->info.remaining > 0) {
        b->info.remaining--;
    }
    return DC_WAIT_OK;
}

DcWaitResult dc_ratelimit_gateway(DcRateLimiter *rl, uint64_t now_ms)
{
    if ((uint32_t)(now_ms - rl->minute_window_start_ms) >= 60000u) {
        rl->minute_window_start_ms = (uint32_t)now_ms;
        rl->minute_count = 0;
    }
    if (rl->minute_count >= DC_GATEWAY_LIMIT_PER_MIN) {
        return DC_WAIT_THROTTLED;
    }
    rl->minute_count++;
    return DC_WAIT_OK;
}

void dc_ratelimit_on_headers(DcRateLimiter *rl, uint32_t route_hash,
                             const DcRateLimitInfo *info, uint64_t now_ms)
{
    DcRouteBucket *b = route_for(rl, route_hash);
    b->info = *info;
    b->observed_at_ms = now_ms;

    if (info->global) {
        rl->global_blocked = true;
        rl->global_until_ms = now_ms + info->reset_after_ms;
    }
}

bool dc_ratelimit_invalid_tripped(const DcRateLimiter *rl, uint64_t now_ms)
{
    if (!invalid_window_current(rl, now_ms)) {
        return false;
    }
    return rl->invalid_count >= DC_RATELIMIT_INVALID_MAX;
}

uint32_t dc_ratelimit_invalid_count(const DcRateLimiter *rl, uint64_t now_ms)
{
    if (!invalid_window_current(rl, now_ms)) {
        return 0;
    }
    return rl->invalid_count;
}

bool dc_ratelimit_note_invalid(DcRateLimiter *rl, uint64_t now_ms)
{
    if (!invalid_window_current(rl, now_ms)) {
        rl->invalid_window_index = invalid_window_index_of(now_ms);
        rl->invalid_count = 0;
    }

    if (rl->invalid_count < DC_RATELIMIT_INVALID_MAX) {
        rl->invalid_count++;
    }
    return rl->invalid_count >= DC_RATELIMIT_INVALID_MAX;
}

void dc_ratelimit_on_429(DcRateLimiter *rl, uint32_t route_hash,
                         uint32_t retry_after_ms, uint64_t now_ms)
{
    DcRouteBucket *b = route_for(rl, route_hash);
    b->info.present = true;
    b->info.remaining = 0;
    b->info.reset_after_ms = retry_after_ms;
    b->info.shared = true;
    b->observed_at_ms = now_ms;
}

uint32_t dc_ratelimit_wait_ms(DcRateLimiter *rl, uint32_t route_hash, uint64_t now_ms)
{
    uint32_t wait = 0;

    if (dc_ratelimit_global_blocked(rl, now_ms)) {
        uint64_t left = rl->global_until_ms - now_ms;
        wait = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
    }

    DcRouteBucket *b = route_for(rl, route_hash);
    if (route_blocked(b, now_ms)) {
        uint64_t left = b->observed_at_ms + b->info.reset_after_ms - now_ms;
        uint32_t left32 = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
        wait = wait > left32 ? wait : left32;
    }

    return wait;
}