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
const ZCL_CHAR_STRING = 0x42;

// ---- Expose access flags ----
const ACCESS_ALL = 0b111;
const ACCESS_READ = 0b001;
const ACCESS_WRITE = 0b010;

// ---- Custom cluster definitions ----
const CLUSTER_DEVICE_CONFIG  = 0xFC00;
const CLUSTER_SEGMENT_CONFIG = 0xFC01;
const CLUSTER_PRESET_CONFIG  = 0xFC02;
const MAX_SEGMENTS = 8;
const MAX_PRESETS = 8;

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

// Preset config cluster attributes
const presetAttrs = {
    presetCount:    {ID: 0x0000, type: ZCL_UINT8},
    activePreset:   {ID: 0x0001, type: ZCL_CHAR_STRING},
    recallPreset:   {ID: 0x0002, type: ZCL_CHAR_STRING, write: true},
    savePreset:     {ID: 0x0003, type: ZCL_CHAR_STRING, write: true},
    deletePreset:   {ID: 0x0004, type: ZCL_CHAR_STRING, write: true},
};
for (let n = 0; n < MAX_PRESETS; n++) {
    presetAttrs[`preset${n}Name`] = {ID: 0x0010 + n, type: ZCL_CHAR_STRING};
}

const presetConfigCluster = {
    ID: CLUSTER_PRESET_CONFIG,
    attributes: presetAttrs,
    commands: {},
    commandsResponse: {},
};

function registerCustomClusters(device) {
    device.addCustomCluster('ledCtrlConfig', ledCtrlConfigCluster);
    device.addCustomCluster('segmentConfig', segmentConfigCluster);
    device.addCustomCluster('presetConfig', presetConfigCluster);
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
    presets: {
        cluster: 'presetConfig',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            const result = {};
            if (msg.data.presetCount !== undefined) result.preset_count = msg.data.presetCount;
            if (msg.data.activePreset !== undefined) result.active_preset = msg.data.activePreset;

            // Publish individual preset names for HA visibility
            for (let n = 0; n < MAX_PRESETS; n++) {
                if (msg.data[`preset${n}Name`] !== undefined) {
                    result[`preset_${n + 1}_name`] = msg.data[`preset${n}Name`];
                }
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
    presets: {
        key: ['save_preset', 'preset_selector', 'recall_action', 'delete_action'],
        convertSet: async (entity, key, value, meta) => {
            registerCustomClusters(meta.device);
            const ep = meta.device.getEndpoint(1);

            if (key === 'save_preset') {
                // User typed a preset name, save current state
                if (value && value.length > 0) {
                    await ep.write('presetConfig', {savePreset: value});
                    // Re-read preset list to update dropdown
                    setTimeout(async () => {
                        const presetNameAttrs = [];
                        for (let n = 0; n < MAX_PRESETS; n++) {
                            presetNameAttrs.push(`preset${n}Name`);
                        }
                        await ep.read('presetConfig', ['presetCount', ...presetNameAttrs]);
                    }, 500);
                }
                return {state: {save_preset: ''}};  // Clear input after save
            }

            if (key === 'preset_selector') {
                // User selected a preset from dropdown, store selection
                return {state: {preset_selector: value}};
            }

            if (key === 'recall_action') {
                // User clicked recall - use preset_selector value
                const selectedPreset = meta.state.preset_selector;
                if (selectedPreset && selectedPreset.length > 0) {
                    await ep.write('presetConfig', {recallPreset: selectedPreset});

                    // Wait for device's deferred ZCL sync (100ms) + safety margin
                    await new Promise(resolve => setTimeout(resolve, 200));

                    // Re-read all segment states to update HA UI
                    for (let n = 1; n <= MAX_SEGMENTS; n++) {
                        const segEp = meta.device.getEndpoint(n);
                        if (segEp) {
                            try {
                                await segEp.read('genOnOff', ['onOff']);
                                await segEp.read('genLevelCtrl', ['currentLevel']);
                                await segEp.read('lightingColorCtrl', [
                                    'enhancedCurrentHue', 'currentSaturation',
                                    'currentX', 'currentY',
                                    'colorTemperature', 'colorMode'
                                ]);
                            } catch (error) {
                                // Segment might be disabled, skip
                            }
                        }
                    }
                }
                return {state: {recall_action: ''}};  // Clear action field
            }

            if (key === 'delete_action') {
                // User clicked delete - use preset_selector value
                const selectedPreset = meta.state.preset_selector;
                if (selectedPreset && selectedPreset.length > 0) {
                    await ep.write('presetConfig', {deletePreset: selectedPreset});
                    // Re-read preset list to update dropdown
                    setTimeout(async () => {
                        const presetNameAttrs = [];
                        for (let n = 0; n < MAX_PRESETS; n++) {
                            presetNameAttrs.push(`preset${n}Name`);
                        }
                        await ep.read('presetConfig', ['presetCount', ...presetNameAttrs]);
                    }, 500);
                }
                return {state: {delete_action: '', preset_selector: ''}};  // Clear both fields
            }
        },
        convertGet: async (entity, key, meta) => {
            registerCustomClusters(meta.device);
            const ep = meta.device.getEndpoint(1);
            if (key === 'preset_count' || key === 'active_preset') {
                await ep.read('presetConfig', ['presetCount', 'activePreset']);
            } else {
                const m = key.match(/^preset_(\d+)_name$/);
                if (m) {
                    const n = parseInt(m[1]) - 1;
                    await ep.read('presetConfig', [`preset${n}Name`]);
                }
            }
        },
    },
};

for (let n = 1; n <= MAX_SEGMENTS; n++) {
    tzLocal.segments.key.push(`seg${n}_start`, `seg${n}_count`, `seg${n}_strip`);
}

// Add preset get keys
for (let n = 1; n <= MAX_PRESETS; n++) {
    tzLocal.presets.key.push(`preset_${n}_name`);
}
tzLocal.presets.key.push('preset_count', 'active_preset');

// ---- Preset exposes ----
function textExpose(name, label, access, description) {
    return {type: 'text', name, label, property: name, access, description};
}

function enumExpose(name, label, access, description, values) {
    return {type: 'enum', name, label, property: name, access, description, values};
}

const presetExposes = [
    numericExpose('preset_count', 'Preset count', ACCESS_READ,
        'Number of stored presets (0-8)', {value_min: 0, value_max: 8}),
    textExpose('active_preset', 'Active preset', ACCESS_READ,
        'Name of last recalled preset'),
    textExpose('save_preset', 'Save new preset', ACCESS_WRITE,
        'Type preset name and submit to save current segment states'),
    textExpose('preset_selector', 'Preset name', ACCESS_ALL,
        'Type or paste preset name here for recall/delete actions'),
    textExpose('recall_action', 'Recall preset', ACCESS_WRITE,
        'Write any value (e.g. "go") to activate preset from Preset name field'),
    textExpose('delete_action', 'Delete preset', ACCESS_WRITE,
        'Write any value (e.g. "delete") to remove preset from Preset name field'),
];

// Add individual preset name fields for visibility in HA
for (let n = 1; n <= MAX_PRESETS; n++) {
    presetExposes.push(
        textExpose(`preset_${n}_name`, `Preset ${n} name`, ACCESS_READ,
            `Name stored in preset slot ${n} (empty if unused)`)
    );
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
        powerOnBehavior: true,
        effect: false,
    }));
}

// ---- Device definition ----
const definition = {
    zigbeeModel: ['ZB_LED_CTRL'],
    model: 'ZB_LED_CTRL',
    vendor: 'DIY',
    description: 'Zigbee LED Strip Controller (ESP32-H2) — 8 RGBW segments, dual strip',

    extend: segLightExtends,

    fromZigbee: [fzLocal.config, fzLocal.segments, fzLocal.presets],
    toZigbee: [tzLocal.strip_counts, tzLocal.segments, tzLocal.presets],

    exposes: [
        numericExpose('strip1_count', 'Strip 1 count', ACCESS_ALL,
            'Number of LEDs on strip 1 (reboot required after change)',
            {value_min: 1, value_max: 500, value_step: 1}),
        numericExpose('strip2_count', 'Strip 2 count', ACCESS_ALL,
            'Number of LEDs on strip 2 (0 = disabled, reboot required after change)',
            {value_min: 0, value_max: 500, value_step: 1}),
        ...segExposes,
        ...presetExposes,
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

        // Read all segment geometry so Z2M state reflects device NVS on re-interview
        const segGeomAttrs = [];
        for (let n = 0; n < MAX_SEGMENTS; n++) {
            segGeomAttrs.push(`seg${n}Start`, `seg${n}Count`, `seg${n}Strip`);
        }
        await ep1.read('segmentConfig', segGeomAttrs);

        // Read preset configuration
        await ep1.read('presetConfig', ['presetCount', 'activePreset']);
        const presetNameAttrs = [];
        for (let n = 0; n < MAX_PRESETS; n++) {
            presetNameAttrs.push(`preset${n}Name`);
        }
        await ep1.read('presetConfig', presetNameAttrs);
    },
};

module.exports = definition;
