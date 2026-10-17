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

## Embedded Security Considerations

- **Tokens & Secrets:** Never hardcode Discord bot tokens or Wi-Fi credentials in source files. Always use the Flash NVS encrypted storage partition or `.env` during provisioning.
- **IDENTIFY Rate Limits:** Discord strictly enforces 1,000 IDENTIFY calls per 24-hour rolling window. This framework persists the quota counter to NVS across device reboots to prevent token revocation.
- **TLS 1.3:** Always use valid root certificates (`mbedTLS 4.1.0`) when communicating with Discord Gateway and REST API endpoints.
