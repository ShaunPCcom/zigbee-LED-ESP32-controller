/**
 * @file main.c
 * @brief Main entry point for Zigbee LED Controller
 *
 * Phase 1: Basic LED control test (no Zigbee yet)
 * This will test the LED driver with a simple rainbow effect
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "led_driver.h"

static const char *TAG = "main";

// GPIO pin assignments for ESP32-H2-DevKitM-1
#define LED_STRIP_1_GPIO    4
#define LED_STRIP_2_GPIO    5
#define LED_STRIP_3_GPIO    10
#define ONBOARD_LED_GPIO    8

// Test configuration
#define TEST_LED_COUNT      30  // Adjust based on your strip

/**
 * @brief Simple HSV to RGB conversion
 * @param h Hue (0-360)
 * @param s Saturation (0-100)
 * @param v Value/Brightness (0-100)
 * @param r Output red (0-255)
 * @param g Output green (0-255)
 * @param b Output blue (0-255)
 */
static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    h %= 360; // Wrap hue to 0-359

    if (s == 0) {
        // Achromatic (gray)
        *r = *g = *b = v * 255 / 100;
        return;
    }

    uint8_t region = h / 60;
    uint8_t remainder = (h - (region * 60)) * 6;

    uint8_t p = (v * (100 - s)) / 100;
    uint8_t q = (v * (100 - ((s * remainder) / 360))) / 100;
    uint8_t t = (v * (100 - ((s * (360 - remainder)) / 360))) / 100;

    v = v * 255 / 100;
    p = p * 255 / 100;
    q = q * 255 / 100;
    t = t * 255 / 100;

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
 * @brief LED test task - displays a rainbow pattern
 */
static void led_test_task(void *pvParameters)
{
    led_strip_handle_t strip = (led_strip_handle_t)pvParameters;

    ESP_LOGI(TAG, "Starting LED test - rainbow effect");

    uint16_t hue = 0;

    while (1) {
        // Create rainbow effect across the strip
        for (uint16_t i = 0; i < TEST_LED_COUNT; i++) {
            uint16_t pixel_hue = (hue + (i * 360 / TEST_LED_COUNT)) % 360;
            uint8_t r, g, b;
            hsv_to_rgb(pixel_hue, 100, 30, &r, &g, &b);  // 30% brightness
            led_strip_set_pixel_rgb(strip, i, r, g, b);
        }

        // Update the strip
        led_strip_refresh(strip);

        // Rotate the rainbow
        hue = (hue + 2) % 360;

        vTaskDelay(pdMS_TO_TICKS(50));  // 50ms delay = ~20 FPS
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Zigbee LED Controller starting...");
    ESP_LOGI(TAG, "Phase 1: LED Driver Test");

    // Initialize NVS (required for future Zigbee and config storage)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Configure LED strip 1 (test with first strip only for now)
    led_strip_config_t strip_config = {
        .gpio_num = LED_STRIP_1_GPIO,
        .led_count = TEST_LED_COUNT,
        .type = LED_STRIP_TYPE_RGB,  // Change to LED_STRIP_TYPE_RGBW for SK6812
        .rmt_resolution_hz = 0,      // Use default 10MHz
    };

    led_strip_handle_t strip1 = NULL;
    ret = led_strip_create(&strip_config, &strip1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED strip: %s", esp_err_to_name(ret));
        return;
    }

    // Clear strip initially
    led_strip_clear(strip1);

    ESP_LOGI(TAG, "LED strip initialized on GPIO %d with %d LEDs", LED_STRIP_1_GPIO, TEST_LED_COUNT);
    ESP_LOGI(TAG, "Starting rainbow test...");

    // Create task to run LED animation
    xTaskCreate(led_test_task, "led_test", 4096, strip1, 5, NULL);

    // Main loop - future Zigbee handling will go here
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
