/**
 * @file board_led.c
 * @brief Onboard LED status indicators implementation
 */

#include "board_led.h"
#include "board_config.h"
#include "led_driver.h"
#include "esp_log.h"

static const char *TAG = "board_led";

static led_strip_handle_t s_board_led = NULL;
static board_led_state_t s_current_state = BOARD_LED_OFF;
static uint32_t s_blink_counter = 0;

void board_led_init(void)
{
    // Create LED strip for onboard LED (single WS2812)
    led_strip_config_t config = {
        .gpio_num = BOARD_LED_GPIO,
        .led_count = 1,
        .type = LED_STRIP_TYPE_RGB,
        .rmt_resolution_hz = 0,  // Use default
    };

    esp_err_t ret = led_strip_create(&config, &s_board_led);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create board LED: %s", esp_err_to_name(ret));
        return;
    }

    // Start with LED off
    led_strip_clear(s_board_led);
    ESP_LOGI(TAG, "Board LED initialized on GPIO %d", BOARD_LED_GPIO);
}

void board_led_set_state(board_led_state_t state)
{
    s_current_state = state;
    s_blink_counter = 0;  // Reset blink counter on state change
}

void board_led_update(void)
{
    if (!s_board_led) return;

    s_blink_counter++;

    uint8_t r = 0, g = 0, b = 0;
    bool led_on = false;

    switch (s_current_state) {
    case BOARD_LED_OFF:
        // LED off
        led_on = false;
        break;

    case BOARD_LED_NOT_JOINED:
        // Slow blink (1Hz) - blue
        led_on = (s_blink_counter / 10) % 2 == 0;
        r = 0; g = 0; b = 30;
        break;

    case BOARD_LED_PAIRING:
        // Fast blink (5Hz) - cyan
        led_on = (s_blink_counter / 2) % 2 == 0;
        r = 0; g = 30; b = 30;
        break;

    case BOARD_LED_JOINED:
        // Solid green
        led_on = true;
        r = 0; g = 30; b = 0;
        break;

    case BOARD_LED_ERROR:
        // Very fast blink (10Hz) - red
        led_on = s_blink_counter % 2 == 0;
        r = 30; g = 0; b = 0;
        break;
    }

    if (led_on) {
        led_strip_set_pixel_rgb(s_board_led, 0, r, g, b);
    } else {
        led_strip_set_pixel_rgb(s_board_led, 0, 0, 0, 0);
    }

    led_strip_refresh(s_board_led);
}
