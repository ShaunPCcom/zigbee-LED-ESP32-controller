/**
 * Zigbee2MQTT External Converter for ZB_LED_CTRL
 *
 * Exposes:
 *   - rgb:   Color wheel (HS) + brightness -> drives R, G, B channels
 *   - white: Brightness only -> drives W channel (fixed color temp)
 *   - led_count: Number of LEDs in the strip (custom cluster, triggers reboot)
 *
 * Installation:
 * 1. Copy this file to your Zigbee2MQTT external converters directory
 * 2. Add to configuration.yaml:
 *    external_converters:
 *      - zb_led_controller.js
 * 3. Restart Zigbee2MQTT
 */

'use strict';

const {light} = require('zigbee-herdsman-converters/lib/modernExtend');

// ---- ZCL data type constants ----
const ZCL_UINT16 = 0x21;

// ---- Expose access flags ----
const ACCESS_ALL = 0b111; // read + write + subscribe

// ---- Custom cluster definition ----
const CLUSTER_DEVICE_CONFIG = 0xFC00;

const ledCtrlConfigCluster = {
    ID: CLUSTER_DEVICE_CONFIG,
    attributes: {
        ledCount: {ID: 0x0000, type: ZCL_UINT16, write: true},
    },
    commands: {},
    commandsResponse: {},
};

function registerCustomClusters(device) {
    device.addCustomCluster('ledCtrlConfig', ledCtrlConfigCluster);
}

// ---- Expose helpers ----
function numericExpose(name, label, access, description, opts) {
    const e = {type: 'numeric', name, label, property: name, access, description};
    if (opts) Object.assign(e, opts);
    return e;
}

// ---- fromZigbee ----
const fzLocal = {
    config: {
        cluster: 'ledCtrlConfig',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            const result = {};
            if (msg.data.ledCount !== undefined) result.led_count = msg.data.ledCount;
            return result;
        },
    },
};

// ---- toZigbee ----
const tzLocal = {
    led_count: {
        key: ['led_count'],
        convertSet: async (entity, key, value, meta) => {
            registerCustomClusters(meta.device);
            const ep = meta.device.getEndpoint(1);
            await ep.write('ledCtrlConfig', {ledCount: value});
            return {state: {led_count: value}};
        },
        convertGet: async (entity, key, meta) => {
            registerCustomClusters(meta.device);
            const ep = meta.device.getEndpoint(1);
            await ep.read('ledCtrlConfig', ['ledCount']);
        },
    },
};

// ---- Device definition ----
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

    fromZigbee: [fzLocal.config],
    toZigbee: [tzLocal.led_count],

    exposes: [
        numericExpose('led_count', 'LED count', ACCESS_ALL,
            'Number of LEDs in the strip (reboot required after change)',
            {value_min: 1, value_max: 500, value_step: 1}),
    ],

    meta: {
        multiEndpoint: true,
    },

    endpoint: (device) => {
        return {rgb: 1, white: 2};
    },

    onEvent: async (type, data, device) => {
        if (type === 'start' || type === 'deviceInterview') {
            registerCustomClusters(device);
        }
    },

    configure: async (device, coordinatorEndpoint) => {
        registerCustomClusters(device);
        const ep1 = device.getEndpoint(1);
        await ep1.read('ledCtrlConfig', ['ledCount']);
    },
};

module.exports = definition;
