#include "discord_xp_curve.h"

uint32_t dc_xp_required_for_level(uint8_t level)
{
    if (level == 0u) {
        return 0u;
    }
    if (level > DC_XP_MAX_LEVEL) {
        level = (uint8_t)DC_XP_MAX_LEVEL;
    }
    uint32_t l = (uint32_t)level;
    return 50u * l * (l + 1u);
}

uint8_t dc_xp_level_for_xp(uint32_t xp)
{
    uint8_t low = 0u;
    uint8_t high = (uint8_t)DC_XP_MAX_LEVEL;
    uint8_t result = 0u;

    while (low <= high) {
        uint8_t mid = (uint8_t)(low + (high - low) / 2u);
        uint32_t req = dc_xp_required_for_level(mid);

        if (req <= xp) {
            result = mid;
            if (mid == (uint8_t)DC_XP_MAX_LEVEL) {
                break;
            }
            low = (uint8_t)(mid + 1u);
        } else {
            if (mid == 0u) {
                break;
            }
            high = (uint8_t)(mid - 1u);
        }
    }
    return result;
}

uint32_t dc_xp_calc_gain(uint32_t seed)
{
    uint32_t range = (DC_XP_GAIN_MAX - DC_XP_GAIN_MIN) + 1u;
    return DC_XP_GAIN_MIN + (seed % range);
}

bool dc_xp_check_level_up(uint32_t current_xp, uint32_t gained_xp,
                          uint8_t current_level, uint8_t *out_new_level)
{
    uint32_t new_xp = current_xp + gained_xp;
    uint8_t new_level = dc_xp_level_for_xp(new_xp);

    if (new_level > current_level) {
        if (out_new_level != NULL) {
            *out_new_level = new_level;
        }
        return true;
    }
    if (out_new_level != NULL) {
        *out_new_level = current_level;
    }
    return false;
}
