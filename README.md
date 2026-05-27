# ESP32-C6 Smart Irrigation Controller

Production-oriented ESP-IDF firmware for an ESP32-C6 based multi-zone irrigation
controller. The project is written for ESP-IDF, uses no MicroPython, and keeps
all valve decisions in one deterministic controller core.

Author and maintainer: Mateusz Sury

## Features

- ESP32-C6 target with ESP-IDF.
- 1 to 8 irrigation lines, 3 lines enabled by default.
- Default valve GPIOs: `GPIO2`, `GPIO3`, `GPIO10`.
- Relay polarity: `active_high=true`.
- Local interlock: when enabled, turning on one line turns all other lines off.
- Local per-line timeout based on ESP-IDF monotonic timers.
- Local schedules with enabled flag, start time, weekdays, and duration.
- Wi-Fi captive portal and LAN control panel.
- MQTT mode with Home Assistant MQTT Discovery.
- Zigbee Router mode with one On/Off endpoint per line.
- Optional Zigbee Router while MQTT remains the primary control mode.
- Zigbee2MQTT external converter and device icon.
- HTTP OTA upload, OTA-by-URL, and MQTT-triggered OTA-by-URL.
- Runtime configuration stored in NVS with schema versioning and CRC fallback.
- UART diagnostic console.
- Onboard RGB status LED on `GPIO8` for ESP32-C6 DevKit boards.

## Safety Model

The firmware is structured around one rule: only `irrigation_core` owns the
desired valve state. MQTT, Zigbee, HTTP, UART, and the scheduler submit events
to one queue; they do not drive GPIO directly.

Implemented safety behavior:

- all relays are forced OFF during boot,
- relay GPIOs are configured before any ON command is accepted,
- configuration changes force all relays OFF,
- OTA forces all relays OFF and blocks new ON commands while an update is in progress,
- each ON command gets a local timeout,
- interlock is enforced in the controller core, independent of command source,
- invalid NVS configuration falls back to defaults,
- NVS data uses a magic value, schema version, and CRC,
- controller and scheduler tasks are registered with the ESP task watchdog,
- the boot path brings relays to OFF again after brownout, watchdog reset, panic, or OTA restart.

This is still not a certified industrial safety controller. Before connecting
real valves permanently, run hardware-in-the-loop tests with the final relay
board, power supply, enclosure, and wiring.

## Hardware Defaults

Default settings live in:

```text
components/board_config/include/board_config.h
```

Important defaults:

```c
#define IRRIGATION_DEFAULT_LINE_COUNT 3
#define IRRIGATION_DEFAULT_RELAY_ACTIVE_HIGH true
#define IRRIGATION_STATUS_LED_GPIO 8
#define IRRIGATION_DEFAULT_ZIGBEE_ROUTER_WITH_MQTT false

static const int IRRIGATION_DEFAULT_RELAY_GPIOS[IRRIGATION_DEFAULT_LINE_COUNT] = {
    2,
    3,
    10,
};
```

`GPIO4` is intentionally not used for valves because it is a strapping/JTAG
risk on ESP32-C6 boards. The firmware also blocks unsafe GPIOs for relay output
validation, including boot strapping pins, USB Serial/JTAG pins, UART0 console
pins, and SPI flash/PSRAM pins.

## Status LED

Official ESP32-C6 DevKit boards expose the onboard addressable RGB LED on
`GPIO8`. The firmware uses it as a status indicator:

- MQTT mode with Wi-Fi connected: slow blue pulse,
- Zigbee mode: slow green pulse,
- Wi-Fi not connected before AP startup: solid yellow,
- captive portal/AP active: slow yellow pulse.

`GPIO8` remains blocked for valve output because it is also a strapping pin and
is used by the onboard LED on common ESP32-C6 development boards.

## Boot Mode Button

Default mode button:

- GPIO: `9`,
- active level: `0`,
- hold time at boot: `1500 ms`.

Holding the button during boot toggles the stored communication mode between
`mqtt` and `zigbee`.

## Control Panel And Captive Portal

The HTTP control panel starts in both communication modes.

When Wi-Fi credentials are missing or the device cannot join Wi-Fi, it starts an
open configuration AP:

```text
SSID: Irrigation-XXXXXX
URL:  http://192.168.4.1/
```

The panel can:

- configure Wi-Fi credentials,
- configure MQTT URI, username, password, and topic prefix,
- select MQTT or Zigbee mode,
- enable or disable interlock,
- enable optional Zigbee Router while in MQTT mode,
- control each line ON/OFF,
- force All OFF,
- configure line count, names, GPIOs, durations, and schedules,
- upload OTA firmware.

Status endpoint:

```text
GET /api/status
```

## MQTT And Home Assistant

MQTT is configured at runtime through the control panel or UART console.

Required runtime values:

- Wi-Fi SSID,
- Wi-Fi password when required by the network,
- MQTT URI, for example `mqtt://broker.local:1883`,
- MQTT prefix, default `irrigation`.

Home Assistant MQTT Discovery payloads are published under:

```text
homeassistant/switch/<device_id>/line_<n>/config
homeassistant/number/<device_id>/line_<n>_duration/config
homeassistant/switch/<device_id>/line_<n>_schedule_enabled/config
homeassistant/text/<device_id>/line_<n>_schedule_time/config
homeassistant/switch/<device_id>/line_<n>_schedule_mon/config
homeassistant/switch/<device_id>/line_<n>_schedule_tue/config
...
homeassistant/switch/<device_id>/line_<n>_schedule_sun/config
homeassistant/switch/<device_id>/interlock/config
homeassistant/sensor/<device_id>/mode/config
homeassistant/sensor/<device_id>/ota_status/config
```

Command topics:

```text
<prefix>/<device_id>/line/<n>/set
<prefix>/<device_id>/line/<n>/duration/set
<prefix>/<device_id>/line/<n>/schedule/enabled/set
<prefix>/<device_id>/line/<n>/schedule/time/set
<prefix>/<device_id>/line/<n>/schedule/day/mon/set
...
<prefix>/<device_id>/line/<n>/schedule/day/sun/set
<prefix>/<device_id>/interlock/set
<prefix>/<device_id>/all_off/set
<prefix>/<device_id>/ota/url/set
```

Availability:

```text
<prefix>/<device_id>/availability
```

The MQTT client uses Last Will `offline` and publishes retained `online` after a
successful connection.

## Zigbee And Zigbee2MQTT

Zigbee mode uses Espressif's ESP Zigbee SDK and starts as a mains-powered
Zigbee Router. Each enabled line is exposed as a Zigbee HA On/Off endpoint:

```text
endpoint 1 = line_1
endpoint 2 = line_2
endpoint 3 = line_3
```

The firmware also exposes a manufacturer-specific cluster for irrigation
configuration:

```text
cluster:           0xFC00
manufacturer code: 0x131B
attribute 0x0001:  uint32 duration seconds
attribute 0x0002:  bool global interlock
attribute 0x0003:  uint8 line count
attribute 0x0010:  bool schedule enabled
attribute 0x0011:  uint16 schedule start minute, 0..1439
attribute 0x0012:  uint8 weekday mask, 0..127
```

The Zigbee2MQTT integration package is in:

```text
zigbee2mqtt/
```

It contains:

- `irrigation_controller_converter.js` external converter,
- `device_icons/esp32c6-irrigation-controller.png` device icon,
- `README.md` installation notes.

The converter exposes:

- switches for `line_1`, `line_2`, `line_3`,
- numeric `duration` values with unit `s`,
- schedule enabled switches,
- schedule start minute values with unit `min`,
- separate weekday switches instead of requiring a raw bit mask,
- global `interlock`.

## Offline Behavior

Line timeout and interlock are local and do not depend on Home Assistant,
MQTT, Wi-Fi, Zigbee, or Zigbee2MQTT.

Schedules are local too, but they require valid system time. In MQTT mode the
device obtains time through SNTP after Wi-Fi connects. If Wi-Fi later drops,
schedules can continue from the last valid system time. After a cold boot with
no network time, clock-based schedules wait until time becomes valid.

## OTA

The project uses two OTA application slots in a 4 MB flash layout:

```text
ota_0: 0x20000,  0x1D0000
ota_1: 0x1F0000, 0x1D0000
```

OTA behavior:

- all lines are forced OFF before update,
- ON commands are blocked while OTA is active,
- image is written to the inactive slot,
- uploaded image descriptor is verified,
- boot partition is changed,
- new image is marked valid after a successful boot,
- ESP-IDF rollback support is enabled.

OTA options:

- HTTP binary upload through the control panel,
- HTTP URL update through the control panel,
- MQTT URL update through `<prefix>/<device_id>/ota/url/set`.

## Build

Use ESP-IDF from WSL:

```bash
cd /path/to/esp32-c6-smart-irrigation-controller
source ~/esp-idf/export.sh
idf.py -B build/esp-idf set-target esp32c6
idf.py -B build/esp-idf build
```

## Flash From PowerShell

Replace `COMx` with the detected ESP32-C6 serial port:

```powershell
py -3 -m esptool --chip esp32c6 -p COMx -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 4MB --flash-freq 80m 0x0 build\esp-idf\bootloader\bootloader.bin 0x8000 build\esp-idf\partition_table\partition-table.bin 0xf000 build\esp-idf\ota_data_initial.bin 0x20000 build\esp-idf\irrigation_controller.bin
```

## UART Console

Default console speed:

```text
115200
```

Commands:

```text
status
off
on <n>
lineoff <n>
wifi <ssid> <password>
mqtt <uri> <user> <password> [prefix]
mode mqtt
mode zigbee
reset-config
```

## Tests

Host tests:

```powershell
wsl.exe bash -lc "cd /path/to/esp32-c6-smart-irrigation-controller && ./tests/run_host_tests.sh"
```

Zigbee2MQTT converter syntax check:

```powershell
node --check zigbee2mqtt\irrigation_controller_converter.js
```

Recommended hardware checks before connecting valves:

1. Build firmware.
2. Flash through USB.
3. Open UART monitor.
4. Confirm all configured lines start OFF.
5. Confirm valve GPIOs are `2`, `3`, and `10`.
6. Test ON/OFF for each line with a meter or indicator LED before using valves.
7. Test timeout.
8. Test interlock.
9. Restart the ESP32-C6 while a test output is ON and confirm boot returns all outputs to OFF.
10. Run a 24-72 hour soak test on final power and relay hardware.


## License

This project is licensed under the MIT License.

Copyright (c) 2026 Mateusz Sury

See [LICENSE](LICENSE).
