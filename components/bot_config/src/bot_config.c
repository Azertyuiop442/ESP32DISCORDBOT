/**
 * @file bot_config.c
 * @brief Validates bot_config.h at startup and reports the resulting footprint.
 */

#include "bot_config.h"

#include "discord_jobq.h"
#include "discord_ratelimit.h"
#include "discord_state.h"

typedef struct {
    const char *label;
    unsigned    value;
    unsigned    min;
    unsigned    max;
} DcRange;

static const DcRange CHECKS[] = {
    { "DC_RANK_SLOTS",         DC_RANK_SLOTS,         1,    4096 },
    { "DC_SPAM_SLOTS",         DC_SPAM_SLOTS,         1,    1024 },
    { "DC_SPAM_WINDOW",        DC_SPAM_WINDOW,        1,    64   },
    { "DC_TICKET_SLOTS",       DC_TICKET_SLOTS,       1,    256  },
    { "DC_CAPTCHA_SLOTS",      DC_CAPTCHA_SLOTS,      1,    64   },
    { "DC_JOBQ_SLOTS",         DC_JOBQ_SLOTS,         1,    128  },
    { "DC_GW_RX_BUFFER",       DC_GW_RX_BUFFER,        256,  16384 },
    { "DC_REST_TX_BUFFER",     DC_REST_TX_BUFFER,      256,  16384 },
    { "DC_REST_RX_BUFFER",     DC_REST_RX_BUFFER,      256,  16384 },
    { "DC_CAPTCHA_TTL_SEC",    DC_CAPTCHA_TTL_SEC,     10,   900  },
    { "DC_CAPTCHA_ATTEMPTS",   DC_CAPTCHA_ATTEMPTS,    1,    10   },
    { "DC_CAPTCHA_CODE_LEN",   DC_CAPTCHA_CODE_LEN,    3,    15   },
    { "DC_RATE_LIMIT_PER_MIN", DC_RATE_LIMIT_PER_MIN,  1,    60   },
    { "DC_XP_PER_MESSAGE",     DC_XP_PER_MESSAGE,      1,    500  },
    { "DC_IDENTIFY_DAILY_MAX", DC_IDENTIFY_DAILY_MAX,  1,    1000 },
};

#define CHECK_COUNT (sizeof(CHECKS) / sizeof(CHECKS[0]))

unsigned dc_config_error_count(void);
unsigned dc_config_projected_ram(void);

unsigned dc_config_error_count(void)
{
    unsigned errors = 0;
    for (unsigned i = 0; i < CHECK_COUNT; i++) {
        if (CHECKS[i].value < CHECKS[i].min || CHECKS[i].value > CHECKS[i].max) {
            errors++;
        }
    }
    if (DC_CAPTCHA_TTL_SEC > 900) {
        errors++;
    }
    return errors;
}

/**
 * RAM the configuration implies, computed at compile time from the same
 * constants the framework uses. No allocator is involved anywhere: every
 * table is a fixed array sized by a #define.
 */
unsigned dc_config_projected_ram(void)
{
    return (unsigned)sizeof(DcBotState) + (unsigned)sizeof(DcJobQueue) +
           (unsigned)sizeof(DcRateLimiter) + DC_GW_RX_BUFFER + DC_REST_TX_BUFFER +
           DC_REST_RX_BUFFER;
}

unsigned dc_config_checks_run(void);
unsigned dc_config_checks_run(void)
{
    return (unsigned)CHECK_COUNT;
}

const char *dc_config_check_label(unsigned index);
unsigned dc_config_check_value(unsigned index);

const char *dc_config_check_label(unsigned index)
{
    return index < CHECK_COUNT ? CHECKS[index].label : "";
}

unsigned dc_config_check_value(unsigned index)
{
    return index < CHECK_COUNT ? CHECKS[index].value : 0;
}