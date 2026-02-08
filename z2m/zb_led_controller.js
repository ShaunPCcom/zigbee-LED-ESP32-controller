/**
 * Zigbee2MQTT External Converter for ZB_LED_CTRL
 *
 * Exposes two light entities:
 *   - rgb:   Color wheel (XY) + brightness -> drives R, G, B channels
 *   - white: Brightness only -> drives W channel (fixed color temp)
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

    extend: [
        // Endpoint 1: RGB with hue/saturation color wheel
        light({
            color: {modes: ['hs'], enhancedHue: true},
            endpointNames: ['rgb'],
        }),
        // Endpoint 2: White channel brightness only
        light({
            endpointNames: ['white'],
        }),
    ],

    meta: {
        multiEndpoint: true,
    },

    endpoint: (device) => {
        return {rgb: 1, white: 2};
    },
};

module.exports = definition;
