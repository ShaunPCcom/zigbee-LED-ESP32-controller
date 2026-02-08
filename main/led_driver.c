/**
 * @file led_driver.c
 * @brief SPI-based LED driver with time-multiplexed dual strip support
 *
 * SK6812 RGBW strips (GRBW byte order). SPI at 2.5 MHz encodes each LED bit
 * as 3 SPI bits:
 *   0 -> 100  (high 400 ns, low 800 ns)
 *   1 -> 110  (high 800 ns, low 400 ns)
 *
 * Both strips share SPI2. Before each strip transmission the MOSI GPIO is
 * switched via the GPIO matrix.
 */

#include "led_driver.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "soc/spi_periph.h"
#include "esp_rom_gpio.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "led_driver";

#define LED_SPI_CLOCK_HZ    2500000   /* 2.5 MHz -> 400 ns per SPI bit */
#define BYTES_PER_LED       4         /* SK6812: GRBW */
#define SPI_BYTES_PER_LED   12        /* 4 bytes * 3 SPI bytes per LED byte */
#define RESET_BYTES         40        /* 40 * 8 * 400ns = 128 us > 80 us reset */

typedef struct {
    uint8_t  *pixel_buf;
    uint8_t  *spi_buf;
    uint16_t  count;
    size_t    spi_len;
} strip_data_t;

static strip_data_t s_strips[LED_DRIVER_MAX_STRIPS];
static spi_device_handle_t s_spi = NULL;

/* GPIO for each strip */
static const int s_gpio[LED_DRIVER_MAX_STRIPS] = {LED_STRIP_1_GPIO, LED_STRIP_2_GPIO};

/* Pre-computed lookup: each LED byte value -> 3 SPI bytes */
static uint8_t s_lut[256][3];

static void build_lut(void)
{
    for (int v = 0; v < 256; v++) {
        uint32_t bits = 0;
        for (int i = 7; i >= 0; i--) {
            bits = (bits << 3) | ((v & (1 << i)) ? 0b110u : 0b100u);
        }
        s_lut[v][0] = (bits >> 16) & 0xFF;
        s_lut[v][1] = (bits >> 8) & 0xFF;
        s_lut[v][2] = bits & 0xFF;
    }
}

static void encode_strip(uint8_t strip_id)
{
    strip_data_t *s = &s_strips[strip_id];
    if (!s->pixel_buf || !s->spi_buf || s->count == 0) return;

    const uint8_t *src = s->pixel_buf;
    uint8_t *dst = s->spi_buf;
    size_t n = (size_t)s->count * BYTES_PER_LED;

    for (size_t i = 0; i < n; i++) {
        dst[0] = s_lut[src[i]][0];
        dst[1] = s_lut[src[i]][1];
        dst[2] = s_lut[src[i]][2];
        dst += 3;
    }
    memset(dst, 0, RESET_BYTES);
}

static void mosi_connect(int gpio_num)
{
    gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(gpio_num,
        spi_periph_signal[SPI2_HOST].spid_out, false, false);
}

static void mosi_idle(int gpio_num)
{
    /* Disconnect from SPI peripheral, drive low for LED reset */
    gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio_num, 0);
}

/* ---------- Public API ---------- */

esp_err_t led_driver_init(uint16_t count0, uint16_t count1)
{
    build_lut();

    uint16_t counts[LED_DRIVER_MAX_STRIPS] = {count0, count1};

    spi_bus_config_t bus = {
        .mosi_io_num   = LED_STRIP_1_GPIO,
        .miso_io_num   = -1,
        .sclk_io_num   = -1,
        .quadhd_io_num = -1,
        .quadwp_io_num = -1,
        .max_transfer_sz = 0,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    spi_device_interface_config_t dev = {
        .clock_speed_hz = LED_SPI_CLOCK_HZ,
        .mode           = 0,
        .spics_io_num   = -1,
        .queue_size     = 1,
        .flags          = SPI_DEVICE_NO_DUMMY,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev, &s_spi),
                        TAG, "SPI add device failed");

    for (int i = 0; i < LED_DRIVER_MAX_STRIPS; i++) {
        s_strips[i].count = counts[i];
        if (counts[i] == 0) {
            gpio_set_direction(s_gpio[i], GPIO_MODE_OUTPUT);
            gpio_set_level(s_gpio[i], 0);
            continue;
        }

        size_t pix_sz = (size_t)counts[i] * BYTES_PER_LED;
        size_t spi_sz = (size_t)counts[i] * SPI_BYTES_PER_LED + RESET_BYTES;
        s_strips[i].spi_len = spi_sz;

        s_strips[i].pixel_buf = calloc(1, pix_sz);
        s_strips[i].spi_buf   = heap_caps_calloc(1, spi_sz, MALLOC_CAP_DMA);
        if (!s_strips[i].pixel_buf || !s_strips[i].spi_buf) {
            ESP_LOGE(TAG, "No memory for strip %d", i);
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "LED driver ready: strip0=%u@GPIO%d strip1=%u@GPIO%d",
             count0, LED_STRIP_1_GPIO, count1, LED_STRIP_2_GPIO);
    return ESP_OK;
}

esp_err_t led_driver_set_pixel(uint8_t strip, uint16_t idx,
                                uint8_t r, uint8_t g, uint8_t b, uint8_t w)
{
    if (strip >= LED_DRIVER_MAX_STRIPS) return ESP_ERR_INVALID_ARG;
    strip_data_t *s = &s_strips[strip];
    if (!s->pixel_buf || idx >= s->count) return ESP_ERR_INVALID_ARG;

    uint8_t *p = s->pixel_buf + (size_t)idx * BYTES_PER_LED;
    p[0] = g;
    p[1] = r;
    p[2] = b;
    p[3] = w;
    return ESP_OK;
}

esp_err_t led_driver_clear(uint8_t strip)
{
    if (strip >= LED_DRIVER_MAX_STRIPS) return ESP_ERR_INVALID_ARG;
    strip_data_t *s = &s_strips[strip];
    if (s->pixel_buf && s->count > 0) {
        memset(s->pixel_buf, 0, (size_t)s->count * BYTES_PER_LED);
    }
    return ESP_OK;
}

esp_err_t led_driver_refresh(void)
{
    for (int i = 0; i < LED_DRIVER_MAX_STRIPS; i++) {
        strip_data_t *s = &s_strips[i];
        if (s->count == 0 || !s->spi_buf) continue;

        encode_strip(i);
        mosi_connect(s_gpio[i]);

        spi_transaction_t t = {
            .length    = s->spi_len * 8,
            .tx_buffer = s->spi_buf,
        };
        esp_err_t err = spi_device_transmit(s_spi, &t);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPI transmit failed strip %d: %s", i, esp_err_to_name(err));
        }
        mosi_idle(s_gpio[i]);
    }
    return ESP_OK;
}

uint16_t led_driver_get_count(uint8_t strip)
{
    if (strip >= LED_DRIVER_MAX_STRIPS) return 0;
    return s_strips[strip].count;
}
