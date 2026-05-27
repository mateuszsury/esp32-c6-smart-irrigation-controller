# Contributing

Contributions are welcome when they preserve the controller safety model:

- command paths must submit events to `irrigation_core`,
- MQTT, Zigbee, HTTP, UART, and scheduler code must not drive relay GPIO directly,
- new ON paths must respect OTA-in-progress and interlock behavior,
- changes that touch configuration persistence must include migration behavior,
- hardware-facing changes need tests or clear manual verification steps.

Before opening a change:

```powershell
wsl.exe bash -lc "cd /path/to/repo && ./tests/run_host_tests.sh"
node --check zigbee2mqtt\irrigation_controller_converter.js
```

Do not commit Wi-Fi passwords, MQTT credentials, Home Assistant tokens, device
network addresses, local logs, build output, or generated ESP-IDF component
downloads.
