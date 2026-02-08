/**
 * @file main.c
 * @brief Main entry point for Zigbee LED Controller
 *
 * Phase 2: Zigbee integration
 * Controls LED strip via Zigbee commands from Home Assistant
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

// Global LED strip handle (accessed by zigbee_handlers.c)
led_strip_handle_t g_led_strip = NULL;

// LED count - loaded from NVS at boot, used by LED driver and Zigbee init
uint16_t g_led_count = LED_STRIP_COUNT;

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Zigbee LED Controller");
    ESP_LOGI(TAG, "========================================");

    // Initialize NVS (required for Zigbee and config storage)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "✓ NVS initialized");

    // Initialize config storage
    ESP_ERROR_CHECK(config_storage_init());

    // Load LED count from NVS (may override compile-time default)
    uint16_t stored_count;
    if (config_storage_load_led_count(&stored_count) == ESP_OK) {
        g_led_count = stored_count;
        ESP_LOGI(TAG, "LED count loaded from NVS: %u", g_led_count);
    }

    // Initialize segment manager — segment 1 defaults to full strip length
    // (must be after g_led_count is resolved from NVS)
    segment_manager_init(g_led_count);

    // Initialize board LED (onboard WS2812 for status)
    board_led_init();
    board_led_set_state(BOARD_LED_NOT_JOINED);

    // Configure LED strip (SK6812 RGBW mode)
    led_strip_config_t strip_config = {
        .gpio_num = LED_STRIP_1_GPIO,
        .led_count = g_led_count,
        .type = LED_STRIP_TYPE,
        .rmt_resolution_hz = 0,       // Use default 10MHz
    };

    ret = led_strip_create(&strip_config, &g_led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "✗ Failed to create LED strip: %s", esp_err_to_name(ret));
        return;
    }

    // Clear strip initially
    led_strip_clear(g_led_strip);
    ESP_LOGI(TAG, "✓ LED strip initialized (GPIO %d, %u LEDs)", LED_STRIP_1_GPIO, g_led_count);

    // Initialize and start Zigbee
    ret = zigbee_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "✗ Failed to initialize Zigbee: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "✓ Zigbee stack initialized as Router");

    // Start serial CLI
    led_cli_start();
    ESP_LOGI(TAG, "✓ CLI started");

    // Start button monitoring task
    button_task_start();
    ESP_LOGI(TAG, "✓ Button task started (GPIO %d)", BOARD_BUTTON_GPIO);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Device ready! Waiting for Zigbee network pairing...");
    ESP_LOGI(TAG, "Button: 3s=Zigbee reset, 10s=Full reset");
    ESP_LOGI(TAG, "");

    // Main loop - Zigbee runs in its own task
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "System running... (uptime: %lld seconds)", esp_timer_get_time() / 1000000);
    }
}

