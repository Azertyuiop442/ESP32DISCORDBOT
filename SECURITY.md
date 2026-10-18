# Security Policy

## Supported Versions

| Version | Supported          |
| ------- | ------------------ |
| 1.0.x   | :white_check_mark: |
| < 1.0   | :x:                |

## Reporting a Vulnerability

Please do not report security vulnerabilities through public GitHub issues.

Instead, please report security concerns privately using GitHub's Security Advisories feature or contact the maintainers directly.

When reporting a vulnerability, please provide:
- A description of the issue and potential impact
- Detailed reproduction steps or proof-of-concept payload
- Environment details (ESP-IDF version, hardware target)

You will receive an acknowledgment within 48 hours and regular updates on the remediation process.

## Embedded Security Considerations & Threat Model

- **TLS 1.2 & Multi-Root Pinned CA Store:** All Gateway WebSocket (`wss://`) and REST (`https://`) communications negotiate **TLS 1.2** using `mbedTLS 4.1.0`. Firmware pins four trusted root authorities in `discord_root_ca.h`: GlobalSign Root CA (2028), DigiCert Global Root CA (2031), Google Trust Services GTS Root R1 (2036), and GTS Root R4 (2036). TLS 1.3 is intentionally disabled via `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` to conserve ~20 KB of DRAM per TLS context (~39 KB total), critical for ESP32 stability.
- **Remote vs. Physical Security (Deployer Responsibility):**
  - **Remote Protection (Built-in):** The firmware strictly blocks HTTP/WS plaintext connections, enforces an allowlist for HTTP headers, drops and rejects redirect chains (`Location` headers) to prevent token exfiltration, and redacts sensitive credentials in serial logs.
  - **Physical Flash Protection (Configured by Deployer):** By default, development builds leave Flash Encryption and Secure Boot disabled in `sdkconfig.defaults` to prevent burning irreversible hardware eFuses on prototyping boards. Physical security against device dumping must be enabled by deployers for production deployments:
    - *Development Mode (`CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT`):* Uses an internal key while permitting re-flashing plaintext images via UART bootloader up to a limit tracked by the 7-bit `FLASH_CRYPT_CNT` eFuse.
    - *Release Mode (`CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE`):* Permanently burns `FLASH_CRYPT_CNT` to `0x7F`, irreversibly locking the board against UART bootloader plaintext flashing (subsequent updates must be signed OTA).
    - *NVS Encryption (`CONFIG_NVS_ENCRYPTION`):* Protects bot tokens and Wi-Fi credentials stored in Flash NVS.
- **Tokens & Credentials Provisioning:** Never commit `.env` or hardcode bot tokens or Wi-Fi credentials into source code. Provisioning is performed over local serial monitor or encrypted NVS partition.
- **IDENTIFY Rate Limits:** Discord strictly enforces 1,000 IDENTIFY calls per 24-hour rolling window. This framework persists the quota counter and window timestamp to NVS across device reboots to prevent accidental token revocation.
- **Privacy Policy & GDPR Compliance (Pseudonymous Personal Data):**
  - Stores two things per member: the Discord user ID and a numeric rank/XP. A Discord ID is a pseudonymous identifier, so it counts as personal data under GDPR. Nothing else is kept: no message text, names, emails or IP addresses.
  - Active tickets, CAPTCHA challenges (5 min TTL), and spam hashes live exclusively in volatile RAM and are completely erased on reboot.
  - A slash command `/forget` (and `/forgetme`, restricted to authorized developers/administrators) allows permanently erasing stored pseudonymous progression data from device Flash/RAM upon user request.


