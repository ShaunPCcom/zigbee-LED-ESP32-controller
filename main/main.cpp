/**
 * @file main.c
 * @brief Main entry point for Zigbee LED Controller
 *
 * Phase 1 (v1.1.0): C++ shared components integration
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
#include "board_config.h"
#include "config_storage.h"
#include "led_cli.h"
#include "segment_manager.h"
#include "preset_manager.h"
#include "transition_engine.h"

/* C++ shared components */
#include "board_led.hpp"
#include "zigbee_button.hpp"

static const char *TAG = "main";

/* Global instances of C++ shared components */
static BoardLed *g_board_led = nullptr;
static ButtonHandler *g_button = nullptr;

/* Per-strip LED counts — loaded from NVS, used by LED driver and Zigbee init */
extern "C" {
    uint16_t g_strip_count[2] = {LED_STRIP_1_COUNT, LED_STRIP_2_COUNT};
}

extern "C" void app_main(void)
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

    /* Initialize transition engine (200Hz for smooth transitions) */
    ESP_ERROR_CHECK(transition_engine_init(200));
    ESP_LOGI(TAG, "Transition engine initialized at 200Hz");

    /* Register all segment transitions with the engine */
    {
        segment_light_t *state = segment_state_get();
        for (int i = 0; i < MAX_SEGMENTS; i++) {
            ESP_ERROR_CHECK(transition_register(&state[i].level_trans));
            ESP_ERROR_CHECK(transition_register(&state[i].hue_trans));
            ESP_ERROR_CHECK(transition_register(&state[i].sat_trans));
            ESP_ERROR_CHECK(transition_register(&state[i].ct_trans));
        }
        ESP_LOGI(TAG, "Registered %d transitions (4 per segment)", MAX_SEGMENTS * 4);
    }

    /* Initialize transition current values from loaded state */
    segment_manager_init_transitions();

    /* Initialize preset manager */
    preset_manager_init();

    /* Apply per-segment power-on behavior (StartUpOnOff) */
    {
        segment_light_t *state = segment_state_get();
        for (int i = 0; i < MAX_SEGMENTS; i++) {
            switch (state[i].startup_on_off) {
            case 0x00: state[i].on = false;       break;
            case 0x01: state[i].on = true;        break;
            case 0x02: state[i].on = !state[i].on; break;
            default:   break;  /* DEFAULT_STARTUP_ON_OFF = previous, no change */
            }
        }
    }

    /* Initialize board LED status (C++ BoardLed class) */
    g_board_led = new BoardLed(BOARD_LED_GPIO);
    g_board_led->set_state(BoardLed::State::NOT_JOINED);

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

    /* Initialize button handler (C++ ButtonHandler class) */
    g_button = new ButtonHandler(BOARD_BUTTON_GPIO,
                                  BOARD_BUTTON_HOLD_ZIGBEE_MS,
                                  BOARD_BUTTON_HOLD_FULL_MS);
    g_button->set_network_reset_callback(zigbee_factory_reset);
    g_button->set_full_reset_callback(zigbee_full_factory_reset);
    g_button->set_led_callback([](int state) {
        switch (state) {
            case 0: /* Restore previous state */
                extern bool s_network_joined;
                g_board_led->set_state(s_network_joined ? BoardLed::State::JOINED : BoardLed::State::NOT_JOINED);
                break;
            case 1: /* Amber (NOT_JOINED) */
                g_board_led->set_state(BoardLed::State::NOT_JOINED);
                break;
            case 2: /* Red (ERROR) */
                g_board_led->set_state(BoardLed::State::ERROR);
                break;
        }
    });
    g_button->start();
    ESP_LOGI(TAG, "Button handler started (GPIO %d)", BOARD_BUTTON_GPIO);

    ESP_LOGI(TAG, "Device ready! Waiting for Zigbee network pairing...");
    ESP_LOGI(TAG, "Button: 3s=Zigbee reset, 10s=Full reset");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "Uptime: %lld s", esp_timer_get_time() / 1000000);
    }
}

/* C wrappers for C++ BoardLed API (called from zigbee_handlers.c) */
extern "C" void board_led_set_state_off(void) {
    if (g_board_led) g_board_led->set_state(BoardLed::State::OFF);
}

extern "C" void board_led_set_state_not_joined(void) {
    if (g_board_led) g_board_led->set_state(BoardLed::State::NOT_JOINED);
}

extern "C" void board_led_set_state_pairing(void) {
    if (g_board_led) g_board_led->set_state(BoardLed::State::PAIRING);
}

extern "C" void board_led_set_state_joined(void) {
    if (g_board_led) g_board_led->set_state(BoardLed::State::JOINED);
}

extern "C" void board_led_set_state_error(void) {
    if (g_board_led) g_board_led->set_state(BoardLed::State::ERROR);
}
