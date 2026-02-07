/**
 * Zigbee2MQTT External Converter for ZB_LED_CTRL
 *
 * Installation:
 * 1. Copy this file to your Zigbee2MQTT external converters directory
 * 2. Add to configuration.yaml:
 *    external_converters:
 *      - zb_led_controller.js
 * 3. Restart Zigbee2MQTT
 */

const {light} = require('zigbee-herdsman-converters/lib/modernExtend');

const definition = {
    zigbeeModel: ['ZB_LED_CTRL'],
    model: 'ZB_LED_CTRL',
    vendor: 'DIY',
    description: 'Zigbee LED Strip Controller (ESP32-H2)',

    // Use the modern light extend with full color support
    extend: [
        light({
            colorTemp: {range: [153, 370]},  // 6500K to 2700K
            color: {modes: ['xy', 'hs'], enhancedHue: true},
        })
    ],

    meta: {
        multiEndpoint: false,  // Single endpoint for now (Phase 2)
    },
};

module.exports = definition;
