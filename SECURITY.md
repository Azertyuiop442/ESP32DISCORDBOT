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

- **TLS 1.2 & Root CA Pinned:** All Gateway WebSocket (`wss://`) and REST (`https://`) communications negotiate **TLS 1.2** using `mbedTLS 4.1.0` with DigiCert Global Root CA and GlobalSign Root CA pinned directly in firmware. TLS 1.3 is intentionally disabled via `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` to conserve ~20 KB of DRAM per TLS context (~39 KB total), critical for ESP32 stability.
- **Remote vs. Physical Security (Deployer Responsibility):**
  - **Remote Protection (Built-in):** The firmware strictly blocks HTTP/WS plaintext connections, enforces an allowlist for HTTP headers, drops and rejects redirect chains (`Location` headers) to prevent token exfiltration, and redacts sensitive credentials in serial logs.
  - **Physical Flash Protection (Configured by Deployer):** By default, development builds do not activate hardware Flash Encryption to avoid burning irreversible eFuses on developer boards. **You (the deployer) decide the hardware security level.** If your ESP32 board is placed in an untrusted physical environment where physical extraction is a threat, you must enable **Flash Encryption** (`CONFIG_SECURE_FLASH_ENC_ENABLED`), **Secure Boot v2** (`CONFIG_SECURE_BOOT`), and **NVS Encryption** (`CONFIG_NVS_ENCRYPTION`) via `idf.py menuconfig` before burning to hardware.
- **Tokens & Credentials Provisioning:** Never commit `.env` or hardcode bot tokens or Wi-Fi credentials into source code. Provisioning is done over local serial monitor or encrypted NVS partition.
- **IDENTIFY Rate Limits:** Discord strictly enforces 1,000 IDENTIFY calls per 24-hour rolling window. This framework persists the quota counter and window timestamp to NVS across device reboots to prevent accidental token revocation.
- **Privacy & Member Data Minimization:** No Personally Identifiable Information (PII) such as message bodies, member usernames, avatars, IP addresses, or emails are stored. Only operational 64-bit Discord Snowflake IDs and numerical progression counters (XP, level) are retained, with volatile memory (RAM) flushed on reboot and persistent records manageable via moderation commands or `dc_store_delete`.

