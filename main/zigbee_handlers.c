/**
 * @file zigbee_handlers.c
 * @brief Zigbee command and signal handlers implementation
 *
 * Processes Zigbee commands and updates LED strip state
 */

#include "zigbee_handlers.h"
#include "led_driver.h"
#include "board_led.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "zb_handler";

/* LED strip handle - set by light_control module */
extern led_strip_handle_t g_led_strip;

/* Current LED state */
static struct {
    bool on;
    uint8_t level;        // 0-254 (RGB brightness)
    uint16_t hue;         // 0-360 (converted from ZCL 0-254)
    uint8_t saturation;   // 0-254
    uint16_t color_temp;  // Mireds (153-370)
    uint8_t color_mode;   // Current color mode (0=HS, 1=XY, 2=CCT)
    uint8_t white_level;  // 0-254 (independent white channel for RGBW)
} s_led_state = {
    .on = false,
    .level = 128,
    .hue = 0,
    .saturation = 0,
    .color_temp = 250,
    .color_mode = 2,  // Color temperature mode
    .white_level = 0,  // White channel off by default
};

/* Network joined flag */
static bool s_network_joined = false;

/**
 * @brief Convert ZCL hue (0-254) to degrees (0-360)
 */
static uint16_t zcl_hue_to_degrees(uint8_t zcl_hue)
{
    return (uint16_t)((uint32_t)zcl_hue * 360 / 254);
}

/**
 * @brief Convert color temperature (mireds) to RGB
 * Simple approximation: warm white at high mireds, cool white at low mireds
 */
static void color_temp_to_rgb(uint16_t mireds, uint8_t *r, uint8_t *g, uint8_t *b)
{
    // Mired range: 153 (6500K) to 370 (2700K)
    // Normalize to 0-1 range
    float t = (float)(mireds - 153) / (370 - 153);
    t = (t < 0.0f) ? 0.0f : (t > 1.0f) ? 1.0f : t;

    // Cool white (6500K) -> Warm white (2700K)
    *r = (uint8_t)(255 * (0.95f + 0.05f * t));  // More red as warmer
    *g = (uint8_t)(255 * (0.85f + 0.10f * t));  // More green as warmer
    *b = (uint8_t)(255 * (1.0f - 0.45f * t));   // Less blue as warmer
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

    if (!s_led_state.on) {
        // Turn off all LEDs
        led_strip_clear(g_led_strip);
        return;
    }

    uint8_t r, g, b;

    // Determine color based on current mode
    if (s_led_state.color_mode == 2) {  // Color temperature mode
        // Color temperature mode (CCT)
        color_temp_to_rgb(s_led_state.color_temp, &r, &g, &b);
    } else {  // Hue/Saturation or XY mode
        // Hue/Saturation mode
        hsv_to_rgb(s_led_state.hue, s_led_state.saturation, s_led_state.level, &r, &g, &b);
    }

    // Apply brightness scaling
    uint32_t brightness_scale = s_led_state.level;
    r = (r * brightness_scale) / 254;
    g = (g * brightness_scale) / 254;
    b = (b * brightness_scale) / 254;

    // White channel (independent for RGBW strips)
    uint8_t w = s_led_state.white_level;

    // Set all LEDs to the same color (for now - segments will come in Phase 3)
    // TODO: Get LED count from configuration
    for (uint16_t i = 0; i < 30; i++) {  // Placeholder count
        led_strip_set_pixel_rgbw(g_led_strip, i, r, g, b, w);
    }

    led_strip_refresh(g_led_strip);

    ESP_LOGI(TAG, "LED update: on=%d, level=%d, mode=%d, RGBW=(%d,%d,%d,%d)",
             s_led_state.on, s_led_state.level, s_led_state.color_mode, r, g, b, w);
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

    // On/Off cluster
    if (cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
        if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
            s_led_state.on = *(bool *)value;
            ESP_LOGI(TAG, "On/Off -> %s", s_led_state.on ? "ON" : "OFF");
            needs_update = true;
        }
    }
    // Level Control cluster
    else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL) {
        if (attr_id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID) {
            s_led_state.level = *(uint8_t *)value;
            ESP_LOGI(TAG, "Level -> %d", s_led_state.level);
            needs_update = true;
        }
    }
    // Color Control cluster
    else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL) {
        if (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID) {
            uint8_t zcl_hue = *(uint8_t *)value;
            s_led_state.hue = zcl_hue_to_degrees(zcl_hue);
            s_led_state.color_mode = 0;  // Hue/Saturation mode
            ESP_LOGI(TAG, "Hue -> %d degrees", s_led_state.hue);
            needs_update = true;
        }
        else if (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID) {
            s_led_state.saturation = *(uint8_t *)value;
            ESP_LOGI(TAG, "Saturation -> %d", s_led_state.saturation);
            needs_update = true;
        }
        else if (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID) {
            s_led_state.color_temp = *(uint16_t *)value;
            s_led_state.color_mode = 2;  // Color temperature mode
            ESP_LOGI(TAG, "Color temp -> %d mireds", s_led_state.color_temp);
            needs_update = true;
        }
        else if (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID) {
            s_led_state.color_mode = *(uint8_t *)value;
            ESP_LOGI(TAG, "Color mode -> %d", s_led_state.color_mode);
            needs_update = true;
        }
    }

    if (needs_update) {
        update_leds();
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
                // Initialize LEDs to last known state
                update_leds();
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
            // Initialize LEDs
            update_leds();
        } else {
            ESP_LOGW(TAG, "Network steering failed (%s), retrying in 1s...",
                     esp_err_to_name(status));
            board_led_set_state(BOARD_LED_NOT_JOINED);
            esp_zb_scheduler_alarm(steering_retry_cb, ESP_ZB_BDB_NETWORK_STEERING, 1000);
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
        // Update board LED status
        board_led_update();

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
