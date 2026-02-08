/**
 * Zigbee2MQTT External Converter for ZB_LED_CTRL
 *
 * Exposes:
 *   - rgb:     Color wheel (HS) + brightness -> drives R, G, B channels (EP1)
 *   - white:   Brightness only -> drives W channel (EP2)
 *   - seg1-8:  Virtual segments, each a Color Dimmable Light (EP3-10)
 *   - led_count:     Number of LEDs (custom cluster 0xFC00)
 *   - seg_N_start/count/white: Segment geometry (custom cluster 0xFC01)
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
const ZCL_UINT8  = 0x20;
const ZCL_UINT16 = 0x21;

// ---- Expose access flags ----
const ACCESS_ALL = 0b111; // read + write + subscribe

// ---- Custom cluster definitions ----
const CLUSTER_DEVICE_CONFIG  = 0xFC00;
const CLUSTER_SEGMENT_CONFIG = 0xFC01;
const MAX_SEGMENTS = 8;

// Build segment config cluster attributes: 3 per segment (start, count, white)
const segAttrs = {};
for (let n = 0; n < MAX_SEGMENTS; n++) {
    segAttrs[`seg${n}Start`] = {ID: 0x0000 + n * 3 + 0, type: ZCL_UINT16, write: true};
    segAttrs[`seg${n}Count`] = {ID: 0x0000 + n * 3 + 1, type: ZCL_UINT16, write: true};
    segAttrs[`seg${n}White`] = {ID: 0x0000 + n * 3 + 2, type: ZCL_UINT8,  write: true};
}

const ledCtrlConfigCluster = {
    ID: CLUSTER_DEVICE_CONFIG,
    attributes: {
        ledCount: {ID: 0x0000, type: ZCL_UINT16, write: true},
    },
    commands: {},
    commandsResponse: {},
};

const segmentConfigCluster = {
    ID: CLUSTER_SEGMENT_CONFIG,
    attributes: segAttrs,
    commands: {},
    commandsResponse: {},
};

function registerCustomClusters(device) {
    device.addCustomCluster('ledCtrlConfig', ledCtrlConfigCluster);
    device.addCustomCluster('segmentConfig', segmentConfigCluster);
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
    segments: {
        cluster: 'segmentConfig',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            const result = {};
            for (let n = 0; n < MAX_SEGMENTS; n++) {
                const s = n + 1;
                if (msg.data[`seg${n}Start`] !== undefined) result[`seg${s}_start`] = msg.data[`seg${n}Start`];
                if (msg.data[`seg${n}Count`] !== undefined) result[`seg${s}_count`] = msg.data[`seg${n}Count`];
                if (msg.data[`seg${n}White`] !== undefined) result[`seg${s}_white`] = msg.data[`seg${n}White`];
            }
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
    segments: {
        key: [],
        convertSet: async (entity, key, value, meta) => {
            registerCustomClusters(meta.device);
            const ep = meta.device.getEndpoint(1);
            // key is like seg1_start, seg3_white, etc.
            const m = key.match(/^seg(\d+)_(start|count|white)$/);
            if (!m) return;
            const n = parseInt(m[1]) - 1; // 0-indexed
            const field = m[2];
            const attrMap = {
                start: `seg${n}Start`,
                count: `seg${n}Count`,
                white: `seg${n}White`,
            };
            const attr = attrMap[field];
            if (!attr) return;
            await ep.write('segmentConfig', {[attr]: value});
            return {state: {[key]: value}};
        },
        convertGet: async (entity, key, meta) => {
            registerCustomClusters(meta.device);
            const ep = meta.device.getEndpoint(1);
            const m = key.match(/^seg(\d+)_(start|count|white)$/);
            if (!m) return;
            const n = parseInt(m[1]) - 1;
            const field = m[2];
            const attrMap = {
                start: `seg${n}Start`,
                count: `seg${n}Count`,
                white: `seg${n}White`,
            };
            await ep.read('segmentConfig', [attrMap[field]]);
        },
    },
};

// Register seg_N_start/count/white keys in tzLocal.segments
for (let n = 1; n <= MAX_SEGMENTS; n++) {
    tzLocal.segments.key.push(`seg${n}_start`, `seg${n}_count`, `seg${n}_white`);
}

// ---- Segment geometry exposes ----
const segExposes = [];
for (let n = 1; n <= MAX_SEGMENTS; n++) {
    segExposes.push(
        numericExpose(`seg${n}_start`, `Seg${n} start`, ACCESS_ALL, `Segment ${n} first LED index`, {value_min: 0, value_max: 65535, value_step: 1}),
        numericExpose(`seg${n}_count`, `Seg${n} count`, ACCESS_ALL, `Segment ${n} LED count (0=disabled)`, {value_min: 0, value_max: 65535, value_step: 1}),
        numericExpose(`seg${n}_white`, `Seg${n} white`, ACCESS_ALL, `Segment ${n} white level`, {value_min: 0, value_max: 254, value_step: 1}),
    );
}

// ---- Build segment light extends (EP3-EP10) ----
const segLightExtends = [];
for (let n = 1; n <= MAX_SEGMENTS; n++) {
    segLightExtends.push(light({
        color: {modes: ['hs'], enhancedHue: true},
        endpointNames: [`seg${n}`],
    }));
}

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
        // Endpoints 3-10: Segment lights
        ...segLightExtends,
    ],

    fromZigbee: [fzLocal.config, fzLocal.segments],
    toZigbee: [tzLocal.led_count, tzLocal.segments],

    exposes: [
        numericExpose('led_count', 'LED count', ACCESS_ALL,
            'Number of LEDs in the strip (reboot required after change)',
            {value_min: 1, value_max: 500, value_step: 1}),
        ...segExposes,
    ],

    meta: {
        multiEndpoint: true,
    },

    endpoint: (device) => {
        const eps = {rgb: 1, white: 2};
        for (let n = 1; n <= MAX_SEGMENTS; n++) {
            eps[`seg${n}`] = 2 + n;  // seg1=3, seg2=4, ...
        }
        return eps;
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
