/**
 * @file led_cli.c
 * @brief Serial CLI for LED controller configuration
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "nvs.h"

#include "led_cli.h"
#include "led_driver.h"
#include "board_config.h"
#include "config_storage.h"
#include "zigbee_handlers.h"

static const char *TAG = "led_cli";

extern uint16_t g_led_count;

static void print_help(void)
{
    printf(
        "\nLED Controller CLI commands:\n"
        "  led help\n"
        "  led count <n>          (1-500, saves to NVS, reboot to apply)\n"
        "  led config             (show current configuration)\n"
        "  led nvs                (NVS health check)\n"
        "  led reboot             (restart device)\n"
        "  led repaire            (Zigbee network reset / re-pair)\n"
        "  led factory-reset      (FULL reset: erase Zigbee + NVS config)\n\n"
    );
}

static void print_config(void)
{
    printf("config: led_count=%u (GPIO %d, type=%s)\n",
           g_led_count, LED_STRIP_1_GPIO,
           (LED_STRIP_TYPE == LED_STRIP_TYPE_RGBW) ? "RGBW" : "RGB");
}

static void cli_task(void *arg)
{
    (void)arg;

    print_help();

    const uart_port_t console_uart = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;

    char line[128];
    size_t len = 0;

    while (1) {
        uint8_t ch;
        int n = uart_read_bytes(console_uart, &ch, 1, pdMS_TO_TICKS(100));
        if (n <= 0) {
            continue;
        }

        /* Echo */
        uart_write_bytes(console_uart, (const char *)&ch, 1);

        if (ch == '\r' || ch == '\n') {
            line[len] = '\0';
            len = 0;

            char *p = line;
            while (*p && isspace((unsigned char)*p)) p++;

            /* Must start with "led" followed by whitespace or end of string */
            if (strncmp(p, "led", 3) != 0 || (p[3] && !isspace((unsigned char)p[3]))) {
                continue;
            }
            p += 3;
            while (*p && isspace((unsigned char)*p)) p++;

            char *cmd = strtok(p, " \t\r\n");
            if (!cmd) { print_help(); continue; }

            if (strcmp(cmd, "help") == 0) { print_help(); continue; }
            if (strcmp(cmd, "config") == 0) { print_config(); continue; }

            if (strcmp(cmd, "count") == 0) {
                char *v = strtok(NULL, " \t\r\n");
                if (!v) { printf("usage: led count <n>  (1-500)\n"); continue; }
                int cnt = atoi(v);
                if (cnt < 1 || cnt > 500) {
                    printf("error: count must be 1-500\n");
                    continue;
                }
                esp_err_t err = config_storage_save_led_count((uint16_t)cnt);
                if (err == ESP_OK) {
                    printf("led_count=%d saved (reboot to apply)\n", cnt);
                } else {
                    printf("error saving led_count: %s\n", esp_err_to_name(err));
                }
                continue;
            }

            if (strcmp(cmd, "nvs") == 0) {
                printf("=== NVS Health Check ===\n");

                nvs_stats_t nvs_stats;
                esp_err_t err = nvs_get_stats(NULL, &nvs_stats);
                if (err == ESP_OK) {
                    printf("NVS stats:\n");
                    printf("  Used:  %zu\n", nvs_stats.used_entries);
                    printf("  Free:  %zu\n", nvs_stats.free_entries);
                    printf("  Total: %zu\n", nvs_stats.total_entries);
                    printf("  Namespaces: %zu\n", nvs_stats.namespace_count);
                } else {
                    printf("Failed to get NVS stats: %s\n", esp_err_to_name(err));
                }

                printf("\nTesting NVS write/read...\n");
                nvs_handle_t h;
                err = nvs_open("led_cfg", NVS_READWRITE, &h);
                if (err != ESP_OK) {
                    printf("  nvs_open FAILED: %s\n", esp_err_to_name(err));
                    continue;
                }

                uint32_t test_val = 0xDEADBEEF;
                err = nvs_set_u32(h, "nvs_test", test_val);
                if (err != ESP_OK) {
                    printf("  nvs_set_u32 FAILED: %s\n", esp_err_to_name(err));
                    nvs_close(h);
                    continue;
                }
                err = nvs_commit(h);
                if (err != ESP_OK) {
                    printf("  nvs_commit FAILED: %s\n", esp_err_to_name(err));
                    nvs_close(h);
                    continue;
                }
                uint32_t read_val = 0;
                err = nvs_get_u32(h, "nvs_test", &read_val);
                nvs_close(h);
                if (err != ESP_OK) {
                    printf("  nvs_get_u32 FAILED: %s\n", esp_err_to_name(err));
                } else if (read_val != test_val) {
                    printf("  MISMATCH: wrote 0x%08X, read 0x%08X\n",
                           (unsigned)test_val, (unsigned)read_val);
                } else {
                    printf("  Write/read PASSED (0x%08X)\n", (unsigned)read_val);
                }
                continue;
            }

            if (strcmp(cmd, "factory-reset") == 0) {
                printf("FULL FACTORY RESET: Erasing Zigbee + NVS config...\n");
                fflush(stdout);
                vTaskDelay(pdMS_TO_TICKS(100));
                zigbee_full_factory_reset();
                continue;
            }

            if (strcmp(cmd, "repaire") == 0) {
                printf("Zigbee network reset (re-pair)...\n");
                fflush(stdout);
                vTaskDelay(pdMS_TO_TICKS(100));
                zigbee_factory_reset();
                continue;
            }

            if (strcmp(cmd, "reboot") == 0) {
                printf("Rebooting...\n");
                fflush(stdout);
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
                continue;
            }

            printf("unknown command\n");
            print_help();
        }

        /* Backspace / delete */
        if (ch == 0x7f || ch == 0x08) {
            if (len > 0) len--;
            continue;
        }

        if (isprint((unsigned char)ch) && len + 1 < sizeof(line)) {
            line[len++] = (char)ch;
        }
    }
}

void led_cli_start(void)
{
    const uart_port_t console_uart = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;

    esp_err_t err = uart_driver_install(console_uart, 1024, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install(uart=%d) failed: %s",
                 (int)console_uart, esp_err_to_name(err));
        return;
    }

    BaseType_t ok = xTaskCreate(cli_task, "led_cli", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start CLI task");
    }
}
