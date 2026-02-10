/**
 * @file preset_manager.c
 * @brief Manage slot-based presets for segment states
 *
 * NVS storage (namespace "led_cfg"):
 *   "prst_0" through "prst_7": blob, each 129 bytes
 *     - 1 byte:  name length (0-16)
 *     - 16 bytes: name (UTF-8, no null terminator)
 *     - 112 bytes: 8 × segment_light_t (14 bytes each)
 *   "prst_version": u8, version flag (2 = slot-based)
 */

#include "preset_manager.h"
#include "segment_manager.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "preset";

#define NVS_NAMESPACE     "led_cfg"
#define NVS_VERSION_KEY   "prst_version"
#define PRESET_VERSION_V2 2
#define PRESET_BLOB_SIZE  (1 + PRESET_NAME_MAX + MAX_SEGMENTS * sizeof(segment_light_t))

typedef struct {
    uint8_t name_length;
    char name[PRESET_NAME_MAX];
    segment_light_t segments[MAX_SEGMENTS];
} preset_slot_t;

static preset_slot_t s_slots[MAX_PRESET_SLOTS];

static const char *s_nvs_keys[MAX_PRESET_SLOTS] = {
    "prst_0", "prst_1", "prst_2", "prst_3",
    "prst_4", "prst_5", "prst_6", "prst_7"
};

/**
 * @brief Migrate legacy name-based presets to slot-based with default names
 */
static esp_err_t migrate_legacy_presets(nvs_handle_t h)
{
    ESP_LOGI(TAG, "Migrating legacy presets to slot-based (version 2)");

    /* Check if any legacy presets exist */
    int migrated_count = 0;
    for (int i = 0; i < MAX_PRESET_SLOTS; i++) {
        size_t sz = sizeof(preset_slot_t);
        esp_err_t err = nvs_get_blob(h, s_nvs_keys[i], &s_slots[i], &sz);
        if (err == ESP_OK && sz == sizeof(preset_slot_t)) {
            if (s_slots[i].name_length > 0 && s_slots[i].name_length <= PRESET_NAME_MAX) {
                ESP_LOGI(TAG, "  Slot %d: preserved existing preset '%.*s'", i,
                         s_slots[i].name_length, s_slots[i].name);
                migrated_count++;
            } else {
                memset(&s_slots[i], 0, sizeof(preset_slot_t));
            }
        } else {
            /* No legacy data, initialize with default name */
            memset(&s_slots[i], 0, sizeof(preset_slot_t));
            char default_name[PRESET_NAME_MAX + 1];
            int len = snprintf(default_name, sizeof(default_name), "Preset %d", i + 1);
            s_slots[i].name_length = (uint8_t)len;
            memcpy(s_slots[i].name, default_name, len);
            ESP_LOGI(TAG, "  Slot %d: initialized with default name '%s'", i, default_name);
        }
    }

    ESP_LOGI(TAG, "Migration complete: %d presets preserved, %d initialized",
             migrated_count, MAX_PRESET_SLOTS - migrated_count);
    return ESP_OK;
}

esp_err_t preset_manager_init(void)
{
    memset(s_slots, 0, sizeof(s_slots));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open NVS: %s", esp_err_to_name(err));
        return err;
    }

    /* Check version flag */
    uint8_t version = 0;
    err = nvs_get_u8(h, NVS_VERSION_KEY, &version);

    if (err == ESP_ERR_NVS_NOT_FOUND || version < PRESET_VERSION_V2) {
        /* Migration needed */
        err = migrate_legacy_presets(h);
        if (err != ESP_OK) {
            nvs_close(h);
            return err;
        }

        /* Set version flag */
        err = nvs_set_u8(h, NVS_VERSION_KEY, PRESET_VERSION_V2);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set version flag: %s", esp_err_to_name(err));
            nvs_close(h);
            return err;
        }
    } else {
        /* Version 2, load slots directly */
        for (int i = 0; i < MAX_PRESET_SLOTS; i++) {
            size_t sz = sizeof(preset_slot_t);
            err = nvs_get_blob(h, s_nvs_keys[i], &s_slots[i], &sz);
            if (err == ESP_OK && sz == sizeof(preset_slot_t)) {
                if (s_slots[i].name_length > 0 && s_slots[i].name_length <= PRESET_NAME_MAX) {
                    ESP_LOGI(TAG, "Loaded slot %d: %.*s", i,
                             s_slots[i].name_length, s_slots[i].name);
                } else {
                    memset(&s_slots[i], 0, sizeof(preset_slot_t));
                }
            }
        }
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Preset manager initialized (version %d)", PRESET_VERSION_V2);
    return ESP_OK;
}

esp_err_t preset_manager_save(uint8_t slot, const char *name)
{
    /* Validate slot range */
    if (slot >= MAX_PRESET_SLOTS) {
        ESP_LOGE(TAG, "Invalid slot %d (must be 0-7)", slot);
        return ESP_ERR_INVALID_ARG;
    }

    /* Get current segment states */
    segment_light_t *states = segment_state_get();
    if (!states) {
        ESP_LOGE(TAG, "Failed to get segment states");
        return ESP_FAIL;
    }

    /* Set name (use provided name or default "Preset N") */
    char preset_name[PRESET_NAME_MAX + 1];
    size_t len;
    if (name && name[0] != '\0') {
        len = strlen(name);
        if (len > PRESET_NAME_MAX) {
            len = PRESET_NAME_MAX;
        }
        memcpy(preset_name, name, len);
    } else {
        len = snprintf(preset_name, sizeof(preset_name), "Preset %d", slot + 1);
    }

    /* Build preset slot structure */
    s_slots[slot].name_length = (uint8_t)len;
    memcpy(s_slots[slot].name, preset_name, len);
    memset(s_slots[slot].name + len, 0, PRESET_NAME_MAX - len); /* Zero-pad unused bytes */
    memcpy(s_slots[slot].segments, states, sizeof(segment_light_t) * MAX_SEGMENTS);

    /* Write to NVS */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(h, s_nvs_keys[slot], &s_slots[slot], sizeof(preset_slot_t));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save preset to slot %d: %s", slot, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Saved preset '%.*s' to slot %d", len, preset_name, slot);
    return ESP_OK;
}

esp_err_t preset_manager_recall(uint8_t slot)
{
    /* Validate slot range */
    if (slot >= MAX_PRESET_SLOTS) {
        ESP_LOGE(TAG, "Invalid slot %d (must be 0-7)", slot);
        return ESP_ERR_INVALID_ARG;
    }

    /* Check if slot is occupied */
    if (s_slots[slot].name_length == 0) {
        ESP_LOGW(TAG, "Slot %d is empty", slot);
        return ESP_ERR_NOT_FOUND;
    }

    /* Get current segment states */
    segment_light_t *states = segment_state_get();
    if (!states) {
        ESP_LOGE(TAG, "Failed to get segment states");
        return ESP_FAIL;
    }

    /* Copy stored states to segment manager (preserve startup_on_off) */
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        uint8_t saved_startup = states[i].startup_on_off;
        memcpy(&states[i], &s_slots[slot].segments[i], sizeof(segment_light_t));
        states[i].startup_on_off = saved_startup;
    }

    ESP_LOGI(TAG, "Recalled preset '%.*s' from slot %d",
             s_slots[slot].name_length, s_slots[slot].name, slot);
    return ESP_OK;
}

esp_err_t preset_manager_delete(uint8_t slot)
{
    /* Validate slot range */
    if (slot >= MAX_PRESET_SLOTS) {
        ESP_LOGE(TAG, "Invalid slot %d (must be 0-7)", slot);
        return ESP_ERR_INVALID_ARG;
    }

    /* Clear slot in memory */
    memset(&s_slots[slot], 0, sizeof(preset_slot_t));

    /* Erase from NVS */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_erase_key(h, s_nvs_keys[slot]);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to delete slot %d: %s", slot, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Deleted preset from slot %d", slot);
    return ESP_OK;
}

esp_err_t preset_manager_get_slot_name(uint8_t slot, char *name_out, size_t max_len)
{
    /* Validate parameters */
    if (slot >= MAX_PRESET_SLOTS) {
        ESP_LOGE(TAG, "Invalid slot %d (must be 0-7)", slot);
        return ESP_ERR_INVALID_ARG;
    }

    if (!name_out || max_len == 0) {
        ESP_LOGE(TAG, "Invalid output buffer");
        return ESP_ERR_INVALID_ARG;
    }

    /* Check if slot is occupied */
    if (s_slots[slot].name_length == 0) {
        name_out[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }

    /* Copy name to output buffer with null terminator */
    size_t copy_len = s_slots[slot].name_length;
    if (copy_len >= max_len) {
        copy_len = max_len - 1;
    }
    memcpy(name_out, s_slots[slot].name, copy_len);
    name_out[copy_len] = '\0';

    return ESP_OK;
}

esp_err_t preset_manager_is_slot_occupied(uint8_t slot, bool *is_occupied)
{
    /* Validate parameters */
    if (slot >= MAX_PRESET_SLOTS) {
        ESP_LOGE(TAG, "Invalid slot %d (must be 0-7)", slot);
        return ESP_ERR_INVALID_ARG;
    }

    if (!is_occupied) {
        ESP_LOGE(TAG, "is_occupied pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    /* Check if name_length > 0 indicates occupied slot */
    *is_occupied = (s_slots[slot].name_length > 0);

    return ESP_OK;
}

void preset_manager_list_presets(void)
{
    printf("=== Preset Slots ===\n");
    for (int i = 0; i < MAX_PRESET_SLOTS; i++) {
        bool occupied = (s_slots[i].name_length > 0);
        if (occupied) {
            printf("  [%d] %.*s (occupied)\n", i,
                   s_slots[i].name_length, s_slots[i].name);
        } else {
            printf("  [%d] (empty)\n", i);
        }
    }
}

/* ================================================================== */
/*  Compatibility functions for Zigbee handlers (deprecated)          */
/*  Kept for backwards compatibility with old Z2M converters          */
/* ================================================================== */

/**
 * @brief Get count of occupied preset slots (compatibility for Zigbee)
 * @return Number of occupied slots (0-8)
 */
int preset_manager_count(void)
{
    int count = 0;
    for (int i = 0; i < MAX_PRESET_SLOTS; i++) {
        if (s_slots[i].name_length > 0) {
            count++;
        }
    }
    return count;
}

/**
 * @brief Stub for active preset (compatibility for Zigbee)
 * @return Empty string (active preset tracking removed in v2)
 */
const char *preset_manager_get_active(void)
{
    static const char empty[] = "";
    return empty;
}

/**
 * @brief Find slot by name (compatibility bridge for Zigbee)
 * @param name Preset name to search for
 * @return Slot index (0-7) or -1 if not found
 */
static int find_slot_by_name(const char *name)
{
    if (!name || name[0] == '\0') return -1;

    size_t len = strlen(name);
    if (len > PRESET_NAME_MAX) return -1;

    for (int i = 0; i < MAX_PRESET_SLOTS; i++) {
        if (s_slots[i].name_length == len &&
            memcmp(s_slots[i].name, name, len) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Recall preset by name (deprecated, kept for backwards compatibility)
 * @param name Preset name
 * @return true on success, false on error
 */
bool preset_manager_recall_by_name(const char *name)
{
    int slot = find_slot_by_name(name);
    if (slot < 0) {
        return false;
    }
    return (preset_manager_recall((uint8_t)slot) == ESP_OK);
}

/**
 * @brief Save preset by name (deprecated, kept for backwards compatibility)
 * @param name Preset name
 * @return true on success, false on error
 */
bool preset_manager_save_by_name(const char *name)
{
    /* Try to find existing slot with this name */
    int slot = find_slot_by_name(name);

    /* If not found, find first empty slot */
    if (slot < 0) {
        for (int i = 0; i < MAX_PRESET_SLOTS; i++) {
            if (s_slots[i].name_length == 0) {
                slot = i;
                break;
            }
        }
    }

    /* All slots full */
    if (slot < 0) {
        return false;
    }

    return (preset_manager_save((uint8_t)slot, name) == ESP_OK);
}

/**
 * @brief Delete preset by name (deprecated, kept for backwards compatibility)
 * @param name Preset name
 * @return true on success, false on error
 */
bool preset_manager_delete_by_name(const char *name)
{
    int slot = find_slot_by_name(name);
    if (slot < 0) {
        return false;
    }
    return (preset_manager_delete((uint8_t)slot) == ESP_OK);
}
