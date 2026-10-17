/**
 * @file bot_config.h
 * @brief Single place to configure a bot instance.
 *
 * This is the file you edit. Everything here is compiled into flash and is
 * public. NEVER put a bot token, Wi-Fi password or any other secret in it:
 * it would be committed to Git and shipped inside the firmware image, which
 * violates the Discord Developer Terms.
 *
 * Secrets are provisioned over the serial console on first boot and stored in
 * encrypted NVS. That provisioning code is NOT written yet: there is no
 * `dc_security_provision()` in this repository. Until it exists, the token has
 * no home on the device. See docs/12_security_audit.md.
 *
 * All values are checked at startup by `dc_config_error_count()`, which
 * refuses to run on an inconsistent configuration instead of failing later on
 * the Gateway task.
 *
 * KNOWN LIMITATION: the memory bounds below are documented intent, not wiring.
 * Each is mirrored by a constant in the owning component header. Editing
 * DC_RANK_SLOTS here does NOT resize the framework arrays. See
 * docs/12_security_audit.md, finding "config does not configure".
 */

#ifndef BOT_CONFIG_H
#define BOT_CONFIG_H

/* ------------------------------------------------------------------ *
 * Identity
 * ------------------------------------------------------------------ */

/** Discord application ID. Public by nature, but keep it out of Git anyway. */
#define DC_APP_ID              0ULL

/** Guild this bot serves. 0 disables guild-scoped behaviour. */
#define DC_GUILD_ID            0ULL

/** Channel for operational logs. 0 disables logging. */
#define DC_LOG_CHANNEL_ID      0ULL

/** Channel for user-facing announcements. 0 disables announcements. */
#define DC_ANNOUNCE_CHANNEL_ID 0ULL

/* ------------------------------------------------------------------ *
 * Features
 *
 * Each feature costs tens of bytes and nothing else. Enable only what the
 * instance actually uses: unused features are removed by the linker.
 * ------------------------------------------------------------------ */

/** /profile, /rank, /leaderboard. Needs no privileged intent. */
#define DC_FEATURE_RANKS       1

/** /bug, /feature. Reports live in Discord; the ESP32 keeps only IDs. */
#define DC_FEATURE_TICKETS     1

/** /verify. Modal text input. Needs no intent at all. */
#define DC_FEATURE_CAPTCHA     1

/**
 * Anti-spam on message text.
 *
 * REQUIRES the MESSAGE_CONTENT privileged intent, enabled in the Discord
 * Developer Portal. Without it, message content arrives empty and this
 * feature silently does nothing. Prefer interaction-based spam detection,
 * which needs no privileged intent.
 */
#define DC_FEATURE_SPAM        0

/* ------------------------------------------------------------------ *
 * Memory budgets
 *
 * These are compile-time bounds, not runtime tunables. Every table below is
 * a fixed array: there is no allocator involved, so the cost is known before
 * the chip boots and cannot grow under load.
 * ------------------------------------------------------------------ */

/** Ranked users held in RAM. 16 B each. */
#define DC_RANK_SLOTS          128

/** Users in the sliding spam window. 56 B each. */
#define DC_SPAM_SLOTS          32

/** Message hashes retained per user, for repeat detection. */
#define DC_SPAM_WINDOW         8

/** Open tickets held in RAM. 40 B each. Persist to flash if reports must
 *  survive a reboot. */
#define DC_TICKET_SLOTS        16

/** Concurrent unresolved CAPTCHAs. 56 B each. */
#define DC_CAPTCHA_SLOTS       8

/** Queued outbound actions. 80 B each. */
#define DC_JOBQ_SLOTS          16

/** WebSocket receive buffer. Must exceed one ETF frame header comfortably. */
#define DC_GW_RX_BUFFER        4096

/** REST transmit buffer. Bounds the largest request we can build. */
#define DC_REST_TX_BUFFER      2048

/** REST receive buffer. */
#define DC_REST_RX_BUFFER      1024

/* ------------------------------------------------------------------ *
 * Feature tuning
 * ------------------------------------------------------------------ */

/** Seconds a user has to answer a CAPTCHA. Discord caps the window at 15 min. */
#define DC_CAPTCHA_TTL_SEC     300

/** Wrong answers tolerated before the challenge is burned. A CAPTCHA that
 *  punishes a single typo is a bad CAPTCHA. */
#define DC_CAPTCHA_ATTEMPTS    3

/** Characters in a generated CAPTCHA code. */
#define DC_CAPTCHA_CODE_LEN    6

/** Per-user action ceiling, enforced by the job queue. */
#define DC_RATE_LIMIT_PER_MIN  5

/** Maximum XP awarded per message, so XP grinding stays bounded. */
#define DC_XP_PER_MESSAGE     15

/* ------------------------------------------------------------------ *
 * Safety
 * ------------------------------------------------------------------ */

/**
 * Refuse to IDENTIFY when this many attempts were used in 24 h.
 * Discord revokes the token above 1000. Do not raise it.
 */
#define DC_IDENTIFY_DAILY_MAX  1000

/** Serial output verbosity. 0 quiet, 1 normal, 2 debug. */
#define DC_LOG_LEVEL           1

#endif /* BOT_CONFIG_H */
