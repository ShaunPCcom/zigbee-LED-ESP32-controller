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

static const char *TAG = "main";

// GPIO pin assignments for ESP32-H2-DevKitM-1
#define LED_STRIP_1_GPIO    4
#define LED_STRIP_2_GPIO    5
#define LED_STRIP_3_GPIO    10
#define ONBOARD_LED_GPIO    8

// LED configuration
#define LED_COUNT           30  // Adjust based on your strip

// Global LED strip handle (accessed by zigbee_handlers.c)
led_strip_handle_t g_led_strip = NULL;

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Zigbee LED Controller");
    ESP_LOGI(TAG, "  Phase 2: Zigbee Integration");
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

    // Initialize board LED (onboard WS2812 for status)
    board_led_init();
    board_led_set_state(BOARD_LED_NOT_JOINED);

    // Configure LED strip (SK6812 RGBW mode)
    led_strip_config_t strip_config = {
        .gpio_num = LED_STRIP_1_GPIO,
        .led_count = LED_COUNT,
        .type = LED_STRIP_TYPE_RGBW,  // SK6812 with white channel
        .rmt_resolution_hz = 0,       // Use default 10MHz
    };

    ret = led_strip_create(&strip_config, &g_led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "✗ Failed to create LED strip: %s", esp_err_to_name(ret));
        return;
    }

    // Clear strip initially
    led_strip_clear(g_led_strip);
    ESP_LOGI(TAG, "✓ LED strip initialized (GPIO %d, %d LEDs)", LED_STRIP_1_GPIO, LED_COUNT);

    // Initialize and start Zigbee
    ret = zigbee_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "✗ Failed to initialize Zigbee: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "✓ Zigbee stack initialized as Router");

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

