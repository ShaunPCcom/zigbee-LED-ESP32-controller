# Zigbee2MQTT External Converter

## Installation

1. **Copy the converter file to your Z2M configuration directory:**
   ```bash
   # For Home Assistant addon:
   cp zb_led_controller.js /config/zigbee2mqtt/

   # For standalone Z2M:
   cp zb_led_controller.js /opt/zigbee2mqtt/data/
   ```

2. **Edit your Zigbee2MQTT configuration.yaml:**
   ```yaml
   external_converters:
     - zb_led_controller.js
   ```

3. **Restart Zigbee2MQTT**

4. **Pair the device:**
   - Power on your ESP32-H2 LED controller
   - In Home Assistant, go to Settings → Devices & Services → Zigbee2MQTT
   - Click "Add Device" or enable permit join
   - The device should appear as "DIY ZB_LED_CTRL"

## Exposed Controls

- **On/Off**: Turn lights on or off
- **Brightness**: 0-254 level control
- **Color (XY)**: Full color picker
- **Color (Hue/Saturation)**: Alternative color control
- **Color Temperature**: Warm to cool white (2700K-6500K)

## Troubleshooting

If the device doesn't pair:
1. Check Z2M logs for errors
2. Verify the external converter is loaded (check Z2M startup logs)
3. Try removing and re-adding the external_converters line
4. Restart Z2M after any configuration changes

If colors don't work:
1. Check ESP32-H2 serial monitor for crash logs
2. Verify LED strip is connected to GPIO4
3. Check power supply is adequate for the LED strip
