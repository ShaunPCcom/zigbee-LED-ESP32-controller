/**
 * @file main.c
 * @brief Main entry point for Zigbee LED Controller
 *
 * Phase 4: Dual physical strip support via SPI time-multiplexing.
 * Controls LED strips via Zigbee commands from Home Assistant.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "led_driver.h"
#include "zigbee_init.h"
#include "zigbee_handlers.h"
#include "board_led.h"
#include "board_config.h"
#include "config_storage.h"
#include "led_cli.h"
#include "segment_manager.h"

static const char *TAG = "main";

/* Per-strip LED counts — loaded from NVS, used by LED driver and Zigbee init */
uint16_t g_strip_count[2] = {LED_STRIP_1_COUNT, LED_STRIP_2_COUNT};

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Zigbee LED Controller");
    ESP_LOGI(TAG, "========================================");

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    ESP_ERROR_CHECK(config_storage_init());

    /* Load per-strip counts from NVS */
    uint16_t tmp;
    for (int i = 0; i < 2; i++) {
        if (config_storage_load_strip_count(i, &tmp) == ESP_OK) {
            g_strip_count[i] = tmp;
            ESP_LOGI(TAG, "Strip %d count from NVS: %u", i, g_strip_count[i]);
        }
    }

    /* Initialize segment manager (segment 1 defaults to full strip 0 length) */
    segment_manager_init(g_strip_count[0]);
    segment_manager_load();

    /* Initialize board LED status (uses strip 0, pixels 0-2) */
    board_led_init();
    board_led_set_state(BOARD_LED_NOT_JOINED);

    /* Initialize LED driver (SPI, both strips) */
    ret = led_driver_init(g_strip_count[0], g_strip_count[1]);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init LED driver: %s", esp_err_to_name(ret));
        return;
    }
    led_driver_clear(0);
    led_driver_clear(1);
    led_driver_refresh();
    ESP_LOGI(TAG, "LED driver initialized (strip0=%u@GPIO%d strip1=%u@GPIO%d)",
             g_strip_count[0], LED_STRIP_1_GPIO, g_strip_count[1], LED_STRIP_2_GPIO);

    /* Initialize and start Zigbee */
    ret = zigbee_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Zigbee: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Zigbee stack initialized as Router");

    led_cli_start();
    ESP_LOGI(TAG, "CLI started");

    button_task_start();
    ESP_LOGI(TAG, "Button task started (GPIO %d)", BOARD_BUTTON_GPIO);

    ESP_LOGI(TAG, "Device ready! Waiting for Zigbee network pairing...");
    ESP_LOGI(TAG, "Button: 3s=Zigbee reset, 10s=Full reset");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "Uptime: %lld s", esp_timer_get_time() / 1000000);
    }
}
