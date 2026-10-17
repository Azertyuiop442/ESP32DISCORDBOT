#include "discord_identify.h"

#include <string.h>

#include "discord_gateway.h"

static bool window_expired(const DcIdentify *id, uint64_t now_ms)
{
    if (!id->window_open || now_ms <= id->window_start_ms) {
        return false;
    }

    return now_ms - id->window_start_ms >= DC_IDENTIFY_WINDOW_MS;
}

void dc_identify_init(DcIdentify *id, const DcIdentifyStore *store)
{
    if (id == NULL) {
        return;
    }

    memset(id, 0, sizeof(*id));
    if (store != NULL) {
        id->store = *store;
    }
    id->loaded = false;
    id->window_open = false;
}

bool dc_identify_load(DcIdentify *id)
{
    uint32_t count = 0;
    uint64_t window_start_ms = 0;

    if (id == NULL || id->store.read == NULL) {
        return false;
    }

    id->loaded = false;
    id->window_open = false;
    id->count = 0;
    id->window_start_ms = 0;

    if (id->store.read(id->store.ctx, &count, &window_start_ms) != 0) {
        return false;
    }

    id->count = count;
    id->window_start_ms = window_start_ms;
    id->loaded = true;
    id->window_open = window_start_ms != 0;

    return true;
}

bool dc_identify_may_send(const DcIdentify *id, uint64_t now_ms)
{
    if (id == NULL || !id->loaded) {
        return false;
    }
    if (!id->window_open || window_expired(id, now_ms)) {
        return true;
    }
    if (now_ms < id->window_start_ms) {
        return false;
    }

    return id->count < DC_GW_IDENTIFY_DAILY_LIMIT;
}

bool dc_identify_record(DcIdentify *id, uint64_t now_ms)
{
    if (id == NULL || !id->loaded) {
        return false;
    }

    if (!id->window_open) {
        id->count = 1;
        id->window_start_ms = now_ms;
        id->window_open = true;
    } else if (window_expired(id, now_ms)) {
        id->count = 1;
        id->window_start_ms = now_ms;
    } else if (id->count < DC_GW_IDENTIFY_DAILY_LIMIT) {
        id->count++;
    }

    if (id->store.write == NULL) {
        return false;
    }

    return id->store.write(id->store.ctx, id->count, id->window_start_ms) == 0;
}

uint32_t dc_identify_used(const DcIdentify *id)
{
    return id == NULL ? 0u : id->count;
}

uint64_t dc_identify_window_start(const DcIdentify *id)
{
    return id == NULL ? 0u : id->window_start_ms;
}

size_t dc_identify_footprint(void)
{
    return sizeof(DcIdentify);
}
