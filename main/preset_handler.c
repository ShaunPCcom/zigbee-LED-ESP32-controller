/**
 * @file preset_handler.c
 * @brief Preset ZCL integration implementation
 *
 * Bridges Zigbee ZCL cluster 0xFC02 to preset_manager module.
 * Handles slot-based and deprecated name-based preset operations.
 */

#include "preset_handler.h"
#include "preset_manager.h"
#include "segment_manager.h"
#include "led_renderer.h"
#include "transition_engine.h"
#include "zigbee_init.h"
#include "esp_zigbee_core.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "preset_handler";

/* Forward declarations for internal helpers */
extern void schedule_zcl_sync(void);
extern uint16_t g_global_transition_ms;

/* Transient storage for save_name (for next save_slot operation) */
static char s_pending_save_name[PRESET_NAME_MAX + 1] = {0};

/* ================================================================== */
/*  Public API Implementation                                          */
/* ================================================================== */

void update_preset_zcl_attrs(void)
{
    uint8_t count = (uint8_t)preset_manager_count();
    esp_zb_zcl_set_attribute_val(ZB_SEGMENT_EP_BASE, ZB_CLUSTER_PRESET_CONFIG,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_PRESET_COUNT, &count, false);

    /* Update active preset (deprecated) */
    const char *active = preset_manager_get_active();
    uint8_t active_buf[17] = {0};
    if (active && active[0] != '\0') {
        size_t len = strlen(active);
        if (len > PRESET_NAME_MAX) len = PRESET_NAME_MAX;
        active_buf[0] = (uint8_t)len;
        memcpy(&active_buf[1], active, len);
    }
    esp_zb_zcl_set_attribute_val(ZB_SEGMENT_EP_BASE, ZB_CLUSTER_PRESET_CONFIG,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_ACTIVE_PRESET, active_buf, false);

    /* Update preset name attributes (slots 0-7) */
    for (int n = 0; n < MAX_PRESET_SLOTS; n++) {
        char name[PRESET_NAME_MAX + 1];
        uint8_t name_buf[17] = {0};
        if (preset_manager_get_slot_name((uint8_t)n, name, sizeof(name)) == ESP_OK) {
            size_t len = strlen(name);
            name_buf[0] = (uint8_t)len;
            memcpy(&name_buf[1], name, len);
        } else {
            /* Empty slot: set default name */
            char default_name[PRESET_NAME_MAX + 1];
            int dlen = snprintf(default_name, sizeof(default_name), "Preset %d", n + 1);
            name_buf[0] = (uint8_t)dlen;
            memcpy(&name_buf[1], default_name, dlen);
        }
        esp_zb_zcl_set_attribute_val(ZB_SEGMENT_EP_BASE, ZB_CLUSTER_PRESET_CONFIG,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_PRESET_NAME_BASE + n, name_buf, false);
    }
}

esp_err_t handle_recall_slot_write(uint8_t slot)
{
    /* Validate slot range */
    if (slot > 7) {
        ESP_LOGE(TAG, "Invalid recall_slot %d (must be 0-7)", slot);
        return ESP_ERR_INVALID_ARG;
    }

    /* Recall preset */
    esp_err_t err = preset_manager_recall(slot);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Recalled preset from slot %d", slot);
        /* Start transitions from current engine values to new preset values */
        segment_light_t *state = segment_state_get();
        for (int i = 0; i < MAX_SEGMENTS; i++) {
            transition_start(&state[i].level_trans, state[i].level, g_global_transition_ms);
            transition_start(&state[i].hue_trans, state[i].hue, 0);  /* Instant - hue wraparound disabled */
            transition_start(&state[i].sat_trans, state[i].saturation, 0);  /* Instant saturation change */
            transition_start(&state[i].ct_trans, state[i].color_temp, g_global_transition_ms);
        }
        schedule_save();
        update_preset_zcl_attrs();
        /* Defer ZCL sync to avoid stack assertion */
        schedule_zcl_sync();
    } else if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Slot %d is empty, cannot recall", slot);
    } else {
        ESP_LOGE(TAG, "Failed to recall slot %d: %s", slot, esp_err_to_name(err));
    }

    /* Clear recall_slot attribute (0xFF = no pending action) */
    uint8_t clear = 0xFF;
    esp_zb_zcl_set_attribute_val(ZB_SEGMENT_EP_BASE, ZB_CLUSTER_PRESET_CONFIG,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_RECALL_SLOT, &clear, false);

    return err;
}

esp_err_t handle_save_slot_write(uint8_t slot)
{
    /* Validate slot range */
    if (slot > 7) {
        ESP_LOGE(TAG, "Invalid save_slot %d (must be 0-7)", slot);
        return ESP_ERR_INVALID_ARG;
    }

    /* Use pending save name if set, otherwise NULL for default name */
    const char *name = (s_pending_save_name[0] != '\0') ? s_pending_save_name : NULL;

    /* Save preset */
    esp_err_t err = preset_manager_save(slot, name);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved preset to slot %d with name '%s'", slot, name ? name : "(default)");
        update_preset_zcl_attrs();
        /* Clear pending save name */
        memset(s_pending_save_name, 0, sizeof(s_pending_save_name));
        uint8_t empty_str[17] = {0};
        esp_zb_zcl_set_attribute_val(ZB_SEGMENT_EP_BASE, ZB_CLUSTER_PRESET_CONFIG,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_SAVE_NAME, empty_str, false);
    } else {
        ESP_LOGE(TAG, "Failed to save slot %d: %s", slot, esp_err_to_name(err));
    }

    /* Clear save_slot attribute (0xFF = no pending action) */
    uint8_t clear = 0xFF;
    esp_zb_zcl_set_attribute_val(ZB_SEGMENT_EP_BASE, ZB_CLUSTER_PRESET_CONFIG,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_SAVE_SLOT, &clear, false);

    return err;
}

esp_err_t handle_delete_slot_write(uint8_t slot)
{
    /* Validate slot range */
    if (slot > 7) {
        ESP_LOGE(TAG, "Invalid delete_slot %d (must be 0-7)", slot);
        return ESP_ERR_INVALID_ARG;
    }

    /* Delete preset */
    esp_err_t err = preset_manager_delete(slot);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Deleted preset from slot %d", slot);
        update_preset_zcl_attrs();
    } else {
        ESP_LOGE(TAG, "Failed to delete slot %d: %s", slot, esp_err_to_name(err));
    }

    /* Clear delete_slot attribute (0xFF = no pending action) */
    uint8_t clear = 0xFF;
    esp_zb_zcl_set_attribute_val(ZB_SEGMENT_EP_BASE, ZB_CLUSTER_PRESET_CONFIG,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_DELETE_SLOT, &clear, false);

    return err;
}

esp_err_t handle_save_name_write(const uint8_t *char_str)
{
    /* Parse CharString: first byte is length, rest is name */
    uint8_t name_len = char_str[0];
    if (name_len > PRESET_NAME_MAX) {
        name_len = PRESET_NAME_MAX;
    }

    /* Store in pending buffer */
    memcpy(s_pending_save_name, &char_str[1], name_len);
    s_pending_save_name[name_len] = '\0';

    ESP_LOGI(TAG, "Stored save_name: '%s' (for next save_slot operation)", s_pending_save_name);
    return ESP_OK;
}

esp_err_t handle_deprecated_preset_write(uint16_t attr_id, const void *value)
{
    /* Handle deprecated name-based attributes (backwards compatibility) */
    uint8_t *char_str = (uint8_t *)value;
    uint8_t name_len = char_str[0];
    if (name_len > PRESET_NAME_MAX) name_len = PRESET_NAME_MAX;
    char name[PRESET_NAME_MAX + 1];
    memcpy(name, &char_str[1], name_len);
    name[name_len] = '\0';

    if (attr_id == ZB_ATTR_RECALL_PRESET) {
        if (preset_manager_recall_by_name(name)) {
            ESP_LOGI(TAG, "Recalled preset '%s' (deprecated API)", name);
            /* Start transitions to new preset values */
            segment_light_t *state = segment_state_get();
            for (int i = 0; i < MAX_SEGMENTS; i++) {
                transition_start(&state[i].level_trans, state[i].level, g_global_transition_ms);
                transition_start(&state[i].hue_trans, state[i].hue, 0);  /* Instant - hue wraparound disabled */
                transition_start(&state[i].sat_trans, state[i].saturation, 0);  /* Instant saturation change */
                transition_start(&state[i].ct_trans, state[i].color_temp, g_global_transition_ms);
            }
            schedule_save();
            update_preset_zcl_attrs();
            /* Defer ZCL sync to avoid stack assertion */
            schedule_zcl_sync();
        } else {
            ESP_LOGW(TAG, "Preset '%s' not found", name);
        }
    } else if (attr_id == ZB_ATTR_SAVE_PRESET) {
        if (preset_manager_save_by_name(name)) {
            ESP_LOGI(TAG, "Saved preset '%s' (deprecated API)", name);
            update_preset_zcl_attrs();
        } else {
            ESP_LOGW(TAG, "Failed to save preset '%s'", name);
        }
    } else if (attr_id == ZB_ATTR_DELETE_PRESET) {
        if (preset_manager_delete_by_name(name)) {
            ESP_LOGI(TAG, "Deleted preset '%s' (deprecated API)", name);
            update_preset_zcl_attrs();
        } else {
            ESP_LOGW(TAG, "Preset '%s' not found", name);
        }
    }
    return ESP_OK;
}
