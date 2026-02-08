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
#include "board_led.h"
#include "board_config.h"
#include "config_storage.h"
#include "segment_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <math.h>

static const char *TAG = "zb_handler";

extern led_strip_handle_t g_led_strip;
extern uint16_t g_led_count;

static bool s_network_joined = false;

/* Debounce timer: batch rapid XY/HS attribute updates */
static esp_timer_handle_t s_color_update_timer = NULL;

/* Debounce timer: avoid NVS write flood */
static esp_timer_handle_t s_save_timer = NULL;

/* Forward declarations */
static void update_leds(void);
static void schedule_save(void);
static void restore_leds_cb(uint8_t param);
static void reboot_cb(uint8_t param);

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

static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    h %= 360;
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

static void color_update_timer_cb(void *arg)
{
    update_leds();
    schedule_save();
}

static void schedule_color_update(void)
{
    if (s_color_update_timer == NULL) {
        esp_timer_create_args_t args = { .callback = color_update_timer_cb, .name = "color_upd" };
        esp_timer_create(&args, &s_color_update_timer);
    }
    esp_timer_stop(s_color_update_timer);
    esp_timer_start_once(s_color_update_timer, 30000);  /* 30ms */
}

static void save_timer_cb(void *arg)
{
    segment_manager_save();
}

static void schedule_save(void)
{
    if (s_save_timer == NULL) {
        esp_timer_create_args_t args = { .callback = save_timer_cb, .name = "cfg_save" };
        esp_timer_create(&args, &s_save_timer);
    }
    esp_timer_stop(s_save_timer);
    esp_timer_start_once(s_save_timer, 500 * 1000);  /* 500ms */
}

/* ================================================================== */
/*  ZCL attribute helpers                                              */
/* ================================================================== */

static bool read_attr_u16(uint8_t ep, uint16_t cluster, uint16_t attr_id, uint16_t *out)
{
    esp_zb_zcl_attr_t *attr = esp_zb_zcl_get_attribute(ep, cluster,
                                  ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr_id);
    if (attr && attr->data_p) { *out = *(uint16_t *)attr->data_p; return true; }
    return false;
}

static bool read_attr_u8(uint8_t ep, uint16_t cluster, uint16_t attr_id, uint8_t *out)
{
    esp_zb_zcl_attr_t *attr = esp_zb_zcl_get_attribute(ep, cluster,
                                  ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr_id);
    if (attr && attr->data_p) { *out = *(uint8_t *)attr->data_p; return true; }
    return false;
}

/* ================================================================== */
/*  HS color polling (50ms)                                            */
/*  MoveToHue/MoveToSat commands are handled inside the Zigbee stack  */
/*  without firing a callback — polling is the only detection method.  */
/* ================================================================== */

static void color_attr_poll_cb(uint8_t param)
{
    bool changed = false;
    segment_light_t *state = segment_state_get();

    for (int n = 0; n < MAX_SEGMENTS; n++) {
        uint8_t ep = (uint8_t)(ZB_SEGMENT_EP_BASE + n);

        uint16_t enh_hue = 0;
        if (read_attr_u16(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                          ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID, &enh_hue)) {
            uint16_t hd = (uint16_t)((uint32_t)enh_hue * 360 / 65535);
            if (hd != state[n].hue) { state[n].hue = hd; state[n].color_mode = 0; changed = true; }
        }

        uint8_t sat = 0;
        if (read_attr_u8(ep, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                         ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID, &sat)) {
            if (sat != state[n].saturation) { state[n].saturation = sat; state[n].color_mode = 0; changed = true; }
        }
    }

    if (changed) { update_leds(); schedule_save(); }

    esp_zb_scheduler_alarm(color_attr_poll_cb, 0, 50);
}

/* ================================================================== */
/*  LED rendering                                                      */
/*                                                                     */
/*  All segments rendered in order; last segment wins overlaps.       */
/*  Segment 1 (index 0) covers the full strip as the base layer.     */
/* ================================================================== */

static void update_leds(void)
{
    if (!g_led_strip) {
        ESP_LOGW(TAG, "LED strip not initialized");
        return;
    }

    segment_geom_t  *geom  = segment_geom_get();
    segment_light_t *state = segment_state_get();

    /* Start with all LEDs off */
    for (uint16_t i = 0; i < g_led_count; i++) {
        led_strip_set_pixel_rgbw(g_led_strip, i, 0, 0, 0, 0);
    }

    /* Render segments in order (1 first = base layer, 8 last = top overlay) */
    for (int n = 0; n < MAX_SEGMENTS; n++) {
        if (geom[n].count == 0) continue;

        uint8_t r = 0, g = 0, b = 0, w = 0;

        if (state[n].on) {
            if (state[n].color_mode == 2) {
                /* CT mode: drive White channel with brightness */
                w = state[n].level;
            } else if (state[n].color_mode == 1) {
                /* XY mode */
                xy_to_rgb(state[n].color_x, state[n].color_y, &r, &g, &b);
                r = (uint8_t)((uint32_t)r * state[n].level / 254);
                g = (uint8_t)((uint32_t)g * state[n].level / 254);
                b = (uint8_t)((uint32_t)b * state[n].level / 254);
            } else {
                /* HS mode */
                hsv_to_rgb(state[n].hue, state[n].saturation, 255, &r, &g, &b);
                r = (uint8_t)((uint32_t)r * state[n].level / 254);
                g = (uint8_t)((uint32_t)g * state[n].level / 254);
                b = (uint8_t)((uint32_t)b * state[n].level / 254);
            }
        }

        uint16_t end = geom[n].start + geom[n].count;
        if (end > g_led_count) end = g_led_count;
        for (uint16_t i = geom[n].start; i < end; i++) {
            led_strip_set_pixel_rgbw(g_led_strip, i, r, g, b, w);
        }
    }

    led_strip_refresh(g_led_strip);
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
    if (cluster == ZB_CLUSTER_DEVICE_CONFIG && attr_id == ZB_ATTR_LED_COUNT) {
        uint16_t new_count = *(uint16_t *)value;
        if (new_count >= 1 && new_count <= 500) {
            ESP_LOGI(TAG, "LED count -> %u (saving, reboot in 1s)", new_count);
            config_storage_save_led_count(new_count);
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
            } else {
                geom[seg_idx].count = *(uint16_t *)value;
                ESP_LOGI(TAG, "Seg%d count -> %u", seg_idx + 1, geom[seg_idx].count);
            }
            segment_manager_save();
            update_leds();
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
                needs_update = true;
            }
        } else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL) {
            if (attr_id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID) {
                state[seg].level = *(uint8_t *)value;
                ESP_LOGI(TAG, "Seg%d level -> %d", seg + 1, state[seg].level);
                needs_update = true;
            }
        } else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL) {
            switch (attr_id) {
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID:
                state[seg].hue = zcl_hue_to_degrees(*(uint8_t *)value);
                state[seg].color_mode = 0;
                schedule_color_update();
                break;
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID: {
                uint16_t eh = *(uint16_t *)value;
                state[seg].hue = (uint16_t)((uint32_t)eh * 360 / 65535);
                state[seg].color_mode = 0;
                schedule_color_update();
                break;
            }
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID:
                state[seg].saturation = *(uint8_t *)value;
                schedule_color_update();
                break;
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID:
                state[seg].color_x = *(uint16_t *)value;
                state[seg].color_mode = 1;
                schedule_color_update();
                break;
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID:
                state[seg].color_y = *(uint16_t *)value;
                state[seg].color_mode = 1;
                schedule_color_update();
                break;
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID:
                state[seg].color_temp = *(uint16_t *)value;
                state[seg].color_mode = 2;
                ESP_LOGI(TAG, "Seg%d CT -> %u mireds", seg + 1, state[seg].color_temp);
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
    board_led_set_state(BOARD_LED_PAIRING);
    esp_zb_bdb_start_top_level_commissioning(param);
}

static void reboot_cb(uint8_t param)
{
    (void)param;
    esp_restart();
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
        segment_manager_load();
        board_led_set_state(BOARD_LED_PAIRING);
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_NETWORK_STEERING);
        esp_zb_scheduler_alarm(color_attr_poll_cb, 0, 50);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (status == ESP_OK) {
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Factory new device, starting network steering");
                board_led_set_state(BOARD_LED_PAIRING);
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device rebooted, already joined network");
                board_led_set_state(BOARD_LED_JOINED);
                s_network_joined = true;
                esp_zb_scheduler_alarm(restore_leds_cb, 0, 5500);
            }
        } else {
            ESP_LOGE(TAG, "Device start/reboot failed: %s", esp_err_to_name(status));
            board_led_set_state(BOARD_LED_ERROR);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (status == ESP_OK) {
            ESP_LOGI(TAG, "Successfully joined Zigbee network!");
            board_led_set_state(BOARD_LED_JOINED);
            s_network_joined = true;
            esp_zb_scheduler_alarm(restore_leds_cb, 0, 5500);
        } else {
            ESP_LOGW(TAG, "Network steering failed (%s), retrying in 5s...", esp_err_to_name(status));
            board_led_set_state(BOARD_LED_ERROR);
            esp_zb_scheduler_alarm(steering_retry_cb, ESP_ZB_BDB_NETWORK_STEERING, 5000);
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE:
        ESP_LOGW(TAG, "Left Zigbee network");
        board_led_set_state(BOARD_LED_NOT_JOINED);
        s_network_joined = false;
        led_strip_clear(g_led_strip);
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
    board_led_set_state(BOARD_LED_ERROR);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_zb_factory_reset();
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

void zigbee_full_factory_reset(void)
{
    ESP_LOGW(TAG, "FULL factory reset - erasing Zigbee network + NVS config");
    board_led_set_state(BOARD_LED_ERROR);
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

static void button_task(void *pv)
{
    (void)pv;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOARD_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "Button task started (GPIO %d)", BOARD_BUTTON_GPIO);

    uint32_t held_ms = 0;
    uint32_t blink_counter = 0;

    while (1) {
        if (gpio_get_level(BOARD_BUTTON_GPIO) == 0) {
            held_ms += 100;
            blink_counter++;
            if (held_ms >= 1000 && held_ms < BOARD_BUTTON_HOLD_ZIGBEE_MS) {
                board_led_set_state((blink_counter % 2) ? BOARD_LED_NOT_JOINED : BOARD_LED_ERROR);
            } else if (held_ms >= BOARD_BUTTON_HOLD_ZIGBEE_MS && held_ms < BOARD_BUTTON_HOLD_FULL_MS) {
                board_led_set_state(((blink_counter / 5) % 2) ? BOARD_LED_NOT_JOINED : BOARD_LED_ERROR);
            } else if (held_ms >= BOARD_BUTTON_HOLD_FULL_MS) {
                board_led_set_state(BOARD_LED_ERROR);
            }
        } else {
            if (held_ms >= BOARD_BUTTON_HOLD_FULL_MS) {
                zigbee_full_factory_reset();
            } else if (held_ms >= BOARD_BUTTON_HOLD_ZIGBEE_MS) {
                zigbee_factory_reset();
            } else if (held_ms >= 1000) {
                board_led_set_state(s_network_joined ? BOARD_LED_JOINED : BOARD_LED_NOT_JOINED);
            }
            held_ms = 0;
            blink_counter = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void button_task_start(void)
{
    xTaskCreate(button_task, "btn_task", 2048, NULL, 5, NULL);
}
