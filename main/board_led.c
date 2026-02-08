/**
 * @file board_led.c
 * @brief Status indication via onboard WS2812 LED (GPIO8, RMT)
 *
 * Uses the ESP-IDF 5.x RMT TX API with a bytes encoder.
 * WS2812B: GRB byte order, 24-bit per pixel.
 * RMT resolution 10 MHz (100 ns/tick). Timing:
 *   bit0: 400 ns high, 800 ns low
 *   bit1: 800 ns high, 400 ns low
 *   reset: idle low >50 µs (satisfied by inter-timer gap)
 */

#include "board_led.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/rmt_tx.h"

static const char *TAG = "board_led";

#define TIMED_STATE_US              (5 * 1000 * 1000)   /* 5 seconds */
#define RMT_RESOLUTION_HZ           10000000             /* 10 MHz, 100 ns/tick */

static board_led_state_t    s_state      = BOARD_LED_OFF;
static esp_timer_handle_t   s_blink_timer  = NULL;
static esp_timer_handle_t   s_timeout_timer = NULL;
static bool                 s_blink_on   = false;

static rmt_channel_handle_t s_rmt_chan   = NULL;
static rmt_encoder_handle_t s_bytes_enc  = NULL;

static void status_apply(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_rmt_chan) return;
    /* WS2812B: GRB byte order */
    uint8_t grb[3] = {g, r, b};
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    rmt_transmit(s_rmt_chan, s_bytes_enc, grb, sizeof(grb), &tx_cfg);
}

static void status_clear(void)
{
    status_apply(0, 0, 0);
}

static void blink_cb(void *arg)
{
    (void)arg;
    s_blink_on = !s_blink_on;

    switch (s_state) {
    case BOARD_LED_NOT_JOINED:
        if (s_blink_on) status_apply(40, 20, 0);   /* amber */
        else            status_clear();
        break;
    case BOARD_LED_PAIRING:
        if (s_blink_on) status_apply(0, 0, 40);    /* blue */
        else            status_clear();
        break;
    case BOARD_LED_ERROR:
        if (s_blink_on) status_apply(60, 0, 0);    /* red */
        else            status_clear();
        break;
    default:
        break;
    }
}

static void timeout_cb(void *arg)
{
    (void)arg;
    switch (s_state) {
    case BOARD_LED_JOINED:
        board_led_set_state(BOARD_LED_OFF);
        break;
    case BOARD_LED_ERROR:
        board_led_set_state(BOARD_LED_PAIRING);
        break;
    default:
        break;
    }
}

void board_led_init(void)
{
    /* Create RMT TX channel for onboard WS2812 */
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num        = BOARD_LED_GPIO,
        .clk_src         = RMT_CLK_SRC_DEFAULT,
        .resolution_hz   = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &s_rmt_chan));

    /* Bytes encoder: WS2812B timing at 10 MHz */
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .duration0 = 4, .level0 = 1,   /* 400 ns high */
                  .duration1 = 8, .level1 = 0 },  /* 800 ns low  */
        .bit1 = { .duration0 = 8, .level0 = 1,   /* 800 ns high */
                  .duration1 = 4, .level1 = 0 },  /* 400 ns low  */
        .flags = { .msb_first = 1 },
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc_cfg, &s_bytes_enc));
    ESP_ERROR_CHECK(rmt_enable(s_rmt_chan));

    /* Status timers */
    const esp_timer_create_args_t blink_args = {
        .callback = blink_cb,
        .name     = "led_blink",
    };
    ESP_ERROR_CHECK(esp_timer_create(&blink_args, &s_blink_timer));

    const esp_timer_create_args_t timeout_args = {
        .callback = timeout_cb,
        .name     = "led_timeout",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timeout_args, &s_timeout_timer));

    ESP_LOGI(TAG, "Onboard WS2812 status LED on GPIO%d (RMT)", BOARD_LED_GPIO);
}

void board_led_set_state(board_led_state_t state)
{
    s_state    = state;
    s_blink_on = false;

    esp_timer_stop(s_blink_timer);
    esp_timer_stop(s_timeout_timer);

    switch (state) {
    case BOARD_LED_OFF:
        status_clear();
        break;

    case BOARD_LED_NOT_JOINED:
        /* blinking amber ~2 Hz, indefinite */
        esp_timer_start_periodic(s_blink_timer, 250 * 1000);
        break;

    case BOARD_LED_PAIRING:
        /* blinking blue ~2 Hz, indefinite */
        esp_timer_start_periodic(s_blink_timer, 250 * 1000);
        break;

    case BOARD_LED_JOINED:
        /* solid green for 5 s, then OFF */
        status_apply(0, 60, 0);
        esp_timer_start_once(s_timeout_timer, TIMED_STATE_US);
        break;

    case BOARD_LED_ERROR:
        /* blinking red ~5 Hz for 5 s, then NOT_JOINED */
        esp_timer_start_periodic(s_blink_timer, 100 * 1000);
        esp_timer_start_once(s_timeout_timer, TIMED_STATE_US);
        break;

    default:
        status_clear();
        break;
    }
}
