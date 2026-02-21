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
#include "segment_manager.h"
#include "preset_manager.h"
#include "zigbee_handlers.h"

static const char *TAG = "led_cli";

extern uint16_t g_strip_count[2];

static void print_help(void)
{
    printf(
        "\nLED Controller CLI commands:\n"
        "  led help\n"
        "  led count <strip> <n>      (strip=1|2, n=1-500, saves to NVS, reboot to apply)\n"
        "  led config                 (show current configuration)\n"
        "  led seg                    (show all segments)\n"
        "  led seg <1-8>              (show one segment)\n"
        "  led seg <1-8> start <n>    (set start LED index)\n"
        "  led seg <1-8> count <n>    (set LED count, 0=disable)\n"
        "  led seg <1-8> strip <n>    (set physical strip, 1 or 2)\n"
        "  led preset                 (list all preset slots)\n"
        "  led preset save <slot> [name]  (save current state to slot 0-7)\n"
        "  led preset apply <slot>    (recall preset from slot 0-7)\n"
        "  led preset delete <slot>   (delete preset from slot 0-7)\n"
        "  led transition             (show current global transition time)\n"
        "  led transition <ms>        (set global transition time in ms, 0-65535)\n"
        "  led nvs                    (NVS health check)\n"
        "  led reboot                 (restart device)\n"
        "  led repair                 (Zigbee network reset / re-pair)\n"
        "  led factory-reset          (FULL reset: erase Zigbee + NVS config)\n\n"
    );
}

static void print_segments(int which)
{
    segment_geom_t  *geom  = segment_geom_get();
    segment_light_t *state = segment_state_get();
    int from = (which >= 1 && which <= MAX_SEGMENTS) ? which - 1 : 0;
    int to   = (which >= 1 && which <= MAX_SEGMENTS) ? which - 1 : MAX_SEGMENTS - 1;
    for (int i = from; i <= to; i++) {
        printf("seg%d: start=%u count=%u strip=%u | on=%d level=%u mode=%d hue=%u sat=%u ct=%u\n",
               i + 1, geom[i].start, geom[i].count, geom[i].strip_id + 1,
               state[i].on, state[i].level, state[i].color_mode,
               state[i].hue, state[i].saturation, state[i].color_temp);
    }
}

static void print_config(void)
{
    printf("config: strip1=%u@GPIO%d strip2=%u@GPIO%d\n",
           g_strip_count[0], LED_STRIP_1_GPIO,
           g_strip_count[1], LED_STRIP_2_GPIO);
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

            if (strcmp(cmd, "seg") == 0) {
                char *arg1 = strtok(NULL, " \t\r\n");
                if (!arg1) { print_segments(0); continue; }
                int seg_num = atoi(arg1);
                if (seg_num < 1 || seg_num > MAX_SEGMENTS) {
                    printf("error: segment must be 1-%d\n", MAX_SEGMENTS);
                    continue;
                }
                char *field = strtok(NULL, " \t\r\n");
                if (!field) { print_segments(seg_num); continue; }
                char *val_s = strtok(NULL, " \t\r\n");
                if (!val_s) {
                    printf("usage: led seg %d %s <value>\n", seg_num, field);
                    continue;
                }
                int val = atoi(val_s);
                int idx = seg_num - 1;
                segment_geom_t *geom = segment_geom_get();
                if (strcmp(field, "start") == 0) {
                    if (val < 0 || val > 65535) { printf("error: start must be 0-65535\n"); continue; }
                    geom[idx].start = (uint16_t)val;
                    printf("seg%d start=%u\n", seg_num, geom[idx].start);
                } else if (strcmp(field, "count") == 0) {
                    if (val < 0 || val > 65535) { printf("error: count must be 0-65535\n"); continue; }
                    geom[idx].count = (uint16_t)val;
                    printf("seg%d count=%u\n", seg_num, geom[idx].count);
                } else if (strcmp(field, "strip") == 0) {
                    if (val < 1 || val > 2) { printf("error: strip must be 1 or 2\n"); continue; }
                    geom[idx].strip_id = (uint8_t)(val - 1);
                    printf("seg%d strip=%u\n", seg_num, val);
                } else {
                    printf("unknown field '%s' (start|count|strip)\n", field);
                    continue;
                }
                segment_manager_save();
                continue;
            }

            if (strcmp(cmd, "count") == 0) {
                char *s = strtok(NULL, " \t\r\n");
                char *v = strtok(NULL, " \t\r\n");
                if (!s || !v) { printf("usage: led count <strip> <n>  (strip=1|2, n=1-500)\n"); continue; }
                int strip = atoi(s);
                int cnt = atoi(v);
                if (strip < 1 || strip > 2) {
                    printf("error: strip must be 1 or 2\n");
                    continue;
                }
                if (cnt < 0 || cnt > 500) {
                    printf("error: count must be 0-500 (0=disable)\n");
                    continue;
                }
                esp_err_t err = config_storage_save_strip_count((uint8_t)(strip - 1), (uint16_t)cnt);
                if (err == ESP_OK) {
                    printf("strip%d count=%d saved (reboot to apply)\n", strip, cnt);
                } else {
                    printf("error saving strip count: %s\n", esp_err_to_name(err));
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

            if (strcmp(cmd, "preset") == 0) {
                char *subcmd = strtok(NULL, " \t\r\n");
                if (!subcmd) {
                    /* List all presets */
                    preset_manager_list_presets();
                    continue;
                }

                if (strcmp(subcmd, "save") == 0) {
                    char *slot_str = strtok(NULL, " \t\r\n");
                    if (!slot_str) {
                        printf("usage: led preset save <slot> [name]\n");
                        continue;
                    }
                    int slot = atoi(slot_str);
                    if (slot < 0 || slot >= MAX_PRESET_SLOTS) {
                        printf("error: slot must be 0-%d\n", MAX_PRESET_SLOTS - 1);
                        continue;
                    }
                    char *name = strtok(NULL, "\r\n");  /* Get rest of line for name */
                    while (name && isspace((unsigned char)*name)) name++;  /* Skip leading spaces */
                    esp_err_t err = preset_manager_save((uint8_t)slot, name);
                    if (err == ESP_OK) {
                        printf("Preset saved to slot %d\n", slot);
                    } else {
                        printf("Failed to save preset: %s\n", esp_err_to_name(err));
                    }
                    continue;
                }

                if (strcmp(subcmd, "apply") == 0) {
                    char *slot_str = strtok(NULL, " \t\r\n");
                    if (!slot_str) {
                        printf("usage: led preset apply <slot>\n");
                        continue;
                    }
                    int slot = atoi(slot_str);
                    if (slot < 0 || slot >= MAX_PRESET_SLOTS) {
                        printf("error: slot must be 0-%d\n", MAX_PRESET_SLOTS - 1);
                        continue;
                    }
                    esp_err_t err = preset_manager_recall((uint8_t)slot);
                    if (err == ESP_OK) {
                        printf("Preset applied from slot %d\n", slot);
                        update_leds();
                        schedule_save();
                        /* Defer ZCL sync to Zigbee task to avoid critical section mismatch */
                        schedule_zcl_sync();
                    } else if (err == ESP_ERR_NOT_FOUND) {
                        printf("Slot %d is empty\n", slot);
                    } else {
                        printf("Failed to apply preset: %s\n", esp_err_to_name(err));
                    }
                    continue;
                }

                if (strcmp(subcmd, "delete") == 0) {
                    char *slot_str = strtok(NULL, " \t\r\n");
                    if (!slot_str) {
                        printf("usage: led preset delete <slot>\n");
                        continue;
                    }
                    int slot = atoi(slot_str);
                    if (slot < 0 || slot >= MAX_PRESET_SLOTS) {
                        printf("error: slot must be 0-%d\n", MAX_PRESET_SLOTS - 1);
                        continue;
                    }
                    esp_err_t err = preset_manager_delete((uint8_t)slot);
                    if (err == ESP_OK) {
                        printf("Preset deleted from slot %d\n", slot);
                    } else {
                        printf("Failed to delete preset: %s\n", esp_err_to_name(err));
                    }
                    continue;
                }

                printf("unknown preset command '%s'\n", subcmd);
                continue;
            }

            if (strcmp(cmd, "transition") == 0) {
                char *ms_str = strtok(NULL, " \t\r\n");
                if (!ms_str) {
                    printf("global_transition_ms = %u ms\n",
                           zigbee_handlers_get_global_transition_ms());
                } else {
                    int ms = atoi(ms_str);
                    if (ms < 0 || ms > 65535) {
                        printf("error: ms must be 0-65535\n");
                    } else {
                        zigbee_handlers_set_global_transition_ms((uint16_t)ms);
                        printf("global_transition_ms = %u ms\n", (uint16_t)ms);
                    }
                }
                continue;
            }

            if (strcmp(cmd, "repair") == 0) {
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
