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
#include "zigbee_init.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_zigbee_core.h"

static const char *TAG = "led_renderer";

/* Per-strip power scale: 0-255, applied as brightness multiplier */
static uint8_t s_power_scale[LED_DRIVER_MAX_STRIPS] = {255, 255};

/* Last raw ZCL values seen by the render loop per segment + EP9 "all" master.
 * Updated ONLY by the render loop — never by callbacks.
 * This makes change detection immune to the SDK firing SET_ATTR_VALUE_CB_ID
 * for one group endpoint but not the other, which would pre-set state[n].hue
 * and cause the render loop to silently skip the update for that segment.
 * Index [MAX_SEGMENTS] tracks EP9 ("all segments" master). */
static uint16_t s_last_enh_hue[MAX_SEGMENTS + 1] = {0};
static uint8_t  s_last_sat[MAX_SEGMENTS + 1]     = {0};
static uint16_t s_last_ct[MAX_SEGMENTS + 1]      = {0};

/* Globals set by main.cpp after NVS load */
extern uint16_t g_strip_max_current[2];

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
/*  Power Limiting                                                    */
/* ================================================================== */

void led_renderer_recalc_power_scale(void)
{
    for (int i = 0; i < LED_DRIVER_MAX_STRIPS; i++) {
        uint16_t count      = led_driver_get_count(i);
        led_strip_type_t t  = led_driver_get_type(i);
        uint16_t max_cur    = g_strip_max_current[i];
        uint16_t per_led_ma = (t == LED_STRIP_TYPE_WS2812B) ? 60 : 80;

        if (max_cur == 0 || count == 0) {
            s_power_scale[i] = 255;
        } else {
            uint32_t total_ma = (uint32_t)count * per_led_ma;
            uint32_t scale = ((uint32_t)max_cur * 255) / total_ma;
            s_power_scale[i] = (scale >= 255) ? 255 : (uint8_t)scale;
        }
        ESP_LOGI(TAG, "Strip%d power scale: %u/255 (max=%umA, count=%u, %umA/LED)",
                 i, s_power_scale[i], max_cur, count, per_led_ma);
    }
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

        uint8_t strip = geom[n].strip_id;

        if (state[n].on) {
            /* Read interpolated values from transition engine */
            uint8_t  level = (uint8_t)transition_get_value(&state[n].level_trans);
            uint16_t hue   = transition_get_value(&state[n].hue_trans);
            uint8_t  sat   = (uint8_t)transition_get_value(&state[n].sat_trans);
            uint16_t ct    = transition_get_value(&state[n].ct_trans);

            /* Apply power scale (worst-case brightness limiting) */
            uint8_t sc = s_power_scale[strip];
            if (sc < 255) {
                level = (uint8_t)(((uint16_t)level * sc) / 255);
            }

            if (state[n].color_mode == 2) {
                if (led_driver_get_type(strip) == LED_STRIP_TYPE_WS2812B) {
                    /* WS2812B: approximate warm white via desaturated orange.
                     * CT range: 153 mir (6500K, cool) to 370 mir (2700K, warm).
                     * Cool end -> sat=0 (pure white). Warm end -> sat~140 (~55%, amber tint).
                     * Hue fixed at 30° (orange/amber). Smooth, perceptually convincing. */
                    uint16_t ct_cool = 153, ct_warm = 370;
                    uint16_t ct_clamped = (ct < ct_cool) ? ct_cool : (ct > ct_warm) ? ct_warm : ct;
                    uint8_t t   = (uint8_t)(((uint32_t)(ct_clamped - ct_cool) * 255) / (ct_warm - ct_cool));
                    uint8_t ww_sat = (uint8_t)(((uint32_t)t * 140) / 255);
                    hsv_to_rgb(30, ww_sat, level, &r, &g, &b);
                } else {
                    /* SK6812: drive White channel with brightness */
                    w = level;
                }
            } else {
                /* Enhanced Hue mode: convert HSV to RGB */
                hsv_to_rgb(hue, sat, level, &r, &g, &b);
            }
        }

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

        /* Always sync color_mode from ZCL — prevents mode getting stuck when
         * a group command changes the mode without changing the value */
        state[n].color_mode = zcl_mode;

        /* Poll Enhanced Hue in color mode (0), CT in white mode (2) */
        if (zcl_mode == 0) {
            /* Read enhanced hue (16-bit, 0-65535 maps to 0-360°) */
            esp_zb_zcl_attr_t *attr_hue = esp_zb_zcl_get_attribute(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID);
            if (attr_hue && attr_hue->data_p) {
                uint16_t enh_hue = *(uint16_t *)attr_hue->data_p;
                if (enh_hue != s_last_enh_hue[n]) {
                    s_last_enh_hue[n] = enh_hue;
                    state[n].hue = (uint16_t)((uint32_t)enh_hue * 360 / 65535);
                    transition_start(&state[n].hue_trans, state[n].hue, 0);
                }
            }

            /* Read saturation */
            esp_zb_zcl_attr_t *attr_sat = esp_zb_zcl_get_attribute(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID);
            if (attr_sat && attr_sat->data_p) {
                uint8_t new_sat = *(uint8_t *)attr_sat->data_p;
                if (new_sat != s_last_sat[n]) {
                    s_last_sat[n] = new_sat;
                    state[n].saturation = new_sat;
                    transition_start(&state[n].sat_trans, new_sat, 0);
                }
            }
        } else if (zcl_mode == 2) {
            /* CT (white) mode - poll color temperature */
            esp_zb_zcl_attr_t *attr_ct = esp_zb_zcl_get_attribute(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID);
            if (attr_ct && attr_ct->data_p) {
                uint16_t new_ct = *(uint16_t *)attr_ct->data_p;
                if (new_ct != s_last_ct[n]) {
                    s_last_ct[n] = new_ct;
                    state[n].color_temp = new_ct;
                    transition_start(&state[n].ct_trans, new_ct, g_global_transition_ms);
                }
            }
        }
    }

    /* EP9 "all segments" master — poll HS/CT and propagate to all active segments.
     * On/off and level are handled via SET_ATTR_VALUE_CB_ID in zigbee_attr_handler.c. */
    {
        uint8_t ep_all = ZB_ALL_EP;

        esp_zb_zcl_attr_t *attr_mode_all = esp_zb_zcl_get_attribute(ep_all,
            ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID);
        uint8_t zcl_mode_all = (attr_mode_all && attr_mode_all->data_p)
                               ? *(uint8_t *)attr_mode_all->data_p : 0;

        if (zcl_mode_all == 0) {
            esp_zb_zcl_attr_t *attr_hue = esp_zb_zcl_get_attribute(ep_all,
                ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID);
            if (attr_hue && attr_hue->data_p) {
                uint16_t enh_hue = *(uint16_t *)attr_hue->data_p;
                if (enh_hue != s_last_enh_hue[MAX_SEGMENTS]) {
                    s_last_enh_hue[MAX_SEGMENTS] = enh_hue;
                    uint16_t hue = (uint16_t)((uint32_t)enh_hue * 360 / 65535);
                    uint8_t mode0 = 0;
                    for (int i = 0; i < MAX_SEGMENTS; i++) {
                        state[i].hue = hue;
                        state[i].color_mode = 0;
                        transition_start(&state[i].hue_trans, hue, 0);
                        /* Sync segment EP ZCL color_mode so render loop doesn't
                         * revert back to CT on next tick. */
                        uint8_t ep_i = (uint8_t)(ZB_SEGMENT_EP_BASE + i);
                        esp_zb_zcl_set_attribute_val(ep_i, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                            ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID, &mode0, false);
                    }
                }
            }

            esp_zb_zcl_attr_t *attr_sat = esp_zb_zcl_get_attribute(ep_all,
                ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID);
            if (attr_sat && attr_sat->data_p) {
                uint8_t new_sat = *(uint8_t *)attr_sat->data_p;
                if (new_sat != s_last_sat[MAX_SEGMENTS]) {
                    s_last_sat[MAX_SEGMENTS] = new_sat;
                    for (int i = 0; i < MAX_SEGMENTS; i++) {
                        state[i].saturation = new_sat;
                        transition_start(&state[i].sat_trans, new_sat, 0);
                    }
                }
            }
        } else if (zcl_mode_all == 2) {
            esp_zb_zcl_attr_t *attr_ct = esp_zb_zcl_get_attribute(ep_all,
                ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID);
            if (attr_ct && attr_ct->data_p) {
                uint16_t new_ct = *(uint16_t *)attr_ct->data_p;
                if (new_ct != s_last_ct[MAX_SEGMENTS]) {
                    s_last_ct[MAX_SEGMENTS] = new_ct;
                    uint8_t mode2 = 2;
                    for (int i = 0; i < MAX_SEGMENTS; i++) {
                        state[i].color_temp = new_ct;
                        state[i].color_mode = 2;
                        transition_start(&state[i].ct_trans, new_ct, g_global_transition_ms);
                        /* Sync segment EP ZCL stores so the per-segment render loop
                         * polling doesn't revert color_mode back to HS next tick. */
                        uint8_t ep_i = (uint8_t)(ZB_SEGMENT_EP_BASE + i);
                        esp_zb_zcl_set_attribute_val(ep_i, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                            ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID, &mode2, false);
                        esp_zb_zcl_set_attribute_val(ep_i, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                            ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID, &new_ct, false);
                    }
                }
            }
        }
    }

    /* Update min_free_heap ZCL attr every ~60s (12000 * 5ms = 60s) */
    static uint16_t s_heap_tick = 0;
    if (++s_heap_tick >= 12000) {
        s_heap_tick = 0;
        uint32_t heap = esp_get_minimum_free_heap_size();
        esp_zb_zcl_set_attribute_val(ZB_SEGMENT_EP_BASE,
            ZB_CLUSTER_DEVICE_CONFIG, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            ZB_ATTR_MIN_FREE_HEAP, &heap, false);
    }

    update_leds();
    esp_zb_scheduler_alarm(led_render_cb, 0, 5);
}

void led_renderer_start(void)
{
    ESP_LOGI(TAG, "Starting LED render/poll loop at 200Hz");
    esp_zb_scheduler_alarm(led_render_cb, 0, 5);
}
