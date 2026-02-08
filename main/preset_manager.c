/**
 * @file preset_manager.c
 * @brief Manage named presets for segment states
 *
 * NVS storage (namespace "led_cfg"):
 *   "prst_0" through "prst_7": blob, each 129 bytes
 *     - 1 byte:  name length (0-16)
 *     - 16 bytes: name (null-padded)
 *     - 112 bytes: 8 × segment_light_t (14 bytes each)
 */

#include "preset_manager.h"
#include "segment_manager.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "preset";

#define NVS_NAMESPACE    "led_cfg"
#define PRESET_BLOB_SIZE (1 + PRESET_NAME_MAX + MAX_SEGMENTS * sizeof(segment_light_t))

typedef struct {
    uint8_t name_len;
    char name[PRESET_NAME_MAX];
    segment_light_t states[MAX_SEGMENTS];
} preset_blob_t;

static preset_blob_t s_presets[MAX_PRESETS];
static char s_active_preset[PRESET_NAME_MAX + 1] = "";

static const char *s_nvs_keys[MAX_PRESETS] = {
    "prst_0", "prst_1", "prst_2", "prst_3",
    "prst_4", "prst_5", "prst_6", "prst_7"
};

void preset_manager_init(void)
{
    memset(s_presets, 0, sizeof(s_presets));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot open NVS for preset load: %s", esp_err_to_name(err));
        return;
    }

    for (int i = 0; i < MAX_PRESETS; i++) {
        size_t sz = sizeof(preset_blob_t);
        err = nvs_get_blob(h, s_nvs_keys[i], &s_presets[i], &sz);
        if (err == ESP_OK && sz == sizeof(preset_blob_t)) {
            if (s_presets[i].name_len > 0 && s_presets[i].name_len <= PRESET_NAME_MAX) {
                ESP_LOGI(TAG, "Loaded preset %d: %.*s", i,
                         s_presets[i].name_len, s_presets[i].name);
            } else {
                memset(&s_presets[i], 0, sizeof(preset_blob_t));
            }
        }
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Preset manager initialized, %d presets loaded", preset_manager_count());
}

int preset_manager_count(void)
{
    int count = 0;
    for (int i = 0; i < MAX_PRESETS; i++) {
        if (s_presets[i].name_len > 0) {
            count++;
        }
    }
    return count;
}

static int find_preset_by_name(const char *name)
{
    if (!name || name[0] == '\0') return -1;

    size_t len = strlen(name);
    if (len > PRESET_NAME_MAX) return -1;

    for (int i = 0; i < MAX_PRESETS; i++) {
        if (s_presets[i].name_len == len &&
            memcmp(s_presets[i].name, name, len) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_empty_slot(void)
{
    for (int i = 0; i < MAX_PRESETS; i++) {
        if (s_presets[i].name_len == 0) {
            return i;
        }
    }
    return -1;
}

bool preset_manager_save(const char *name)
{
    if (!name || name[0] == '\0') {
        ESP_LOGW(TAG, "Cannot save preset: empty name");
        return false;
    }

    size_t len = strlen(name);
    if (len > PRESET_NAME_MAX) {
        ESP_LOGW(TAG, "Cannot save preset: name too long (%zu > %d)", len, PRESET_NAME_MAX);
        return false;
    }

    /* Find existing preset with same name, or empty slot */
    int slot = find_preset_by_name(name);
    if (slot < 0) {
        slot = find_empty_slot();
        if (slot < 0) {
            ESP_LOGW(TAG, "Cannot save preset: all %d slots full", MAX_PRESETS);
            return false;
        }
    }

    /* Save current segment states (skip startup_on_off field) */
    segment_light_t *states = segment_state_get();
    s_presets[slot].name_len = (uint8_t)len;
    memcpy(s_presets[slot].name, name, len);
    memset(s_presets[slot].name + len, 0, PRESET_NAME_MAX - len);
    memcpy(s_presets[slot].states, states, sizeof(segment_light_t) * MAX_SEGMENTS);

    /* Write to NVS */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_blob(h, s_nvs_keys[slot], &s_presets[slot], sizeof(preset_blob_t));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save preset '%s': %s", name, esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Saved preset '%s' to slot %d", name, slot);
    return true;
}

bool preset_manager_recall(const char *name)
{
    int slot = find_preset_by_name(name);
    if (slot < 0) {
        ESP_LOGW(TAG, "Preset '%s' not found", name);
        return false;
    }

    /* Copy stored states to segment manager (preserve startup_on_off) */
    segment_light_t *states = segment_state_get();
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        uint8_t saved_startup = states[i].startup_on_off;
        memcpy(&states[i], &s_presets[slot].states[i], sizeof(segment_light_t));
        states[i].startup_on_off = saved_startup;
    }

    /* Update active preset name */
    size_t len = s_presets[slot].name_len;
    memcpy(s_active_preset, s_presets[slot].name, len);
    s_active_preset[len] = '\0';

    ESP_LOGI(TAG, "Recalled preset '%s' from slot %d", name, slot);
    return true;
}

bool preset_manager_delete(const char *name)
{
    int slot = find_preset_by_name(name);
    if (slot < 0) {
        ESP_LOGW(TAG, "Preset '%s' not found", name);
        return false;
    }

    /* Clear slot in memory */
    memset(&s_presets[slot], 0, sizeof(preset_blob_t));

    /* Clear active preset if it was this one */
    if (strcmp(s_active_preset, name) == 0) {
        s_active_preset[0] = '\0';
    }

    /* Erase from NVS */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_erase_key(h, s_nvs_keys[slot]);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to delete preset '%s': %s", name, esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Deleted preset '%s' from slot %d", name, slot);
    return true;
}

const char *preset_manager_get_active(void)
{
    return s_active_preset;
}

bool preset_manager_get_name(int slot, char *buf, size_t len)
{
    if (slot < 0 || slot >= MAX_PRESETS || !buf || len == 0) {
        return false;
    }

    if (s_presets[slot].name_len > 0) {
        size_t copy_len = s_presets[slot].name_len;
        if (copy_len >= len) {
            copy_len = len - 1;
        }
        memcpy(buf, s_presets[slot].name, copy_len);
        buf[copy_len] = '\0';
        return true;
    }

    buf[0] = '\0';
    return false;
}
