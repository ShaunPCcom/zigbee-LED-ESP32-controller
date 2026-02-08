# ZB-H2 LED Controller

A Zigbee LED strip controller firmware for the ESP32-H2, integrating with Home Assistant via Zigbee2MQTT. Supports up to two physical SK6812 RGBW LED strips divided into up to eight independent virtual segments, each exposed as a separate Extended Color Light in Home Assistant.

## Features

- **Dual physical strip support** — two LED outputs via SPI2 time-multiplexing
- **8 virtual segments** — independently controllable overlapping or non-overlapping regions
- **Full color control** — RGB (HS/XY) and color temperature (CT/white) modes per segment
- **Per-segment power-on behavior** — off, on, toggle, or restore previous state
- **NVS persistence** — geometry, state, and configuration survive reboots
- **Zigbee Router** — extends your Zigbee mesh (mains-powered)
- **Home Assistant integration** — via Zigbee2MQTT external converter
- **Serial CLI** — configure strip counts, segment geometry, and device settings
- **OTA-ready** (planned Phase 5)

## Hardware Requirements

- **MCU**: ESP32-H2-DevKitM-1 (or compatible ESP32-H2 board)
- **LED strips**: SK6812 RGBW (WS2812B RGB also supported)
- **Power**: Mains-powered (5V for LED strips, USB for dev board)

### GPIO Pinout

| Function | GPIO | Notes |
|----------|------|-------|
| LED Strip 1 | GPIO4 | SPI2 MOSI |
| LED Strip 2 | GPIO5 | SPI2 MOSI (time-multiplexed) |
| Onboard LED | GPIO8 | Status indicator (built-in WS2812) |
| Boot Button | GPIO9 | Reset functions |

### Why SPI instead of RMT?

The ESP32-H2 RMT peripheral conflicts with the Zigbee radio when driving WS2812-style LEDs. The onboard status LED uses the single safe RMT channel. Both external strips use SPI2 with MOSI time-multiplexing between strip refreshes.

## Building

Requires [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/).

```bash
idf.py set-target esp32h2
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Zigbee2MQTT Setup

Copy `z2m/zb_led_controller.js` to your Zigbee2MQTT `data/external_converters/` directory and restart Z2M. The device will appear as **ZB_LED_CTRL** after pairing.

To pair: hold the boot button for 3 seconds (Zigbee network reset) while Z2M is in pairing mode.

## Device Endpoints

The controller presents 8 endpoints (EP1–EP8), each an Extended Color Light:

| EP | Role |
|----|------|
| 1 | Segment 1 (default: full strip 1) + device config |
| 2–8 | Segments 2–8 (disabled by default, count=0) |

Each segment exposes brightness, RGB color (hue/saturation), color temperature (white channel), on/off, and power-on behavior.

### Custom Clusters

**0xFC00 — Device Configuration (EP1)**

| Attribute | Description |
|-----------|-------------|
| `strip1_count` | LED count for physical strip 1 |
| `strip2_count` | LED count for physical strip 2 (0 = disabled) |

**0xFC01 — Segment Geometry (EP1)**

Each of the 8 segments has three attributes:

| Attribute | Description |
|-----------|-------------|
| `segN_start` | First LED index |
| `segN_count` | Number of LEDs (0 = disabled) |
| `segN_strip` | Physical strip assignment (1 or 2) |

## CLI Reference

Connect via serial monitor (`idf.py -p /dev/ttyACM0 monitor`). All commands are prefixed with `led `.

| Command | Description |
|---------|-------------|
| `led help` | Show available commands |
| `led config` | Show strip configuration |
| `led count <strip> <n>` | Set LED count for strip 1 or 2 (reboot to apply) |
| `led seg [1-8]` | Show segment geometry and state |
| `led seg <n> start <val>` | Set segment start index |
| `led seg <n> count <val>` | Set segment LED count (0 disables) |
| `led seg <n> strip <val>` | Assign segment to strip 1 or 2 |
| `led nvs` | NVS health check |
| `led reboot` | Restart device |
| `led repair` | Zigbee network reset (keeps config) |
| `led factory-reset` | Full reset (erases Zigbee + all config) |

### Button Reset (Boot Button / GPIO9)

| Hold time | Action |
|-----------|--------|
| 3 seconds | Zigbee network reset (keeps NVS config) |
| 10 seconds | Full factory reset (Zigbee + NVS erased) |

## Status LED

The onboard WS2812 (GPIO8) indicates device state:

| Color / Pattern | State |
|-----------------|-------|
| Amber blinking | Not joined to network |
| Blue blinking | Pairing / joining |
| Solid green (5s) | Successfully joined |
| Red blinking | Error (retries after 5s) |

## License

MIT — see [LICENSE](LICENSE)
