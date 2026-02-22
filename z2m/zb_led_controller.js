/**
 * Zigbee2MQTT External Converter for ZB_LED_CTRL
 *
 * Device: 8 virtual segments on dual LED strips (EP1-EP8).
 * Each segment is an Extended Color Light:
 *   - Color mode (Enhanced Hue 16-bit + Saturation) drives RGB channels
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

// Device config attributes: led_count (compat alias), strip1_count, strip2_count, global_transition_ms
const ledCtrlConfigCluster = {
    ID: CLUSTER_DEVICE_CONFIG,
    attributes: {
        ledCount:            {ID: 0x0000, type: ZCL_UINT16, write: true},
        strip1Count:         {ID: 0x0001, type: ZCL_UINT16, write: true},
        strip2Count:         {ID: 0x0002, type: ZCL_UINT16, write: true},
        globalTransitionMs:  {ID: 0x0003, type: ZCL_UINT16, write: true},
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
    activePreset:   {ID: 0x0001, type: ZCL_CHAR_STRING},  // DEPRECATED
    recallPreset:   {ID: 0x0002, type: ZCL_CHAR_STRING, write: true},  // DEPRECATED
    savePreset:     {ID: 0x0003, type: ZCL_CHAR_STRING, write: true},  // DEPRECATED
    deletePreset:   {ID: 0x0004, type: ZCL_CHAR_STRING, write: true},  // DEPRECATED
    recallSlot:     {ID: 0x0020, type: ZCL_UINT8, write: true},
    saveSlot:       {ID: 0x0021, type: ZCL_UINT8, write: true},
    deleteSlot:     {ID: 0x0022, type: ZCL_UINT8, write: true},
    saveName:       {ID: 0x0023, type: ZCL_CHAR_STRING, write: true},
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
            if (msg.data.ledCount           !== undefined) result.led_count            = msg.data.ledCount;
            if (msg.data.strip1Count        !== undefined) result.strip1_count         = msg.data.strip1Count;
            if (msg.data.strip2Count        !== undefined) result.strip2_count         = msg.data.strip2Count;
            if (msg.data.globalTransitionMs !== undefined) result.global_transition_ms = msg.data.globalTransitionMs;
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

            // Publish individual preset names for HA visibility (0-indexed)
            for (let n = 0; n < MAX_PRESETS; n++) {
                if (msg.data[`preset${n}Name`] !== undefined) {
                    const name = msg.data[`preset${n}Name`];
                    // Show "(empty)" for empty or default names
                    if (!name || name === '' || name === `Preset ${n + 1}`) {
                        result[`preset_${n}_name`] = '(empty)';
                    } else {
                        result[`preset_${n}_name`] = name;
                    }
                }
            }

            return result;
        },
    },
};

// ---- toZigbee ----
const tzLocal = {
    strip_counts: {
        key: ['strip1_count', 'strip2_count', 'global_transition_ms'],
        convertSet: async (entity, key, value, meta) => {
            registerCustomClusters(meta.device);
            const ep = meta.device.getEndpoint(1);
            if (key === 'strip1_count') {
                await ep.write('ledCtrlConfig', {strip1Count: value});
            } else if (key === 'strip2_count') {
                await ep.write('ledCtrlConfig', {strip2Count: value});
            } else if (key === 'global_transition_ms') {
                await ep.write('ledCtrlConfig', {globalTransitionMs: value});
            }
            return {state: {[key]: value}};
        },
        convertGet: async (entity, key, meta) => {
            registerCustomClusters(meta.device);
            const ep = meta.device.getEndpoint(1);
            const attrMap = {strip1_count: 'strip1Count', strip2_count: 'strip2Count', global_transition_ms: 'globalTransitionMs'};
            await ep.read('ledCtrlConfig', [attrMap[key]]);
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
        key: ['preset_slot', 'new_preset_name', 'apply_preset', 'save_preset', 'delete_preset'],
        convertSet: async (entity, key, value, meta) => {
            registerCustomClusters(meta.device);
            const ep = meta.device.getEndpoint(1);

            if (key === 'preset_slot') {
                // Store selected slot in meta.state (no device write)
                return {state: {preset_slot: value}};
            }

            if (key === 'new_preset_name') {
                // Store new preset name in meta.state (no device write)
                return {state: {new_preset_name: value}};
            }

            if (key === 'apply_preset') {
                // Read preset_slot from meta.state and recall
                const slot = meta.state.preset_slot;
                if (slot !== undefined && slot !== '') {
                    const slotNum = parseInt(slot);
                    await ep.write('presetConfig', {recallSlot: slotNum});

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
                                    'colorTemperature', 'colorMode'
                                ]);
                            } catch (error) {
                                // Segment might be disabled, skip
                            }
                        }
                    }
                }
                return {state: {apply_preset: ''}};  // Clear action field
            }

            if (key === 'save_preset') {
                // Read new_preset_name and preset_slot from meta.state
                const name = meta.state.new_preset_name;
                const slot = meta.state.preset_slot;

                if (slot !== undefined && slot !== '') {
                    const slotNum = parseInt(slot);

                    // Write name if provided (optional)
                    if (name && name.length > 0) {
                        await ep.write('presetConfig', {saveName: name});
                    }

                    // Write slot to save
                    await ep.write('presetConfig', {saveSlot: slotNum});

                    // Wait for device to save, then re-read that slot's name
                    await new Promise(resolve => setTimeout(resolve, 500));
                    await ep.read('presetConfig', [`preset${slotNum}Name`]);
                }
                return {state: {save_preset: ''}};  // Clear action field only
            }

            if (key === 'delete_preset') {
                // Read preset_slot from meta.state and delete
                const slot = meta.state.preset_slot;
                if (slot !== undefined && slot !== '') {
                    const slotNum = parseInt(slot);
                    await ep.write('presetConfig', {deleteSlot: slotNum});

                    // Wait for device to delete, then re-read that slot's name
                    await new Promise(resolve => setTimeout(resolve, 500));
                    await ep.read('presetConfig', [`preset${slotNum}Name`]);
                }
                return {state: {delete_preset: ''}};  // Clear action field only
            }
        },
        convertGet: async (entity, key, meta) => {
            registerCustomClusters(meta.device);
            const ep = meta.device.getEndpoint(1);
            // No GET operations needed for slot-based system
        },
    },
};

for (let n = 1; n <= MAX_SEGMENTS; n++) {
    tzLocal.segments.key.push(`seg${n}_start`, `seg${n}_count`, `seg${n}_strip`);
}

// No additional keys needed - all handled in main key array

// ---- Preset exposes ----
function textExpose(name, label, access, description) {
    return {type: 'text', name, label, property: name, access, description};
}

function enumExpose(name, label, access, description, values) {
    return {type: 'enum', name, label, property: name, access, description, values};
}

const presetExposes = [
    // Preset slot selector
    enumExpose('preset_slot', 'Preset Slot', ACCESS_ALL,
        'Select preset slot (0-7)',
        ['0', '1', '2', '3', '4', '5', '6', '7']),

    // Action buttons
    enumExpose('apply_preset', 'Apply Preset', ACCESS_WRITE,
        'Activate selected preset', ['Apply']),
    enumExpose('delete_preset', 'Delete Preset', ACCESS_WRITE,
        'Delete selected preset', ['Delete']),

    // Save workflow
    textExpose('new_preset_name', 'New Preset Name', ACCESS_ALL,
        'Name for new preset (max 16 chars)'),
    enumExpose('save_preset', 'Save Preset', ACCESS_WRITE,
        'Save current state to selected slot', ['Save']),

    // 8 read-only slot name sensors
    textExpose('preset_0_name', 'Slot 0 Name', ACCESS_READ, 'Name of preset in slot 0'),
    textExpose('preset_1_name', 'Slot 1 Name', ACCESS_READ, 'Name of preset in slot 1'),
    textExpose('preset_2_name', 'Slot 2 Name', ACCESS_READ, 'Name of preset in slot 2'),
    textExpose('preset_3_name', 'Slot 3 Name', ACCESS_READ, 'Name of preset in slot 3'),
    textExpose('preset_4_name', 'Slot 4 Name', ACCESS_READ, 'Name of preset in slot 4'),
    textExpose('preset_5_name', 'Slot 5 Name', ACCESS_READ, 'Name of preset in slot 5'),
    textExpose('preset_6_name', 'Slot 6 Name', ACCESS_READ, 'Name of preset in slot 6'),
    textExpose('preset_7_name', 'Slot 7 Name', ACCESS_READ, 'Name of preset in slot 7'),
];

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
// Each segment is a Color Dimmable Light with enhanced hue (16-bit precision) and color_temp (CT=white)
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
    ota: true,  // Enable OTA update support

    exposes: [
        numericExpose('strip1_count', 'Strip 1 count', ACCESS_ALL,
            'Number of LEDs on strip 1 (reboot required after change)',
            {value_min: 1, value_max: 500, value_step: 1}),
        numericExpose('strip2_count', 'Strip 2 count', ACCESS_ALL,
            'Number of LEDs on strip 2 (0 = disabled, reboot required after change)',
            {value_min: 0, value_max: 500, value_step: 1}),
        numericExpose('global_transition_ms', 'Global transition time', ACCESS_ALL,
            'Default transition duration in milliseconds for color and brightness changes',
            {value_min: 0, value_max: 65535, value_step: 100, unit: 'ms'}),
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

    configure: async (device, coordinatorEndpoint, logger) => {
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
