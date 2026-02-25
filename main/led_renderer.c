/**
 * @file led_renderer.c
 * @brief LED render loop, ZCL polling, and state synchronization
 */

#include "led_renderer.h"
#include "segment_manager.h"
#include "color_engine.h"
#include "led_driver.h"
#include "transition_engine.h"
#include "config_storage.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_zigbee_core.h"

static const char *TAG = "led_renderer";

/* ================================================================== */
/*  Configuration Save Timer                                          */
/* ================================================================== */

static esp_timer_handle_t s_save_timer = NULL;

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
/*  Global Transition Duration                                        */
/* ================================================================== */

/* Global transition duration for preset recalls and explicit color commands.
 * Units: milliseconds. 0 = instant. Default: 100ms (smoothing filter).
 *
 * SMOOTHING FILTER: 100ms provides smooth interpolation between SDK updates.
 * When HA sends a timed transition, the Zigbee SDK interpolates values internally
 * and updates attributes at discrete intervals. The firmware transition engine acts
 * as a smoothing filter, creating smooth 200Hz interpolation between SDK updates.
 * 100ms is fast enough to feel instant while eliminating visible stepping.
 */
uint16_t g_global_transition_ms = 100;

uint16_t led_renderer_get_global_transition_ms(void)
{
    return g_global_transition_ms;
}

void led_renderer_set_global_transition_ms(uint16_t ms)
{
    g_global_transition_ms = ms;
}

/* ================================================================== */
/*  ZCL Attribute Store Synchronization                               */
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

        /* Sync enhanced hue (16-bit) - convert from degrees to ZCL format */
        uint16_t enh_hue = (uint16_t)((uint32_t)state[n].hue * 65535 / 360);
        esp_zb_zcl_set_attribute_val(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID,
            &enh_hue, false);

        /* Sync saturation */
        esp_zb_zcl_set_attribute_val(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID,
            &state[n].saturation, false);

        /* Sync color temperature */
        esp_zb_zcl_set_attribute_val(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID,
            &state[n].color_temp, false);
    }

    ESP_LOGI(TAG, "ZCL attribute store synced from saved state");
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

/* ================================================================== */
/*  LED Rendering                                                     */
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
            } else {
                /* Enhanced Hue mode: convert HSV to RGB */
                hsv_to_rgb(hue, sat, level, &r, &g, &b);
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

void restore_leds_cb(uint8_t param)
{
    (void)param;
    update_leds();
}

/* ================================================================== */
/*  LED Render Loop (200Hz via scheduler alarm)                       */
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

        /* Poll Enhanced Hue in color mode (0), CT in white mode (2) */
        if (zcl_mode == 0) {
            /* Read enhanced hue (16-bit, 0-65535 maps to 0-360°) */
            esp_zb_zcl_attr_t *attr_hue = esp_zb_zcl_get_attribute(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID);
            if (attr_hue && attr_hue->data_p) {
                uint16_t enh_hue = *(uint16_t *)attr_hue->data_p;
                uint16_t new_hue = (uint16_t)((uint32_t)enh_hue * 360 / 65535);
                if (new_hue != state[n].hue) {
                    state[n].hue = new_hue;
                    state[n].color_mode = 0;
                    /* Instant color change (no transition) - hue wraparound disabled */
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

void led_renderer_start(void)
{
    ESP_LOGI(TAG, "Starting LED render/poll loop at 200Hz");
    esp_zb_scheduler_alarm(led_render_cb, 0, 5);
}
