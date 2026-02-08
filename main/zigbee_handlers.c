/**
 * @file zigbee_handlers.c
 * @brief Zigbee command and signal handlers implementation
 *
 * Processes Zigbee commands and updates LED strip state
 */

#include "zigbee_handlers.h"
#include "zigbee_init.h"
#include "led_driver.h"
#include "board_led.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_check.h"
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

/* LED strip handle - set by light_control module */
extern led_strip_handle_t g_led_strip;

/* Current LED state */
static struct {
    /* Endpoint 1: RGB */
    bool on;
    uint8_t level;        // 0-254 (RGB brightness)
    uint16_t color_x;     // CIE X * 65535 (ZCL format)
    uint16_t color_y;     // CIE Y * 65535 (ZCL format)
    uint16_t hue;         // 0-360 degrees (converted from ZCL 0-254)
    uint8_t saturation;   // 0-254
    uint8_t color_mode;   // 0=HS, 1=XY
    /* Endpoint 2: White channel */
    bool white_on;
    uint8_t white_level;  // 0-254
} s_led_state = {
    .on = false,
    .level = 128,
    .color_x = 0x616B,   // Default white CIE X (~0.3800)
    .color_y = 0x607D,   // Default white CIE Y (~0.3760)
    .hue = 0,
    .saturation = 0,
    .color_mode = 0,     // HS mode by default
    .white_on = false,
    .white_level = 128,
};

/* Network joined flag */
static bool s_network_joined = false;

/* Debounce timer for batching rapid color attribute updates (X+Y arrive separately) */
static esp_timer_handle_t s_color_update_timer = NULL;

/* Forward declaration */
static void update_leds(void);

/**
 * @brief Convert ZCL hue (0-254) to degrees (0-360)
 */
static uint16_t zcl_hue_to_degrees(uint8_t zcl_hue)
{
    return (uint16_t)((uint32_t)zcl_hue * 360 / 254);
}

/**
 * @brief Timer callback - renders LEDs after color attribute debounce window
 *
 * XY sends CurrentX and CurrentY as separate writes; debounce batches them.
 */
static void color_update_timer_cb(void *arg)
{
    update_leds();
}

/**
 * @brief Schedule a deferred LED update (30ms debounce for XY attribute pairs)
 */
static void schedule_color_update(void)
{
    if (s_color_update_timer == NULL) {
        esp_timer_create_args_t args = {
            .callback = color_update_timer_cb,
            .name = "color_upd",
        };
        esp_timer_create(&args, &s_color_update_timer);
    }
    esp_timer_stop(s_color_update_timer);
    esp_timer_start_once(s_color_update_timer, 30000);  // 30ms
}

/**
 * @brief Read a uint16 attribute from the ZCL store
 */
static bool read_attr_u16(uint8_t ep, uint16_t cluster, uint16_t attr_id, uint16_t *out)
{
    esp_zb_zcl_attr_t *attr = esp_zb_zcl_get_attribute(ep, cluster,
                                    ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr_id);
    if (attr && attr->data_p) {
        *out = *(uint16_t *)attr->data_p;
        return true;
    }
    return false;
}

/**
 * @brief Read a uint8 attribute from the ZCL store
 */
static bool read_attr_u8(uint8_t ep, uint16_t cluster, uint16_t attr_id, uint8_t *out)
{
    esp_zb_zcl_attr_t *attr = esp_zb_zcl_get_attribute(ep, cluster,
                                    ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr_id);
    if (attr && attr->data_p) {
        *out = *(uint8_t *)attr->data_p;
        return true;
    }
    return false;
}

/**
 * @brief Periodic poll of ZCL color attributes (runs in Zigbee task context)
 *
 * HS commands (moveToHue, moveToHueAndSaturation, enhanced variants) are
 * handled entirely within the Zigbee stack — no callback fires. Polling the
 * attribute store at 50ms is the only way to detect those changes.
 */
static void color_attr_poll_cb(uint8_t param)
{
    bool changed = false;

    /* Read enhanced hue (used by Z2M when enhancedHue: true) */
    uint16_t enh_hue = 0;
    if (read_attr_u16(ZB_LED_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                      ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID, &enh_hue)) {
        uint16_t hue_deg = (uint16_t)((uint32_t)enh_hue * 360 / 65535);
        if (hue_deg != s_led_state.hue) {
            s_led_state.hue = hue_deg;
            s_led_state.color_mode = 0;
            changed = true;
        }
    }

    /* Read saturation */
    uint8_t sat = 0;
    if (read_attr_u8(ZB_LED_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                     ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID, &sat)) {
        if (sat != s_led_state.saturation) {
            s_led_state.saturation = sat;
            s_led_state.color_mode = 0;
            changed = true;
        }
    }

    if (changed) {
        update_leds();
    }

    /* Re-arm for next poll */
    esp_zb_scheduler_alarm(color_attr_poll_cb, 0, 50);
}

/**
 * @brief Convert CIE XY color (ZCL format) to RGB
 *
 * Uses the Wide RGB D65 conversion matrix (Philips Hue approach).
 * ZCL stores X and Y as uint16 where value/65535 = float coordinate.
 */
static void xy_to_rgb(uint16_t zcl_x, uint16_t zcl_y, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float x = (float)zcl_x / 65535.0f;
    float y = (float)zcl_y / 65535.0f;

    if (y < 0.0001f) {
        *r = *g = *b = 0;
        return;
    }

    // xy + Y=1 -> XYZ
    float Y = 1.0f;
    float X = (Y / y) * x;
    float Z = (Y / y) * (1.0f - x - y);

    // Wide RGB D65 conversion matrix
    float rf =  X * 1.656492f - Y * 0.354851f - Z * 0.255038f;
    float gf = -X * 0.707196f + Y * 1.655397f + Z * 0.036152f;
    float bf =  X * 0.051713f - Y * 0.121364f + Z * 1.011530f;

    // Clamp negatives
    if (rf < 0.0f) rf = 0.0f;
    if (gf < 0.0f) gf = 0.0f;
    if (bf < 0.0f) bf = 0.0f;

    // Normalize if any channel exceeds 1.0
    float max_c = rf > gf ? rf : gf;
    if (bf > max_c) max_c = bf;
    if (max_c > 1.0f) {
        rf /= max_c;
        gf /= max_c;
        bf /= max_c;
    }

    // Reverse sRGB gamma
    rf = rf <= 0.0031308f ? 12.92f * rf : 1.055f * powf(rf, 1.0f / 2.4f) - 0.055f;
    gf = gf <= 0.0031308f ? 12.92f * gf : 1.055f * powf(gf, 1.0f / 2.4f) - 0.055f;
    bf = bf <= 0.0031308f ? 12.92f * bf : 1.055f * powf(bf, 1.0f / 2.4f) - 0.055f;

    *r = (uint8_t)(rf * 255.0f + 0.5f);
    *g = (uint8_t)(gf * 255.0f + 0.5f);
    *b = (uint8_t)(bf * 255.0f + 0.5f);
}

/**
 * @brief HSV to RGB conversion
 */
static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    h %= 360;

    if (s == 0) {
        // Achromatic (gray)
        *r = *g = *b = v;
        return;
    }

    uint8_t region = h / 60;
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

/**
 * @brief Update the physical LED strip based on current state
 */
static void update_leds(void)
{
    if (!g_led_strip) {
        ESP_LOGW(TAG, "LED strip not initialized");
        return;
    }

    uint8_t r = 0, g = 0, b = 0;

    if (s_led_state.on) {
        // Determine RGB from color mode
        if (s_led_state.color_mode == 1) {  // XY mode
            xy_to_rgb(s_led_state.color_x, s_led_state.color_y, &r, &g, &b);
        } else {  // HS mode (0) or fallback
            hsv_to_rgb(s_led_state.hue, s_led_state.saturation, 255, &r, &g, &b);
        }

        // Apply RGB brightness
        r = (uint8_t)((uint32_t)r * s_led_state.level / 254);
        g = (uint8_t)((uint32_t)g * s_led_state.level / 254);
        b = (uint8_t)((uint32_t)b * s_led_state.level / 254);
    }

    // White channel from endpoint 2 (independent)
    uint8_t w = s_led_state.white_on ? s_led_state.white_level : 0;

    for (uint16_t i = 0; i < 30; i++) {
        led_strip_set_pixel_rgbw(g_led_strip, i, r, g, b, w);
    }

    led_strip_refresh(g_led_strip);

    ESP_LOGI(TAG, "LED update: RGB on=%d level=%d mode=%d (%d,%d,%d) W on=%d level=%d",
             s_led_state.on, s_led_state.level, s_led_state.color_mode, r, g, b,
             s_led_state.white_on, w);
}

/**
 * @brief Handle attribute value changes from Zigbee
 */
static esp_err_t handle_set_attr_value(const esp_zb_zcl_set_attr_value_message_t *message)
{
    if (!message || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        return ESP_OK;
    }

    uint8_t endpoint = message->info.dst_endpoint;
    uint16_t cluster = message->info.cluster;
    uint16_t attr_id = message->attribute.id;
    void *value = message->attribute.data.value;

    ESP_LOGI(TAG, "Attr change: EP=%d, cluster=0x%04X, attr=0x%04X",
             endpoint, cluster, attr_id);

    bool needs_update = false;

    if (endpoint == ZB_WHITE_ENDPOINT) {
        /* Endpoint 2: White channel */
        if (cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
                s_led_state.white_on = *(bool *)value;
                ESP_LOGI(TAG, "White On/Off -> %s", s_led_state.white_on ? "ON" : "OFF");
                needs_update = true;
            }
        } else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL) {
            if (attr_id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID) {
                s_led_state.white_level = *(uint8_t *)value;
                ESP_LOGI(TAG, "White level -> %d", s_led_state.white_level);
                needs_update = true;
            }
        }
        if (needs_update) {
            update_leds();
        }
    } else {
        /* Endpoint 1: RGB */
        if (cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
                s_led_state.on = *(bool *)value;
                ESP_LOGI(TAG, "RGB On/Off -> %s", s_led_state.on ? "ON" : "OFF");
                update_leds();
            }
        } else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL) {
            if (attr_id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID) {
                s_led_state.level = *(uint8_t *)value;
                ESP_LOGI(TAG, "RGB level -> %d", s_led_state.level);
                update_leds();
            }
        } else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL) {
            if (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID) {
                s_led_state.hue = zcl_hue_to_degrees(*(uint8_t *)value);
                s_led_state.color_mode = 0;
                ESP_LOGI(TAG, "Hue -> %d deg", s_led_state.hue);
                schedule_color_update();
            } else if (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID) {
                uint16_t enh_hue = *(uint16_t *)value;
                s_led_state.hue = (uint16_t)((uint32_t)enh_hue * 360 / 65535);
                s_led_state.color_mode = 0;
                ESP_LOGI(TAG, "Enhanced hue -> %d (= %d deg)", enh_hue, s_led_state.hue);
                schedule_color_update();
            } else if (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID) {
                s_led_state.saturation = *(uint8_t *)value;
                ESP_LOGI(TAG, "Saturation -> %d", s_led_state.saturation);
                schedule_color_update();
            } else if (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID) {
                s_led_state.color_x = *(uint16_t *)value;
                s_led_state.color_mode = 1;
                ESP_LOGI(TAG, "Color X -> %d", s_led_state.color_x);
                schedule_color_update();
            } else if (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID) {
                s_led_state.color_y = *(uint16_t *)value;
                s_led_state.color_mode = 1;
                ESP_LOGI(TAG, "Color Y -> %d", s_led_state.color_y);
                schedule_color_update();
            } else if (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID) {
                s_led_state.color_mode = *(uint8_t *)value;
                ESP_LOGI(TAG, "Color mode -> %d", s_led_state.color_mode);
                schedule_color_update();
            }
            /* CCT ignored: SK6812 W channel is fixed temperature */
        }
    }

    return ESP_OK;
}

/**
 * @brief Zigbee core action handler
 */
esp_err_t zigbee_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    esp_err_t ret = ESP_OK;

    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        ret = handle_set_attr_value((const esp_zb_zcl_set_attr_value_message_t *)message);
        break;

    case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID:
        ESP_LOGD(TAG, "Read attribute response");
        break;

    case ESP_ZB_CORE_CMD_WRITE_ATTR_RESP_CB_ID:
        ESP_LOGD(TAG, "Write attribute response");
        break;

    case ESP_ZB_CORE_CMD_REPORT_CONFIG_RESP_CB_ID:
        ESP_LOGD(TAG, "Report config response");
        break;

    case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID:
        ESP_LOGD(TAG, "Default response");
        break;

    default:
        ESP_LOGD(TAG, "Unhandled callback: 0x%x", callback_id);
        break;
    }

    return ret;
}

/**
 * @brief Retry network steering after failure
 */
static void steering_retry_cb(uint8_t param)
{
    ESP_LOGI(TAG, "Retrying network steering...");
    board_led_set_state(BOARD_LED_PAIRING);
    esp_zb_bdb_start_top_level_commissioning(param);
}

/**
 * @brief Zigbee signal handler
 */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct ? signal_struct->p_app_signal : NULL;
    esp_zb_app_signal_type_t sig = p_sg_p ? *p_sg_p : 0;
    esp_err_t status = signal_struct ? signal_struct->esp_err_status : ESP_OK;

    switch (sig) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Stack initialized, starting network steering");
        board_led_set_state(BOARD_LED_PAIRING);
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_NETWORK_STEERING);
        /* Start polling color attributes for HS command detection */
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
        } else {
            ESP_LOGW(TAG, "Network steering failed (%s), retrying in 5s...",
                     esp_err_to_name(status));
            board_led_set_state(BOARD_LED_ERROR);
            esp_zb_scheduler_alarm(steering_retry_cb, ESP_ZB_BDB_NETWORK_STEERING, 5000);
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE:
        ESP_LOGW(TAG, "Left Zigbee network");
        board_led_set_state(BOARD_LED_NOT_JOINED);
        s_network_joined = false;
        // Turn off LEDs when disconnected
        led_strip_clear(g_led_strip);
        // Retry joining
        esp_zb_scheduler_alarm(steering_retry_cb, ESP_ZB_BDB_NETWORK_STEERING, 1000);
        break;

    case ESP_ZB_COMMON_SIGNAL_CAN_SLEEP:
        // Router doesn't sleep
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
    /* esp_zb_factory_reset() restarts, but just in case: */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

void zigbee_full_factory_reset(void)
{
    ESP_LOGW(TAG, "FULL factory reset - erasing Zigbee network + NVS config");
    board_led_set_state(BOARD_LED_ERROR);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Erase application NVS namespace */
    nvs_handle_t h;
    if (nvs_open("led_cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "NVS config erased");
    }

    /* Then erase Zigbee network data and restart */
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

    ESP_LOGI(TAG, "Button task started, monitoring GPIO %d", BOARD_BUTTON_GPIO);

    uint32_t held_ms = 0;
    uint32_t blink_counter = 0;

    while (1) {
        if (gpio_get_level(BOARD_BUTTON_GPIO) == 0) {
            /* Button pressed */
            held_ms += 100;
            blink_counter++;

            if (held_ms >= 1000 && held_ms < BOARD_BUTTON_HOLD_ZIGBEE_MS) {
                /* 1-3s: Fast blink - building to Zigbee reset */
                if (blink_counter % 2 == 0) {
                    board_led_set_state(BOARD_LED_ERROR);
                } else {
                    board_led_set_state(BOARD_LED_NOT_JOINED);
                }
            } else if (held_ms >= BOARD_BUTTON_HOLD_ZIGBEE_MS && held_ms < BOARD_BUTTON_HOLD_FULL_MS) {
                /* 3-10s: Slow blink - Zigbee reset armed, holding for full */
                if ((blink_counter / 5) % 2 == 0) {
                    board_led_set_state(BOARD_LED_ERROR);
                } else {
                    board_led_set_state(BOARD_LED_NOT_JOINED);
                }
            } else if (held_ms >= BOARD_BUTTON_HOLD_FULL_MS) {
                /* >10s: Solid red - full reset armed */
                board_led_set_state(BOARD_LED_ERROR);
            }
        } else {
            /* Button released */
            if (held_ms >= BOARD_BUTTON_HOLD_FULL_MS) {
                /* >10s hold: Full factory reset (Zigbee + NVS) */
                zigbee_full_factory_reset();
            } else if (held_ms >= BOARD_BUTTON_HOLD_ZIGBEE_MS) {
                /* 3-10s hold: Zigbee network reset only */
                zigbee_factory_reset();
            } else if (held_ms >= 1000) {
                /* 1-3s hold: Cancelled - restore LED to current network state */
                board_led_set_state(s_network_joined
                    ? BOARD_LED_JOINED : BOARD_LED_NOT_JOINED);
            }
            held_ms = 0;
            blink_counter = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/**
 * @brief Start button monitoring task
 */
void button_task_start(void)
{
    xTaskCreate(button_task, "btn_task", 2048, NULL, 5, NULL);
}
