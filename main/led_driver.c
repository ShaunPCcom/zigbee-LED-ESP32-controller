/**
 * @file led_driver.c
 * @brief LED strip driver implementation using RMT peripheral
 *
 * WS2812B/SK6812 timing requirements:
 * - T0H (0 bit high): ~0.4us
 * - T0L (0 bit low): ~0.85us
 * - T1H (1 bit high): ~0.8us
 * - T1L (1 bit low): ~0.45us
 * - Reset: >50us low
 */

#include "led_driver.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include <string.h>

static const char *TAG = "led_driver";

#define LED_STRIP_RMT_RESOLUTION_HZ 10000000 // 10MHz = 0.1us per tick
#define LED_STRIP_RESET_TIME_US     50       // Reset time in microseconds

// WS2812B/SK6812 timing in 0.1us ticks (at 10MHz resolution)
#define LED_T0H_TICKS  4  // 0.4us
#define LED_T0L_TICKS  8  // 0.8us
#define LED_T1H_TICKS  8  // 0.8us
#define LED_T1L_TICKS  4  // 0.4us

/**
 * @brief LED strip structure (internal)
 */
struct led_strip_t {
    rmt_channel_handle_t rmt_chan;    /*!< RMT channel handle */
    rmt_encoder_handle_t rmt_encoder; /*!< RMT encoder handle */
    uint8_t *pixel_buf;                /*!< Pixel buffer (GRB or GRBW) */
    uint16_t led_count;                /*!< Number of LEDs */
    led_strip_type_t type;             /*!< Strip type */
    uint8_t bytes_per_pixel;           /*!< 3 for RGB, 4 for RGBW */
};

/**
 * @brief RMT encoder for LED strip
 */
typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    rmt_symbol_word_t reset_code;
} led_strip_encoder_t;

/**
 * @brief Encode LED strip data into RMT symbols
 */
static size_t IRAM_ATTR led_strip_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                               const void *primary_data, size_t data_size, rmt_encode_state_t *ret_state)
{
    led_strip_encoder_t *led_encoder = __containerof(encoder, led_strip_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    // Encode the pixel data
    encoded_symbols += led_encoder->bytes_encoder->encode(led_encoder->bytes_encoder, channel,
                                                           primary_data, data_size, &session_state);
    if (session_state & RMT_ENCODING_COMPLETE) {
        // Send reset code after data
        encoded_symbols += led_encoder->copy_encoder->encode(led_encoder->copy_encoder, channel,
                                                             &led_encoder->reset_code,
                                                             sizeof(led_encoder->reset_code), &session_state);
    }

    *ret_state = session_state;
    return encoded_symbols;
}

/**
 * @brief Delete LED strip encoder
 */
static esp_err_t led_strip_encoder_del(rmt_encoder_t *encoder)
{
    led_strip_encoder_t *led_encoder = __containerof(encoder, led_strip_encoder_t, base);
    rmt_del_encoder(led_encoder->bytes_encoder);
    rmt_del_encoder(led_encoder->copy_encoder);
    free(led_encoder);
    return ESP_OK;
}

/**
 * @brief Reset LED strip encoder
 */
static esp_err_t IRAM_ATTR led_strip_encoder_reset(rmt_encoder_t *encoder)
{
    led_strip_encoder_t *led_encoder = __containerof(encoder, led_strip_encoder_t, base);
    rmt_encoder_reset(led_encoder->bytes_encoder);
    rmt_encoder_reset(led_encoder->copy_encoder);
    return ESP_OK;
}

/**
 * @brief Create LED strip encoder
 */
static esp_err_t led_strip_encoder_create(uint32_t resolution, rmt_encoder_handle_t *ret_encoder)
{
    esp_err_t ret = ESP_OK;
    led_strip_encoder_t *led_encoder = calloc(1, sizeof(led_strip_encoder_t));
    ESP_RETURN_ON_FALSE(led_encoder, ESP_ERR_NO_MEM, TAG, "no mem for LED encoder");

    led_encoder->base.encode = led_strip_encode;
    led_encoder->base.del = led_strip_encoder_del;
    led_encoder->base.reset = led_strip_encoder_reset;

    // Configure bytes encoder for bit-by-bit encoding
    rmt_bytes_encoder_config_t bytes_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = LED_T0H_TICKS,
            .level1 = 0,
            .duration1 = LED_T0L_TICKS,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = LED_T1H_TICKS,
            .level1 = 0,
            .duration1 = LED_T1L_TICKS,
        },
        .flags.msb_first = 1, // WS2812B uses MSB first
    };
    ESP_GOTO_ON_ERROR(rmt_new_bytes_encoder(&bytes_config, &led_encoder->bytes_encoder), err, TAG, "create bytes encoder failed");

    // Configure copy encoder for reset code
    rmt_copy_encoder_config_t copy_config = {};
    ESP_GOTO_ON_ERROR(rmt_new_copy_encoder(&copy_config, &led_encoder->copy_encoder), err, TAG, "create copy encoder failed");

    // Create reset code (>50us low)
    uint32_t reset_ticks = resolution / 1000000 * LED_STRIP_RESET_TIME_US;
    led_encoder->reset_code.level0 = 0;
    led_encoder->reset_code.duration0 = reset_ticks;
    led_encoder->reset_code.level1 = 0;
    led_encoder->reset_code.duration1 = 0;

    *ret_encoder = &led_encoder->base;
    return ESP_OK;

err:
    if (led_encoder) {
        if (led_encoder->bytes_encoder) {
            rmt_del_encoder(led_encoder->bytes_encoder);
        }
        if (led_encoder->copy_encoder) {
            rmt_del_encoder(led_encoder->copy_encoder);
        }
        free(led_encoder);
    }
    return ret;
}

esp_err_t led_strip_create(const led_strip_config_t *config, led_strip_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(config && handle, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(config->led_count > 0, ESP_ERR_INVALID_ARG, TAG, "LED count must be > 0");

    esp_err_t ret = ESP_OK;
    struct led_strip_t *strip = calloc(1, sizeof(struct led_strip_t));
    ESP_RETURN_ON_FALSE(strip, ESP_ERR_NO_MEM, TAG, "no mem for LED strip");

    strip->led_count = config->led_count;
    strip->type = config->type;
    strip->bytes_per_pixel = (config->type == LED_STRIP_TYPE_RGB) ? 3 : 4;

    // Allocate pixel buffer
    size_t buf_size = config->led_count * strip->bytes_per_pixel;
    strip->pixel_buf = calloc(1, buf_size);
    if (!strip->pixel_buf) {
        free(strip);
        ESP_LOGE(TAG, "no mem for pixel buffer");
        return ESP_ERR_NO_MEM;
    }

    // Configure RMT TX channel
    uint32_t resolution = config->rmt_resolution_hz ? config->rmt_resolution_hz : LED_STRIP_RMT_RESOLUTION_HZ;
    rmt_tx_channel_config_t tx_config = {
        .gpio_num = config->gpio_num,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = resolution,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_GOTO_ON_ERROR(rmt_new_tx_channel(&tx_config, &strip->rmt_chan), err, TAG, "create RMT TX channel failed");

    // Create LED strip encoder
    ESP_GOTO_ON_ERROR(led_strip_encoder_create(resolution, &strip->rmt_encoder), err, TAG, "create LED encoder failed");

    // Enable RMT channel
    ESP_GOTO_ON_ERROR(rmt_enable(strip->rmt_chan), err, TAG, "enable RMT channel failed");

    *handle = strip;
    ESP_LOGI(TAG, "LED strip created: GPIO=%d, LEDs=%d, Type=%s",
             config->gpio_num, config->led_count,
             config->type == LED_STRIP_TYPE_RGB ? "RGB" : "RGBW");
    return ESP_OK;

err:
    if (strip) {
        if (strip->rmt_chan) {
            rmt_del_channel(strip->rmt_chan);
        }
        if (strip->rmt_encoder) {
            rmt_del_encoder(strip->rmt_encoder);
        }
        if (strip->pixel_buf) {
            free(strip->pixel_buf);
        }
        free(strip);
    }
    return ret;
}

esp_err_t led_strip_delete(led_strip_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");

    rmt_disable(handle->rmt_chan);
    rmt_del_channel(handle->rmt_chan);
    rmt_del_encoder(handle->rmt_encoder);
    free(handle->pixel_buf);
    free(handle);

    return ESP_OK;
}

esp_err_t led_strip_set_pixel_rgb(led_strip_handle_t handle, uint16_t led_index,
                                   uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    ESP_RETURN_ON_FALSE(led_index < handle->led_count, ESP_ERR_INVALID_ARG, TAG, "LED index out of range");

    // WS2812B uses GRB order
    uint8_t *pixel = handle->pixel_buf + (led_index * handle->bytes_per_pixel);
    pixel[0] = green;
    pixel[1] = red;
    pixel[2] = blue;

    return ESP_OK;
}

esp_err_t led_strip_set_pixel_rgbw(led_strip_handle_t handle, uint16_t led_index,
                                    uint8_t red, uint8_t green, uint8_t blue, uint8_t white)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    ESP_RETURN_ON_FALSE(led_index < handle->led_count, ESP_ERR_INVALID_ARG, TAG, "LED index out of range");
    ESP_RETURN_ON_FALSE(handle->type == LED_STRIP_TYPE_RGBW, ESP_ERR_NOT_SUPPORTED, TAG, "not an RGBW strip");

    // SK6812 uses GRBW order
    uint8_t *pixel = handle->pixel_buf + (led_index * handle->bytes_per_pixel);
    pixel[0] = green;
    pixel[1] = red;
    pixel[2] = blue;
    pixel[3] = white;

    return ESP_OK;
}

esp_err_t led_strip_refresh(led_strip_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");

    rmt_transmit_config_t tx_config = {
        .loop_count = 0, // No loop
    };

    size_t data_size = handle->led_count * handle->bytes_per_pixel;
    return rmt_transmit(handle->rmt_chan, handle->rmt_encoder, handle->pixel_buf, data_size, &tx_config);
}

esp_err_t led_strip_clear(led_strip_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");

    size_t buf_size = handle->led_count * handle->bytes_per_pixel;
    memset(handle->pixel_buf, 0, buf_size);
    return led_strip_refresh(handle);
}
