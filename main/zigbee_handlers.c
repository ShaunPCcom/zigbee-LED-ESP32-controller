/**
 * @file zigbee_handlers.c
 * @brief Zigbee command and signal handlers implementation
 *
 * All 8 segment endpoints (EP1-EP8) are Extended Color Light devices.
 * HS/XY color mode drives RGB channels; CT mode drives the White channel.
 * Segment 1 (EP1) is the base layer covering the full strip by default.
 */

#include "zigbee_handlers.h"
#include "zigbee_init.h"
#include "led_driver.h"
#include "board_config.h"
#include "config_storage.h"
#include "segment_manager.h"
#include "preset_manager.h"
#include "transition_engine.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_level.h"
#include "zcl/esp_zigbee_zcl_color_control.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "zigbee_ota.h"
#include <math.h>

static const char *TAG = "zb_handler";

extern uint16_t g_strip_count[2];

bool s_network_joined = false;

/* Transient storage for save_name (for next save_slot operation) */
static char s_pending_save_name[PRESET_NAME_MAX + 1] = {0};

/* Debounce timer: avoid NVS write flood */
static esp_timer_handle_t s_save_timer = NULL;

/* Forward declarations */
void update_leds(void);
void schedule_save(void);
void sync_zcl_from_state(void);
static void restore_leds_cb(uint8_t param);
static void reboot_cb(uint8_t param);
static void sync_zcl_deferred_cb(uint8_t param);

/* ================================================================== */
/*  Color conversion helpers                                           */
/* ================================================================== */

static uint16_t zcl_hue_to_degrees(uint8_t zcl_hue)
{
    return (uint16_t)((uint32_t)zcl_hue * 360 / 254);
}

static void xy_to_rgb(uint16_t zcl_x, uint16_t zcl_y, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float x = (float)zcl_x / 65535.0f;
    float y = (float)zcl_y / 65535.0f;

    if (y < 0.0001f) { *r = *g = *b = 0; return; }

    float Y = 1.0f;
    float X = (Y / y) * x;
    float Z = (Y / y) * (1.0f - x - y);

    float rf =  X * 1.656492f - Y * 0.354851f - Z * 0.255038f;
    float gf = -X * 0.707196f + Y * 1.655397f + Z * 0.036152f;
    float bf =  X * 0.051713f - Y * 0.121364f + Z * 1.011530f;

    if (rf < 0.0f) rf = 0.0f;
    if (gf < 0.0f) gf = 0.0f;
    if (bf < 0.0f) bf = 0.0f;

    float max_c = rf > gf ? rf : gf;
    if (bf > max_c) max_c = bf;
    if (max_c > 1.0f) { rf /= max_c; gf /= max_c; bf /= max_c; }

    rf = rf <= 0.0031308f ? 12.92f * rf : 1.055f * powf(rf, 1.0f/2.4f) - 0.055f;
    gf = gf <= 0.0031308f ? 12.92f * gf : 1.055f * powf(gf, 1.0f/2.4f) - 0.055f;
    bf = bf <= 0.0031308f ? 12.92f * bf : 1.055f * powf(bf, 1.0f/2.4f) - 0.055f;

    *r = (uint8_t)(rf * 255.0f + 0.5f);
    *g = (uint8_t)(gf * 255.0f + 0.5f);
    *b = (uint8_t)(bf * 255.0f + 0.5f);
}

/**
 * @brief Normalize hue value to 0-360 range, handling wraparound.
 *
 * Converts wrapped negative values (e.g., -60 as 65476) back to 0-360.
 *
 * @param hue_raw Raw hue from transition engine (may be wrapped)
 * @return Normalized hue in 0-360 range
 */
static uint16_t normalize_hue(uint16_t hue_raw)
{
    if (hue_raw > 360) {
        /* Wrapped negative value (e.g., -60 = 65476 as uint16_t) */
        int16_t h = (int16_t)hue_raw;
        return (uint16_t)((h % 360) + 360);
    }
    return hue_raw % 360;
}

/**
 * @brief Calculate shortest hue arc for wraparound transitions.
 *
 * Adjusts target_hue so the transition takes the shortest path around
 * the color wheel. Result may be negative or > 360 (will wrap as uint16_t).
 *
 * @param current_hue Current hue value (0-360, normalized)
 * @param target_hue  Desired target hue (0-360, normalized)
 * @return Adjusted target for shortest arc (may be negative or > 360)
 */
static int16_t hue_shortest_arc(uint16_t current_hue, uint16_t target_hue)
{
    int16_t current = (int16_t)current_hue;
    int16_t target = (int16_t)target_hue;
    int16_t diff = target - current;

    /* If arc > 180°, go the short way */
    if (diff > 180) {
        target -= 360;  /* e.g., 10→300 becomes 10→-60 (20° arc through 0) */
    } else if (diff < -180) {
        target += 360;  /* e.g., 300→10 becomes 300→370 (70° arc through 360) */
    }

    return target;
}

/**
 * @brief Start a hue transition with automatic shortest-arc calculation.
 *
 * Always calculates shortest path around color wheel, even if current
 * transition value is wrapped from a previous arc-adjusted transition.
 *
 * @param hue_trans   Pointer to hue transition_t
 * @param target_hue  Desired target hue (0-360)
 * @param duration_ms Transition duration in milliseconds
 */
static void start_hue_transition(transition_t *hue_trans, uint16_t target_hue, uint32_t duration_ms)
{
    uint16_t current_hue_raw = transition_get_value(hue_trans);
    uint16_t current_hue = normalize_hue(current_hue_raw);
    int16_t adjusted_target = hue_shortest_arc(current_hue, target_hue);
    transition_start(hue_trans, (uint16_t)adjusted_target, duration_ms);
}

static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* Handle wraparound: negative values (wrapped as large uint16_t) */
    int16_t h_signed = (int16_t)h;
    if (h > 360) {
        /* Wrapped negative (e.g., -60 = 65476 as uint16_t) */
        h = (uint16_t)((h_signed % 360) + 360);
    } else {
        h %= 360;
    }
    if (s == 0) { *r = *g = *b = v; return; }

    uint8_t region    = h / 60;
    uint8_t remainder = (h - (region * 60)) * 6;
    uint8_t p = (v * (254 - s)) / 254;
    uint8_t q = (v * (254 - ((s * remainder) / 360))) / 254;
    uint8_t t = (v * (254 - ((s * (360 - remainder)) / 360))) / 254;

    switch (region) {
    case 0: *r = v; *g = t; *b = p; break;
    case 1: *r = q; *g = v; *b = p; break;
    case 2: *r = p; *g = v; *b = t; break;
    case 3: *r = p; *g = q; *b = v; break;
    case 4: *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}

/* ================================================================== */
/*  Debounce timers                                                    */
/* ================================================================== */

/* Color update timer removed — 200Hz render loop handles all updates */

static void save_timer_cb(void *arg)
{
    segment_manager_save();
}

void schedule_save(void)
{
    if (s_save_timer == NULL) {
        esp_timer_create_args_t args = { .callback = save_timer_cb, .name = "cfg_save" };
        esp_timer_create(&args, &s_save_timer);
    }
    esp_timer_stop(s_save_timer);
    esp_timer_start_once(s_save_timer, 500 * 1000);  /* 500ms */
}

/* ================================================================== */
/*  LED render loop (replaces old ZCL polling)                        */
/* ================================================================== */

/* Global transition duration for preset recalls and explicit color commands.
 * Units: milliseconds. 0 = instant. Default: 1000ms. */
static uint16_t g_global_transition_ms = 1000;

uint16_t zigbee_handlers_get_global_transition_ms(void)
{
    return g_global_transition_ms;
}

void zigbee_handlers_set_global_transition_ms(uint16_t ms)
{
    g_global_transition_ms = ms;
}

/* ================================================================== */
/*  LED render loop (200Hz via scheduler alarm)                        */
/*  Replaces old polling callback. Renders whatever the transition     */
/*  engine's current_values are at each call.                         */
/* ================================================================== */

static void led_render_cb(uint8_t param)
{
    /* Poll attributes (SDK handles some commands internally, no callbacks) */
    segment_light_t *state = segment_state_get();
    for (int n = 0; n < MAX_SEGMENTS; n++) {
        uint8_t ep = (uint8_t)(ZB_SEGMENT_EP_BASE + n);

        /* Read color mode to decide what to poll */
        esp_zb_zcl_attr_t *attr_mode = esp_zb_zcl_get_attribute(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID);
        uint8_t zcl_mode = (attr_mode && attr_mode->data_p) ? *(uint8_t *)attr_mode->data_p : 0;

        /* Read brightness (level) - applies to all modes */
        esp_zb_zcl_attr_t *attr_level = esp_zb_zcl_get_attribute(ep, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID);
        if (attr_level && attr_level->data_p) {
            uint8_t new_level = *(uint8_t *)attr_level->data_p;
            if (new_level != state[n].level) {
                state[n].level = new_level;
                transition_start(&state[n].level_trans, new_level, g_global_transition_ms);
            }
        }

        /* Only poll HS in HS/XY color modes (0 or 1), not CT mode (2) */
        if (zcl_mode <= 1) {
            /* Read enhanced hue (16-bit, 0-65535 maps to 0-360°) */
            esp_zb_zcl_attr_t *attr_hue = esp_zb_zcl_get_attribute(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID);
            if (attr_hue && attr_hue->data_p) {
                uint16_t enh_hue = *(uint16_t *)attr_hue->data_p;
                uint16_t new_hue = (uint16_t)((uint32_t)enh_hue * 360 / 65535);
                if (new_hue != state[n].hue) {
                    state[n].hue = new_hue;
                    state[n].color_mode = zcl_mode;
                    /* Instant color change (no transition) - hue wraparound needs debugging */
                    transition_start(&state[n].hue_trans, new_hue, 0);
                }
            }

            /* Read saturation */
            esp_zb_zcl_attr_t *attr_sat = esp_zb_zcl_get_attribute(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID);
            if (attr_sat && attr_sat->data_p) {
                uint8_t new_sat = *(uint8_t *)attr_sat->data_p;
                if (new_sat != state[n].saturation) {
                    state[n].saturation = new_sat;
                    /* Instant saturation change (no transition) */
                    transition_start(&state[n].sat_trans, new_sat, 0);
                }
            }
        } else if (zcl_mode == 2) {
            /* CT (white) mode - poll color temperature */
            esp_zb_zcl_attr_t *attr_ct = esp_zb_zcl_get_attribute(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID);
            if (attr_ct && attr_ct->data_p) {
                uint16_t new_ct = *(uint16_t *)attr_ct->data_p;
                if (new_ct != state[n].color_temp) {
                    state[n].color_temp = new_ct;
                    state[n].color_mode = 2;
                    transition_start(&state[n].ct_trans, new_ct, g_global_transition_ms);
                }
            }
        }
    }

    update_leds();
    esp_zb_scheduler_alarm(led_render_cb, 0, 5);
}

/* ================================================================== */
/*  ZCL attribute store sync (called once after stack starts)         */
/*  Pushes in-memory segment state (loaded from NVS) into the ZCL    */
/*  attribute store so Z2M/HA see the correct values on reconnect.   */
/* ================================================================== */

void sync_zcl_from_state(void)
{
    segment_light_t *state = segment_state_get();

    for (int n = 0; n < MAX_SEGMENTS; n++) {
        uint8_t ep = (uint8_t)(ZB_SEGMENT_EP_BASE + n);

        uint8_t on = state[n].on ? 1 : 0;
        esp_zb_zcl_set_attribute_val(ep, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, &on, false);

        esp_zb_zcl_set_attribute_val(ep, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ON_OFF_START_UP_ON_OFF,
            &state[n].startup_on_off, false);

        esp_zb_zcl_set_attribute_val(ep, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID,
            &state[n].level, false);

        esp_zb_zcl_set_attribute_val(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID,
            &state[n].color_mode, false);

        uint8_t hue8 = (uint8_t)((uint32_t)state[n].hue * 254 / 360);
        esp_zb_zcl_set_attribute_val(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID,
            &hue8, false);

        uint16_t enh_hue = (uint16_t)((uint32_t)state[n].hue * 65535 / 360);
        esp_zb_zcl_set_attribute_val(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID,
            &enh_hue, false);

        esp_zb_zcl_set_attribute_val(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID,
            &state[n].saturation, false);

        esp_zb_zcl_set_attribute_val(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID,
            &state[n].color_x, false);

        esp_zb_zcl_set_attribute_val(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID,
            &state[n].color_y, false);

        esp_zb_zcl_set_attribute_val(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID,
            &state[n].color_temp, false);
    }

    ESP_LOGI(TAG, "ZCL attribute store synced from saved state");
}

/* ================================================================== */
/*  LED rendering                                                      */
/*                                                                     */
/*  All segments rendered in order; last segment wins overlaps.       */
/*  Segment 1 (index 0) covers the full strip as the base layer.     */
/* ================================================================== */

void update_leds(void)
{
    segment_geom_t  *geom  = segment_geom_get();
    segment_light_t *state = segment_state_get();

    /* Clear both strip buffers */
    led_driver_clear(0);
    led_driver_clear(1);

    /* Render segments in order (1 first = base layer, 8 last = top overlay) */
    for (int n = 0; n < MAX_SEGMENTS; n++) {
        if (geom[n].count == 0) continue;

        uint8_t r = 0, g = 0, b = 0, w = 0;

        if (state[n].on) {
            /* Read interpolated values from transition engine */
            uint8_t  level = (uint8_t)transition_get_value(&state[n].level_trans);
            uint16_t hue   = transition_get_value(&state[n].hue_trans);
            uint8_t  sat   = (uint8_t)transition_get_value(&state[n].sat_trans);
            uint16_t ct    = transition_get_value(&state[n].ct_trans);

            if (state[n].color_mode == 2) {
                /* CT mode: drive White channel with brightness */
                w = level;
            } else if (state[n].color_mode == 1) {
                /* XY mode: color_x/y don't transition, use direct state */
                xy_to_rgb(state[n].color_x, state[n].color_y, &r, &g, &b);
                r = (uint8_t)((uint32_t)r * level / 254);
                g = (uint8_t)((uint32_t)g * level / 254);
                b = (uint8_t)((uint32_t)b * level / 254);
            } else {
                /* HS mode */
                hsv_to_rgb(hue, sat, 255, &r, &g, &b);
                r = (uint8_t)((uint32_t)r * level / 254);
                g = (uint8_t)((uint32_t)g * level / 254);
                b = (uint8_t)((uint32_t)b * level / 254);
            }
            (void)ct;  /* ct used in CT mode via level, kept for future use */
        }

        uint8_t strip = geom[n].strip_id;
        uint16_t strip_len = led_driver_get_count(strip);
        uint16_t end = geom[n].start + geom[n].count;
        if (end > strip_len) end = strip_len;
        for (uint16_t i = geom[n].start; i < end; i++) {
            led_driver_set_pixel(strip, i, r, g, b, w);
        }
    }

    led_driver_refresh();
}

/* ================================================================== */
/*  Preset attribute helpers                                           */
/* ================================================================== */

/**
 * @brief Update preset ZCL attributes (count, names, active) from preset manager
 */
static void update_preset_zcl_attrs(void)
{
    uint8_t count = (uint8_t)preset_manager_count();
    esp_zb_zcl_set_attribute_val(ZB_SEGMENT_EP_BASE, ZB_CLUSTER_PRESET_CONFIG,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_PRESET_COUNT, &count, false);

    /* Update active preset (deprecated) */
    const char *active = preset_manager_get_active();
    uint8_t active_buf[17] = {0};
    if (active && active[0] != '\0') {
        size_t len = strlen(active);
        if (len > PRESET_NAME_MAX) len = PRESET_NAME_MAX;
        active_buf[0] = (uint8_t)len;
        memcpy(&active_buf[1], active, len);
    }
    esp_zb_zcl_set_attribute_val(ZB_SEGMENT_EP_BASE, ZB_CLUSTER_PRESET_CONFIG,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_ACTIVE_PRESET, active_buf, false);

    /* Update preset name attributes (slots 0-7) */
    for (int n = 0; n < MAX_PRESET_SLOTS; n++) {
        char name[PRESET_NAME_MAX + 1];
        uint8_t name_buf[17] = {0};
        if (preset_manager_get_slot_name((uint8_t)n, name, sizeof(name)) == ESP_OK) {
            size_t len = strlen(name);
            name_buf[0] = (uint8_t)len;
            memcpy(&name_buf[1], name, len);
        } else {
            /* Empty slot: set default name */
            char default_name[PRESET_NAME_MAX + 1];
            int dlen = snprintf(default_name, sizeof(default_name), "Preset %d", n + 1);
            name_buf[0] = (uint8_t)dlen;
            memcpy(&name_buf[1], default_name, dlen);
        }
        esp_zb_zcl_set_attribute_val(ZB_SEGMENT_EP_BASE, ZB_CLUSTER_PRESET_CONFIG,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_PRESET_NAME_BASE + n, name_buf, false);
    }
}

/**
 * @brief Handle recall_slot write (0x0020)
 */
static esp_err_t handle_recall_slot_write(uint8_t slot)
{
    /* Validate slot range */
    if (slot > 7) {
        ESP_LOGE(TAG, "Invalid recall_slot %d (must be 0-7)", slot);
        return ESP_ERR_INVALID_ARG;
    }

    /* Recall preset */
    esp_err_t err = preset_manager_recall(slot);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Recalled preset from slot %d", slot);
        /* Start transitions from current engine values to new preset values */
        segment_light_t *state = segment_state_get();
        for (int i = 0; i < MAX_SEGMENTS; i++) {
            transition_start(&state[i].level_trans, state[i].level, g_global_transition_ms);
            transition_start(&state[i].hue_trans, state[i].hue, 0);  /* Instant - hue wraparound disabled */
            transition_start(&state[i].sat_trans, state[i].saturation, 0);  /* Instant */
            transition_start(&state[i].ct_trans, state[i].color_temp, g_global_transition_ms);
        }
        schedule_save();
        update_preset_zcl_attrs();
        /* Defer ZCL sync to avoid stack assertion */
        esp_zb_scheduler_alarm(sync_zcl_deferred_cb, 0, 100);
    } else if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Slot %d is empty, cannot recall", slot);
    } else {
        ESP_LOGE(TAG, "Failed to recall slot %d: %s", slot, esp_err_to_name(err));
    }

    /* Clear recall_slot attribute (0xFF = no pending action) */
    uint8_t clear = 0xFF;
    esp_zb_zcl_set_attribute_val(ZB_SEGMENT_EP_BASE, ZB_CLUSTER_PRESET_CONFIG,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_RECALL_SLOT, &clear, false);

    return err;
}

/**
 * @brief Handle save_slot write (0x0021)
 */
static esp_err_t handle_save_slot_write(uint8_t slot)
{
    /* Validate slot range */
    if (slot > 7) {
        ESP_LOGE(TAG, "Invalid save_slot %d (must be 0-7)", slot);
        return ESP_ERR_INVALID_ARG;
    }

    /* Use pending save name if set, otherwise NULL for default name */
    const char *name = (s_pending_save_name[0] != '\0') ? s_pending_save_name : NULL;

    /* Save preset */
    esp_err_t err = preset_manager_save(slot, name);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved preset to slot %d with name '%s'", slot, name ? name : "(default)");
        update_preset_zcl_attrs();
        /* Clear pending save name */
        memset(s_pending_save_name, 0, sizeof(s_pending_save_name));
        uint8_t empty_str[17] = {0};
        esp_zb_zcl_set_attribute_val(ZB_SEGMENT_EP_BASE, ZB_CLUSTER_PRESET_CONFIG,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_SAVE_NAME, empty_str, false);
    } else {
        ESP_LOGE(TAG, "Failed to save slot %d: %s", slot, esp_err_to_name(err));
    }

    /* Clear save_slot attribute (0xFF = no pending action) */
    uint8_t clear = 0xFF;
    esp_zb_zcl_set_attribute_val(ZB_SEGMENT_EP_BASE, ZB_CLUSTER_PRESET_CONFIG,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_SAVE_SLOT, &clear, false);

    return err;
}

/**
 * @brief Handle delete_slot write (0x0022)
 */
static esp_err_t handle_delete_slot_write(uint8_t slot)
{
    /* Validate slot range */
    if (slot > 7) {
        ESP_LOGE(TAG, "Invalid delete_slot %d (must be 0-7)", slot);
        return ESP_ERR_INVALID_ARG;
    }

    /* Delete preset */
    esp_err_t err = preset_manager_delete(slot);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Deleted preset from slot %d", slot);
        update_preset_zcl_attrs();
    } else {
        ESP_LOGE(TAG, "Failed to delete slot %d: %s", slot, esp_err_to_name(err));
    }

    /* Clear delete_slot attribute (0xFF = no pending action) */
    uint8_t clear = 0xFF;
    esp_zb_zcl_set_attribute_val(ZB_SEGMENT_EP_BASE, ZB_CLUSTER_PRESET_CONFIG,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_DELETE_SLOT, &clear, false);

    return err;
}

/**
 * @brief Handle save_name write (0x0023)
 */
static esp_err_t handle_save_name_write(const uint8_t *char_str)
{
    /* Parse CharString: first byte is length, rest is name */
    uint8_t name_len = char_str[0];
    if (name_len > PRESET_NAME_MAX) {
        name_len = PRESET_NAME_MAX;
    }

    /* Store in pending buffer */
    memcpy(s_pending_save_name, &char_str[1], name_len);
    s_pending_save_name[name_len] = '\0';

    ESP_LOGI(TAG, "Stored save_name: '%s' (for next save_slot operation)", s_pending_save_name);
    return ESP_OK;
}

/* ================================================================== */
/*  Attribute write handler                                            */
/* ================================================================== */

static esp_err_t handle_set_attr_value(const esp_zb_zcl_set_attr_value_message_t *message)
{
    if (!message || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) return ESP_OK;

    uint8_t  endpoint = message->info.dst_endpoint;
    uint16_t cluster  = message->info.cluster;
    uint16_t attr_id  = message->attribute.id;
    void    *value    = message->attribute.data.value;

    ESP_LOGD(TAG, "Attr: EP=%d cluster=0x%04X attr=0x%04X", endpoint, cluster, attr_id);

    /* Custom cluster: device config (EP1 only) */
    if (cluster == ZB_CLUSTER_DEVICE_CONFIG) {
        if (attr_id == ZB_ATTR_GLOBAL_TRANSITION_MS) {
            uint16_t ms = *(uint16_t *)value;
            g_global_transition_ms = ms;
            config_storage_save_global_transition_ms(ms);
            ESP_LOGI(TAG, "global_transition_ms -> %u ms", ms);
            return ESP_OK;
        }
        uint16_t new_count = *(uint16_t *)value;
        if (new_count >= 1 && new_count <= 500) {
            uint8_t strip = (attr_id == ZB_ATTR_STRIP2_COUNT) ? 1 : 0;
            ESP_LOGI(TAG, "Strip%d count -> %u (saving, reboot in 1s)", strip, new_count);
            config_storage_save_strip_count(strip, new_count);
            esp_zb_scheduler_alarm(reboot_cb, 0, 1000);
        }
        return ESP_OK;
    }

    /* Custom cluster: segment geometry (EP1 only) */
    if (cluster == ZB_CLUSTER_SEGMENT_CONFIG) {
        if (attr_id < ZB_ATTR_SEG_BASE + MAX_SEGMENTS * ZB_SEG_ATTRS_PER_SEG) {
            int offset  = attr_id - ZB_ATTR_SEG_BASE;
            int seg_idx = offset / ZB_SEG_ATTRS_PER_SEG;
            int field   = offset % ZB_SEG_ATTRS_PER_SEG;
            segment_geom_t *geom = segment_geom_get();
            if (field == 0) {
                geom[seg_idx].start = *(uint16_t *)value;
                ESP_LOGI(TAG, "Seg%d start -> %u", seg_idx + 1, geom[seg_idx].start);
            } else if (field == 1) {
                geom[seg_idx].count = *(uint16_t *)value;
                ESP_LOGI(TAG, "Seg%d count -> %u", seg_idx + 1, geom[seg_idx].count);
            } else {
                uint8_t v = *(uint8_t *)value;
                geom[seg_idx].strip_id = (v >= 2) ? 1 : 0;
                ESP_LOGI(TAG, "Seg%d strip -> %u", seg_idx + 1, geom[seg_idx].strip_id);
            }
            segment_manager_save();
            update_leds();
        }
        return ESP_OK;
    }

    /* Custom cluster: preset configuration (EP1 only) */
    if (cluster == ZB_CLUSTER_PRESET_CONFIG) {
        /* Handle slot-based attributes (Phase 3) */
        if (attr_id == ZB_ATTR_RECALL_SLOT) {
            uint8_t slot = *(uint8_t *)value;
            handle_recall_slot_write(slot);
            return ESP_OK;
        } else if (attr_id == ZB_ATTR_SAVE_SLOT) {
            uint8_t slot = *(uint8_t *)value;
            handle_save_slot_write(slot);
            return ESP_OK;
        } else if (attr_id == ZB_ATTR_DELETE_SLOT) {
            uint8_t slot = *(uint8_t *)value;
            handle_delete_slot_write(slot);
            return ESP_OK;
        } else if (attr_id == ZB_ATTR_SAVE_NAME) {
            uint8_t *char_str = (uint8_t *)value;
            handle_save_name_write(char_str);
            return ESP_OK;
        }

        /* Handle deprecated name-based attributes (backwards compatibility) */
        uint8_t *char_str = (uint8_t *)value;
        uint8_t name_len = char_str[0];
        if (name_len > PRESET_NAME_MAX) name_len = PRESET_NAME_MAX;
        char name[PRESET_NAME_MAX + 1];
        memcpy(name, &char_str[1], name_len);
        name[name_len] = '\0';

        if (attr_id == ZB_ATTR_RECALL_PRESET) {
            if (preset_manager_recall_by_name(name)) {
                ESP_LOGI(TAG, "Recalled preset '%s' (deprecated API)", name);
                /* Start transitions to new preset values */
                segment_light_t *state = segment_state_get();
                for (int i = 0; i < MAX_SEGMENTS; i++) {
                    transition_start(&state[i].level_trans, state[i].level, g_global_transition_ms);
                    transition_start(&state[i].hue_trans, state[i].hue, 0);  /* Instant - hue wraparound disabled */
                    transition_start(&state[i].sat_trans, state[i].saturation, 0);  /* Instant */
                    transition_start(&state[i].ct_trans, state[i].color_temp, g_global_transition_ms);
                }
                schedule_save();
                update_preset_zcl_attrs();
                /* Defer ZCL sync to avoid stack assertion */
                esp_zb_scheduler_alarm(sync_zcl_deferred_cb, 0, 100);
            } else {
                ESP_LOGW(TAG, "Preset '%s' not found", name);
            }
        } else if (attr_id == ZB_ATTR_SAVE_PRESET) {
            if (preset_manager_save_by_name(name)) {
                ESP_LOGI(TAG, "Saved preset '%s' (deprecated API)", name);
                update_preset_zcl_attrs();
            } else {
                ESP_LOGW(TAG, "Failed to save preset '%s'", name);
            }
        } else if (attr_id == ZB_ATTR_DELETE_PRESET) {
            if (preset_manager_delete_by_name(name)) {
                ESP_LOGI(TAG, "Deleted preset '%s' (deprecated API)", name);
                update_preset_zcl_attrs();
            } else {
                ESP_LOGW(TAG, "Preset '%s' not found", name);
            }
        }
        return ESP_OK;
    }

    /* Segment light endpoints (EP1-EP8) */
    if (endpoint >= ZB_SEGMENT_EP_BASE && endpoint < ZB_SEGMENT_EP_BASE + MAX_SEGMENTS) {
        int seg = endpoint - ZB_SEGMENT_EP_BASE;
        segment_light_t *state = segment_state_get();
        bool needs_update = false;

        if (cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
                state[seg].on = *(bool *)value;
                ESP_LOGI(TAG, "Seg%d on/off -> %s", seg + 1, state[seg].on ? "ON" : "OFF");
                /* Snap level_trans to current level instantly (on/off does not fade) */
                transition_start(&state[seg].level_trans, state[seg].level, 0);
                needs_update = true;
            } else if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_START_UP_ON_OFF) {
                state[seg].startup_on_off = *(uint8_t *)value;
                ESP_LOGI(TAG, "Seg%d startup_on_off -> 0x%02X", seg + 1, state[seg].startup_on_off);
                schedule_save();
            }
        } else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL) {
            if (attr_id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID) {
                state[seg].level = *(uint8_t *)value;
                ESP_LOGI(TAG, "Seg%d level -> %d", seg + 1, state[seg].level);
                /* Start transition to new level with global duration */
                transition_start(&state[seg].level_trans, state[seg].level, g_global_transition_ms);
                needs_update = true;
            }
        } else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL) {
            switch (attr_id) {
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID:
                state[seg].hue = zcl_hue_to_degrees(*(uint8_t *)value);
                state[seg].color_mode = 0;
                transition_start(&state[seg].hue_trans, state[seg].hue, 0);  /* Instant - hue wraparound disabled */
                break;
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID: {
                uint16_t eh = *(uint16_t *)value;
                state[seg].hue = (uint16_t)((uint32_t)eh * 360 / 65535);
                state[seg].color_mode = 0;
                transition_start(&state[seg].hue_trans, state[seg].hue, 0);  /* Instant - hue wraparound disabled */
                break;
            }
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID:
                state[seg].saturation = *(uint8_t *)value;
                transition_start(&state[seg].sat_trans, state[seg].saturation, g_global_transition_ms);
                break;
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID:
                state[seg].color_x = *(uint16_t *)value;
                state[seg].color_mode = 1;
                break;
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID:
                state[seg].color_y = *(uint16_t *)value;
                state[seg].color_mode = 1;
                break;
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID:
                state[seg].color_temp = *(uint16_t *)value;
                state[seg].color_mode = 2;
                ESP_LOGI(TAG, "Seg%d CT -> %u mireds", seg + 1, state[seg].color_temp);
                transition_start(&state[seg].ct_trans, state[seg].color_temp, g_global_transition_ms);
                needs_update = true;
                break;
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID:
                state[seg].color_mode = *(uint8_t *)value;
                ESP_LOGI(TAG, "Seg%d color_mode -> %d", seg + 1, state[seg].color_mode);
                needs_update = true;
                break;
            default:
                break;
            }
        }

        if (needs_update) { update_leds(); schedule_save(); }
    }

    return ESP_OK;
}

/* ================================================================== */
/*  Public action handler                                              */
/* ================================================================== */

esp_err_t zigbee_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    /* Route OTA callbacks to OTA component */
    esp_err_t ota_ret = zigbee_ota_action_handler(callback_id, message);
    if (ota_ret != ESP_ERR_NOT_SUPPORTED) {
        return ota_ret;  /* OTA component handled it */
    }

    esp_err_t ret = ESP_OK;
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        ret = handle_set_attr_value((const esp_zb_zcl_set_attr_value_message_t *)message);
        break;
    default:
        ESP_LOGD(TAG, "Unhandled callback: 0x%x", callback_id);
        break;
    }
    return ret;
}

/* ================================================================== */
/*  Signal handler                                                     */
/* ================================================================== */

static void steering_retry_cb(uint8_t param)
{
    ESP_LOGI(TAG, "Retrying network steering...");
    board_led_set_state_pairing();
    esp_zb_bdb_start_top_level_commissioning(param);
}

static void reboot_cb(uint8_t param)
{
    (void)param;
    esp_restart();
}

static void sync_zcl_deferred_cb(uint8_t param)
{
    (void)param;
    ESP_LOGI(TAG, "Deferred ZCL sync after preset recall");
    sync_zcl_from_state();
}

void schedule_zcl_sync(void)
{
    esp_zb_scheduler_alarm(sync_zcl_deferred_cb, 0, 100);
}

static void restore_leds_cb(uint8_t param)
{
    (void)param;
    update_leds();
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct ? signal_struct->p_app_signal : NULL;
    esp_zb_app_signal_type_t sig = p_sg_p ? *p_sg_p : 0;
    esp_err_t status = signal_struct ? signal_struct->esp_err_status : ESP_OK;

    switch (sig) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Stack initialized, starting network steering");
        sync_zcl_from_state();
        board_led_set_state_pairing();
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_NETWORK_STEERING);
        esp_zb_scheduler_alarm(led_render_cb, 0, 50);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (status == ESP_OK) {
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Factory new device, starting network steering");
                board_led_set_state_pairing();
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device rebooted, already joined network");
                board_led_set_state_joined();
                s_network_joined = true;
                esp_zb_scheduler_alarm(restore_leds_cb, 0, 5500);
            }
        } else {
            ESP_LOGE(TAG, "Device start/reboot failed: %s", esp_err_to_name(status));
            board_led_set_state_error();
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (status == ESP_OK) {
            ESP_LOGI(TAG, "Successfully joined Zigbee network!");
            board_led_set_state_joined();
            s_network_joined = true;
            esp_zb_scheduler_alarm(restore_leds_cb, 0, 5500);
        } else {
            ESP_LOGW(TAG, "Network steering failed (%s), retrying in 5s...", esp_err_to_name(status));
            board_led_set_state_error();
            esp_zb_scheduler_alarm(steering_retry_cb, ESP_ZB_BDB_NETWORK_STEERING, 5000);
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE:
        ESP_LOGW(TAG, "Left Zigbee network");
        board_led_set_state_not_joined();
        s_network_joined = false;
        led_driver_clear(0);
        led_driver_clear(1);
        led_driver_refresh();
        esp_zb_scheduler_alarm(steering_retry_cb, ESP_ZB_BDB_NETWORK_STEERING, 1000);
        break;

    case ESP_ZB_COMMON_SIGNAL_CAN_SLEEP:
        break;

    default:
        ESP_LOGI(TAG, "Zigbee signal: 0x%x, status: %s", sig, esp_err_to_name(status));
        break;
    }
}

/* ================================================================== */
/*  Factory reset functions                                            */
/* ================================================================== */

void zigbee_factory_reset(void)
{
    ESP_LOGW(TAG, "Zigbee network reset - leaving network, keeping config");
    board_led_set_state_error();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_zb_factory_reset();
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

void zigbee_full_factory_reset(void)
{
    ESP_LOGW(TAG, "FULL factory reset - erasing Zigbee network + NVS config");
    board_led_set_state_error();
    vTaskDelay(pdMS_TO_TICKS(200));

    nvs_handle_t h;
    if (nvs_open("led_cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "NVS config erased");
    }

    esp_zb_factory_reset();
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

/* ================================================================== */
/*  Boot button monitor: 3s = Zigbee reset, 10s = full factory reset  */
/* ================================================================== */

/* Button task removed - now handled by ButtonHandler C++ class in main.cpp */
