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

## Preset Management

The controller supports up to 8 saved presets (slots 0-7) that capture the complete state of all 8 segments (on/off, brightness, color, white temperature). Presets are stored in NVS flash and survive reboots.

### Overview

Each preset slot stores:
- Custom name (up to 16 characters, optional)
- All 8 segment states (on/off, brightness, RGB color, CT/white mode)
- Persistent across power cycles and firmware updates

Slot numbers (0-7) are stable identifiers designed for reliable Home Assistant automations. Names are metadata for human readability.

### Workflow

**1. Configure your desired lighting scene**
- Use Home Assistant or Z2M to set segment colors, brightness, and on/off states
- Adjust as many segments as needed (segments can be on or off)

**2. Save the preset**
- Select a slot number (0-7) in the Z2M UI or HA
- Optionally provide a custom name (e.g., "Evening", "Movie", "Work")
- Click "Save Preset"

**3. Recall the preset**
- Select the slot number
- Click "Apply Preset"
- All segments instantly change to the saved state (including turning on if they were off)
- Home Assistant UI updates automatically after ~500ms

**4. Delete a preset** (optional)
- Select the slot number
- Click "Delete Preset"
- Slot becomes empty and available for reuse

### Using Presets in Home Assistant Automations

Presets are designed for WLED-style pattern usage in HA automations. Use slot numbers as stable identifiers:

```yaml
# Example: Activate preset slot 0 ("Evening") at sunset
automation:
  - alias: "Evening Lights"
    trigger:
      - platform: sun
        event: sunset
    action:
      - service: number.set_value
        target:
          entity_id: number.zb_led_ctrl_preset_slot
        data:
          value: 0
      - service: select.select_option
        target:
          entity_id: select.zb_led_ctrl_apply_preset
        data:
          option: "Apply"

# Example: Morning routine with preset slot 1 ("Morning")
automation:
  - alias: "Morning Lights"
    trigger:
      - platform: time
        at: "07:00:00"
    action:
      - service: number.set_value
        target:
          entity_id: number.zb_led_ctrl_preset_slot
        data:
          value: 1
      - service: select.select_option
        target:
          entity_id: select.zb_led_ctrl_apply_preset
        data:
          option: "Apply"

# Example: Movie mode with preset slot 2
automation:
  - alias: "Movie Mode"
    trigger:
      - platform: state
        entity_id: media_player.living_room_tv
        to: "playing"
    action:
      - service: number.set_value
        target:
          entity_id: number.zb_led_ctrl_preset_slot
        data:
          value: 2
      - service: select.select_option
        target:
          entity_id: select.zb_led_ctrl_apply_preset
        data:
          option: "Apply"
```

**Why slot numbers instead of names?**
- Slot numbers (0-7) never change, making automations reliable
- Names can be changed without breaking automations
- Follows WLED's proven pattern for preset management

### Zigbee2MQTT UI Usage

**Preset Controls (in device page):**
- **Preset Slot** — dropdown to select slot 0-7
- **Apply Preset** — button to recall selected slot
- **Delete Preset** — button to delete selected slot
- **New Preset Name** — text field for custom name (optional, max 16 chars)
- **Save Preset** — button to save current state to selected slot

**Preset Slot Names (sensors):**
- **Slot 0-7 Name** — displays name or "(empty)" for each slot

**Workflow:**
1. Configure lighting in HA/Z2M
2. Select slot number from dropdown
3. Enter custom name (optional)
4. Click "Save Preset"
5. Later: select slot and click "Apply Preset" to recall

### Custom Zigbee Cluster (0xFC02)

**Preset management attributes (on EP1):**

| Attribute | ID | Type | Access | Purpose |
|-----------|-----|------|--------|---------|
| `preset_count` | 0x0000 | U8 | R | Number of occupied slots (0-8) |
| `recall_slot` | 0x0020 | U8 | W | Write slot number (0-7) to recall |
| `save_slot` | 0x0021 | U8 | W | Write slot number (0-7) to save |
| `delete_slot` | 0x0022 | U8 | W | Write slot number (0-7) to delete |
| `save_name` | 0x0023 | CharString | W | Write name before save (optional) |
| `preset_0_name` | 0x0010 | CharString | R | Name of preset in slot 0 |
| `preset_1_name` | 0x0011 | CharString | R | Name of preset in slot 1 |
| ... | ... | ... | ... | ... |
| `preset_7_name` | 0x0017 | CharString | R | Name of preset in slot 7 |

**Deprecated attributes (kept for backwards compatibility):**
- `active_preset` (0x0001) — always returns empty string
- `recall_preset` (0x0002) — name-based recall (use `recall_slot` instead)
- `save_preset` (0x0003) — name-based save (use `save_slot` instead)
- `delete_preset` (0x0004) — name-based delete (use `delete_slot` instead)

### CLI Commands

```bash
# List all preset slots
led preset

# Save current state to slot 3 with name "Evening"
led preset save 3 Evening

# Recall preset from slot 3
led preset apply 3

# Delete preset from slot 3
led preset delete 3

# Save to slot 0 without custom name (uses default "Preset 1")
led preset save 0
```

### Migration Notes

**From name-based presets (pre-v2):**
- Existing presets with names are automatically preserved in slots 0-7
- Slots without presets get default names ("Preset 1" through "Preset 8")
- Migration happens transparently on first boot after firmware update
- No user action required

**NVS storage:**
- Namespace: `led_cfg`
- Keys: `prst_0` through `prst_7` (129 bytes each)
- Version flag: `prst_version` (value 2 for slot-based)

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
| `led preset` | List all preset slots with names and status |
| `led preset save <slot> [name]` | Save current state to slot 0-7 (optional name) |
| `led preset apply <slot>` | Recall preset from slot 0-7 |
| `led preset delete <slot>` | Delete preset from slot 0-7 |
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
