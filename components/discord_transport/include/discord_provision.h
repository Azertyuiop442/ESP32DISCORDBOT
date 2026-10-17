#ifndef DISCORD_PROVISION_H
#define DISCORD_PROVISION_H

#include <esp_err.h>

#include "discord_identify.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * NVS namespace every key owned by the transport lives in.
 *
 * One namespace keeps the token and the IDENTIFY budget together,
 * where an operator can inspect them with the NVS partition tool,
 * and keeps this component's keys from colliding with any other
 * NVS user in the same firmware. The name is seven characters
 * because an NVS namespace name, like an NVS key name, is limited
 * to 15 characters: the name is stored in a fixed-size field of
 * the on-flash entry, and the API answers a longer name with
 * ESP_ERR_NVS_INVALID_NAME.
 */
#define DC_NVS_NAMESPACE "discord"

/**
 * Longest Wi-Fi credential the prompt accepts, in characters.
 *
 * An SSID is at most 32 characters and a WPA password at
 * most 64, the field sizes of wifi_config_t, so the prompt
 * rejects anything longer rather than storing a value the
 * station interface would truncate. Callers size their
 * buffers as the constant plus one for the NUL.
 */
#define DC_PROVISION_SSID_LEN_MAX 32u
#define DC_PROVISION_PASS_LEN_MAX 64u

/**
 * First-boot token provisioning over the serial console.
 *
 * WHY THE TOKEN IS NOT IN THE FIRMWARE: a token compiled into the
 * image is shipped in every flash copy, in every build artifact
 * and in the Git history of the machine that built it, and
 * Discord's Developer Terms forbid embedding a bot token in a
 * client. The only channel that never writes the token into the
 * repository or the image is the serial console, so the first
 * boot after a chip erase asks an operator to paste it there.
 * After that one boot the token lives only in NVS, which a
 * routine idf.py flash does not touch, so reflashing never asks
 * again.
 *
 * THE PROMPT NEVER EXITS: the documented recovery for a device
 * that cannot store anything is a chip erase, and a chip erase
 * resets the IDENTIFY counter, so exiting the prompt on a storage
 * error buys nothing and costs quota. A failed token write
 * therefore stays in the prompt and retries with bounded backoff,
 * doubling from half a second up to thirty seconds and no further,
 * for as long as the device is powered. The one storage failure
 * that IS fatal at boot is the IDENTIFY counter write: running
 * unidentified spends a quota the device can no longer account
 * for, so a false return from dc_identify_load() or
 * dc_identify_record() over this store is fatal for the gateway,
 * and discord_nvs.c logs the failed counter write with a distinct
 * message so the operator can tell it from every other boot
 * failure.
 *
 * AN UNREADABLE TOKEN RE-PROMPTS: a token that is present but
 * unreadable, for instance after a rotation of the flash
 * encryption keys, can be neither used nor repaired from
 * software. Treating it as absent and provisioning again is the
 * only repair that does not need a chip erase, so it is never
 * fatal.
 *
 * THE TOKEN IS NEVER PRINTED, not even redacted, and never
 * logged. dc_redact() keeps the first four characters, and the
 * first four characters of a Discord token encode part of the
 * application id, so even a redacted token is a small leak. No
 * printf, ESP_LOGI or error path in this component sees the
 * token's content: failures are reported as numeric codes and
 * shape codes only. The console echo of what the operator types
 * is the terminal's own behavior and the one place the token ever
 * appears, which is why the whole protocol is one prompt and one
 * line.
 *
 * CONSOLE: one line is read from stdin, which the ESP-IDF VFS
 * maps to whichever console the sdkconfig selected, so no UART
 * number is hard-coded here and the same code serves the default
 * UART console and a CDC console. The read blocks until the
 * operator presses Enter. A line longer than the internal buffer
 * is rejected, not stored truncated: the tail of an over-long
 * paste is drained first, so it cannot be mistaken for a second,
 * shorter entry, and a truncated prefix that still looks like a
 * token is never written to NVS.
 *
 * RESTART: a successful store is followed by esp_restart(), which
 * never returns. The reboot is deliberate: the boot that follows
 * reads the token back from NVS and thereby proves the write
 * landed, instead of trusting a RAM copy written seconds ago.
 *
 * RETURNS: ESP_OK when a usable token is already stored, checked
 * before any prompt is printed. Otherwise the call blocks, and the
 * only exit from the prompt is a successful store followed by
 * esp_restart(), so no other return value exists.
 *
 * THREADING: call once from app_main, before any task that needs
 * the token. The module holds no state and uses no RTOS primitive
 * of its own apart from the backoff delay.
 */
esp_err_t dc_provision_ensure_token(void);

/**
 * First-boot Wi-Fi provisioning over the same serial console.
 *
 * WHY THE CREDENTIALS ARE NOT IN THE FIRMWARE: a Wi-Fi
 * password compiled into the image ships in every flash copy
 * and every build artifact, and docs/11_configuration.md
 * puts every secret on the serial-to-NVS path instead: the
 * image carries no secret, and the credentials are
 * reconfigurable without a reflash. The token and the
 * Wi-Fi credentials therefore share one rule and one
 * prompt flow, and .env.example documents the two values
 * the builder pipes into this prompt at first boot.
 *
 * THE PROMPT: the SSID first, then the password. An empty
 * password line is accepted on purpose, because an open
 * network has no password; the empty line is the only way
 * to say so. An empty SSID is not accepted, no network has
 * an empty SSID, and a line longer than the matching buffer
 * is rejected whole rather than stored truncated, the same
 * discipline as the token prompt.
 *
 * BOTH KEYS OR NEITHER: "wifi_ssid" and "wifi_pass" are
 * written and read as a pair inside DC_NVS_NAMESPACE. A
 * torn write that lands only the SSID reads as absent, so
 * the device re-prompts, which is the safe direction: a
 * password-less pair can never be mistaken for an open
 * network, because the read requires both keys and the
 * operator explicitly chose the password.
 *
 * RESTART: like the token, a successful store is followed by
 * esp_restart(), which never returns. The reboot reads the
 * pair back from NVS and thereby proves the commit landed,
 * instead of trusting RAM copies written seconds ago. The
 * values the caller asked for are therefore only ever
 * returned on the path where they already live in NVS.
 *
 * THE PASSWORD IS NEVER PRINTED AND NEVER LOGGED, in any
 * path, not even redacted: unlike the token it has no
 * prefix that encodes an application id, so any echo is a
 * plain secret leak. Failures are reported as numeric codes
 * only, and the console echo of what the operator types is
 * the terminal's own behavior.
 *
 * RETURNS: ESP_OK with the credentials copied into the
 * caller's buffers when both are already stored, checked
 * before any prompt is printed. ESP_ERR_INVALID_ARG for a
 * NULL buffer or a zero capacity. Otherwise the call blocks
 * in the prompt, and the only exit is a successful store
 * followed by esp_restart(), so no other return value
 * exists. Storage errors stay in the prompt with the same
 * bounded backoff as the token: the documented recovery for
 * a device that cannot store anything is a chip erase, so
 * exiting the prompt buys nothing.
 *
 * THREADING: call once from app_main, after
 * dc_provision_ensure_token() and before the Wi-Fi station
 * is configured. The module holds no state of its own.
 */
esp_err_t dc_provision_ensure_wifi(char *ssid, size_t ssid_cap,
                                       char *pass, size_t pass_cap);

/**
 * The DcIdentifyStore that persists the IDENTIFY budget to NVS.
 *
 * Keys, all inside DC_NVS_NAMESPACE: "identify_count" as u32 and
 * "identify_wstart" as u64, the latter being the window start in
 * epoch milliseconds. The plan named the second key
 * "identify_window_start", which is 21 characters and cannot be
 * an NVS key name: the 15-character limit described above applies
 * to keys as well, and the API answers the full name with
 * ESP_ERR_NVS_INVALID_NAME, which would fail every write and put
 * every boot into the fatal unwritable-counter state. The
 * shortened physical name is the one deviation from the plan and
 * is recorded in the Task 8 report; it is a single #define to
 * revisit if the limit ever changes.
 *
 * A missing key reads as zero with ESP_OK, because a zero count
 * and a zero window start are discord_identify.h's answer for a
 * device that has never sent an IDENTIFY: the first IDENTIFY is
 * allowed. Only a real read error, such as a corrupted namespace,
 * returns non-zero, and dc_identify_load() turns that into a
 * fatal boot stop rather than an unknown budget, which is the
 * safe direction for a guard whose failure mode is permanent
 * token revocation.
 *
 * The pair is read as a whole: both keys present is a complete
 * answer, both absent is a fresh device, and one without the
 * other is corruption, reported as an error rather than as a
 * budget, because a count without its window start is not a state
 * this module can reason about, and guessing is how a quota gets
 * burned.
 *
 * Every write sets both keys and then commits: an uncommitted
 * write does not survive a reboot, and the reboot is the exact
 * case this store exists for. One nvs_commit persists both keys,
 * but a power loss in the middle of it can leave one key updated.
 * The count is written first, so the torn state reads as an
 * expired window, which the next record rolls over normally,
 * rather than as a new window that still carries the old count,
 * which would hold the quota spent for a further 24 h.
 *
 * A failed write returns the esp_err_t unchanged through the DcErr
 * callback - esp_err_t and DcErr are the same 32-bit int type on
 * the ESP32, so the callbacks return esp_err_t and pass it
 * through with no adapter and no cast - and logs it with ESP_LOGE
 * under the fixed message "identify counter write failed", which
 * is the distinct log line the fatal-at-boot rule refers to.
 *
 * The NVS handle is opened per operation rather than kept open,
 * because the DcIdentifyStore interface has no close hook, so a
 * long-lived handle would either leak it or invent a lifecycle
 * that nobody owns. Identify operations happen a few times a day
 * at most, so the cost of an open is irrelevant next to a TLS
 * handshake.
 *
 * ctx is ignored: the callbacks are stateless and the store is
 * returned with a NULL context, because the namespace and the
 * keys are compile-time constants rather than per-instance values.
 */
DcIdentifyStore dc_nvs_identify_store(void);

#ifdef __cplusplus
}
#endif

#endif
