# Security Policy

## Supported Versions

The `main` branch is the supported development branch.

## Reporting Security Issues

Report security issues privately to the repository owner, Mateusz Sury, through
GitHub's private vulnerability reporting flow when enabled. If that flow is not
available, open a minimal issue asking for a private contact path and do not
include exploit details or credentials in the public issue.

## Credential Handling

Runtime credentials must stay out of git:

- Wi-Fi SSID/password,
- MQTT username/password,
- Home Assistant tokens,
- broker addresses tied to private deployments,
- Zigbee network keys,
- OTA URLs containing secrets.

The firmware stores runtime credentials in ESP32-C6 NVS after configuration.
Examples in this repository use placeholders only.
