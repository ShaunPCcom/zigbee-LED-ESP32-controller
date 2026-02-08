/**
 * Zigbee2MQTT External Converter for ZB_LED_CTRL
 *
 * Device: 8 virtual segments on dual LED strips (EP1-EP8).
 * Each segment is an Extended Color Light:
 *   - Color mode (HS/XY) drives RGB channels
 *   - Color temperature mode drives the White channel
 *
 * Segment 1 (EP1) defaults to covering the full strip and acts as the base layer.
 * Segments 2-8 overlay their configured LED range on top.
 *
 * Custom clusters (on EP1):
 *   0xFC00: Device config (strip1_count, strip2_count — reboot required after change)
 *   0xFC01: Segment geometry (start + count + strip per segment, 3 attrs × 8 = 24 total)
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
const ACCESS_ALL = 0b111;

// ---- Custom cluster definitions ----
const CLUSTER_DEVICE_CONFIG  = 0xFC00;
const CLUSTER_SEGMENT_CONFIG = 0xFC01;
const MAX_SEGMENTS = 8;

// Device config attributes: led_count (compat alias), strip1_count, strip2_count
const ledCtrlConfigCluster = {
    ID: CLUSTER_DEVICE_CONFIG,
    attributes: {
        ledCount:    {ID: 0x0000, type: ZCL_UINT16, write: true},
        strip1Count: {ID: 0x0001, type: ZCL_UINT16, write: true},
        strip2Count: {ID: 0x0002, type: ZCL_UINT16, write: true},
    },
    commands: {},
    commandsResponse: {},
};

// Segment geometry attributes: 3 per segment (start, count, strip), base = n * 3
const segAttrs = {};
for (let n = 0; n < MAX_SEGMENTS; n++) {
    const base = n * 3;
    segAttrs[`seg${n}Start`] = {ID: base + 0, type: ZCL_UINT16, write: true};
    segAttrs[`seg${n}Count`] = {ID: base + 1, type: ZCL_UINT16, write: true};
    segAttrs[`seg${n}Strip`] = {ID: base + 2, type: ZCL_UINT8,  write: true};
}

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
            if (msg.data.ledCount    !== undefined) result.led_count    = msg.data.ledCount;
            if (msg.data.strip1Count !== undefined) result.strip1_count = msg.data.strip1Count;
            if (msg.data.strip2Count !== undefined) result.strip2_count = msg.data.strip2Count;
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
                if (msg.data[`seg${n}Strip`] !== undefined) result[`seg${s}_strip`] = msg.data[`seg${n}Strip`];
            }
            return result;
        },
    },
};

// ---- toZigbee ----
const tzLocal = {
    strip_counts: {
        key: ['strip1_count', 'strip2_count'],
        convertSet: async (entity, key, value, meta) => {
            registerCustomClusters(meta.device);
            const ep = meta.device.getEndpoint(1);
            if (key === 'strip1_count') {
                await ep.write('ledCtrlConfig', {strip1Count: value});
            } else {
                await ep.write('ledCtrlConfig', {strip2Count: value});
            }
            return {state: {[key]: value}};
        },
        convertGet: async (entity, key, meta) => {
            registerCustomClusters(meta.device);
            const ep = meta.device.getEndpoint(1);
            const attr = key === 'strip1_count' ? 'strip1Count' : 'strip2Count';
            await ep.read('ledCtrlConfig', [attr]);
        },
    },
    segments: {
        key: [],
        convertSet: async (entity, key, value, meta) => {
            registerCustomClusters(meta.device);
            const ep = meta.device.getEndpoint(1);
            const m = key.match(/^seg(\d+)_(start|count|strip)$/);
            if (!m) return;
            const n = parseInt(m[1]) - 1;
            const field = m[2];
            const attrMap = {start: `seg${n}Start`, count: `seg${n}Count`, strip: `seg${n}Strip`};
            await ep.write('segmentConfig', {[attrMap[field]]: value});
            return {state: {[key]: value}};
        },
        convertGet: async (entity, key, meta) => {
            registerCustomClusters(meta.device);
            const ep = meta.device.getEndpoint(1);
            const m = key.match(/^seg(\d+)_(start|count|strip)$/);
            if (!m) return;
            const n = parseInt(m[1]) - 1;
            const field = m[2];
            const attrMap = {start: `seg${n}Start`, count: `seg${n}Count`, strip: `seg${n}Strip`};
            await ep.read('segmentConfig', [attrMap[field]]);
        },
    },
};

for (let n = 1; n <= MAX_SEGMENTS; n++) {
    tzLocal.segments.key.push(`seg${n}_start`, `seg${n}_count`, `seg${n}_strip`);
}

// ---- Segment geometry exposes ----
const segExposes = [];
for (let n = 1; n <= MAX_SEGMENTS; n++) {
    segExposes.push(
        numericExpose(`seg${n}_start`, `Seg${n} start`, ACCESS_ALL,
            `Segment ${n} first LED index`, {value_min: 0, value_max: 65535, value_step: 1}),
        numericExpose(`seg${n}_count`, `Seg${n} count`, ACCESS_ALL,
            `Segment ${n} LED count (0 = disabled)`, {value_min: 0, value_max: 65535, value_step: 1}),
        numericExpose(`seg${n}_strip`, `Seg${n} strip`, ACCESS_ALL,
            `Segment ${n} physical strip (1 or 2)`, {value_min: 1, value_max: 2, value_step: 1}),
    );
}

// ---- 8 segment light extends (EP1-EP8) ----
// Each segment is a Color Dimmable Light with both color (HS) and color_temp (CT=white)
const segLightExtends = [];
for (let n = 1; n <= MAX_SEGMENTS; n++) {
    segLightExtends.push(light({
        color: {modes: ['hs'], enhancedHue: true},
        colorTemp: {range: [153, 370]},
        endpointNames: [`seg${n}`],
    }));
}

// ---- Device definition ----
const definition = {
    zigbeeModel: ['ZB_LED_CTRL'],
    model: 'ZB_LED_CTRL',
    vendor: 'DIY',
    description: 'Zigbee LED Strip Controller (ESP32-H2) — 8 RGBW segments, dual strip',

    extend: segLightExtends,

    fromZigbee: [fzLocal.config, fzLocal.segments],
    toZigbee: [tzLocal.strip_counts, tzLocal.segments],

    exposes: [
        numericExpose('strip1_count', 'Strip 1 count', ACCESS_ALL,
            'Number of LEDs on strip 1 (reboot required after change)',
            {value_min: 1, value_max: 500, value_step: 1}),
        numericExpose('strip2_count', 'Strip 2 count', ACCESS_ALL,
            'Number of LEDs on strip 2 (0 = disabled, reboot required after change)',
            {value_min: 0, value_max: 500, value_step: 1}),
        ...segExposes,
    ],

    meta: {
        multiEndpoint: true,
    },

    endpoint: (device) => {
        const eps = {};
        for (let n = 1; n <= MAX_SEGMENTS; n++) {
            eps[`seg${n}`] = n;  // seg1=EP1, seg2=EP2, ...
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
        await ep1.read('ledCtrlConfig', ['strip1Count', 'strip2Count']);
    },
};

module.exports = definition;
