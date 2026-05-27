# Zigbee2MQTT Support

This directory contains the Zigbee2MQTT integration package for the ESP32-C6
Smart Irrigation Controller by Mateusz Sury.

## Files

- `irrigation_controller_converter.js` - Zigbee2MQTT external converter.
- `device_icons/esp32c6-irrigation-controller.png` - optional device icon.

## Install

1. Copy `irrigation_controller_converter.js` to the external converters
   directory used by your Zigbee2MQTT installation.
2. Configure Zigbee2MQTT to load the converter according to your Zigbee2MQTT
   version.
3. Restart Zigbee2MQTT.
4. Enable `permit_join`.
5. Set the controller to Zigbee mode or enable Zigbee Router while MQTT mode is
   active.
6. Wait for the device interview to finish.

## Device Icon

Copy the PNG file to a `device_icons` directory next to your Zigbee2MQTT
configuration and set the device option:

```json
{
  "id": "0xYOUR_DEVICE_IEEE_ADDRESS",
  "options": {
    "icon": "device_icons/esp32c6-irrigation-controller.png"
  }
}
```

You can set the same option through the Zigbee2MQTT frontend or by publishing to:

```text
zigbee2mqtt/bridge/request/device/options
```

## Exposes

The converter exposes:

- `line_1`, `line_2`, `line_3` switches,
- `duration` per line, unit `s`, range `1..86400`,
- `schedule_enabled` per line,
- `schedule_start_minute` per line, unit `min`, range `0..1439`,
- weekday switches for Monday through Sunday per line,
- global `interlock`.
