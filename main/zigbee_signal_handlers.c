/**
 * @file zigbee_signal_handlers.c
 * @brief Zigbee signal handler and factory reset implementation
 *
 * Handles Zigbee network signals (join, leave, steering) and factory reset operations.
 */

#include "zigbee_signal_handlers.h"
#include "zigbee_init.h"
#include "led_renderer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "zb_handler";

/* C wrappers for BoardLed (defined in main.cpp) */
extern void board_led_set_state_off(void);
extern void board_led_set_state_not_joined(void);
extern void board_led_set_state_pairing(void);
extern void board_led_set_state_joined(void);
extern void board_led_set_state_error(void);

/* LED driver functions */
extern void led_driver_clear(uint8_t strip);
extern void led_driver_refresh(void);

bool s_network_joined = false;

/* ================================================================== */
/*  Signal handler callbacks                                           */
/* ================================================================== */

static void steering_retry_cb(uint8_t param)
{
    ESP_LOGI(TAG, "Retrying network steering...");
    board_led_set_state_pairing();
    esp_zb_bdb_start_top_level_commissioning(param);
}

void reboot_cb(uint8_t param)
{
    (void)param;
    esp_restart();
}

/* ================================================================== */
/*  Signal handler                                                     */
/* ================================================================== */

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
        led_renderer_start();
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
