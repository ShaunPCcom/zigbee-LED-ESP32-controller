/**
 * @file zigbee_attr_handler.c
 * @brief Zigbee attribute write dispatch implementation
 *
 * Processes incoming ZCL attribute writes and dispatches to appropriate
 * handlers for segment control, device configuration, and preset operations.
 */

#include "zigbee_attr_handler.h"
#include "preset_handler.h"
#include "led_renderer.h"
#include "segment_manager.h"
#include "config_storage.h"
#include "color_engine.h"
#include "transition_engine.h"
#include "zigbee_init.h"
#include "preset_manager.h"
#include "board_config.h"
#include "zigbee_ota.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "zigbee_attr";

/**
 * @brief Handle ZCL attribute write
 *
 * Dispatches attribute writes to appropriate handlers based on cluster and attribute ID.
 * Handles:
 * - Device config cluster (0xFC00): Strip counts, global transition time
 * - Segment geometry cluster (0xFC01): Segment start/count/strip assignments
 * - Preset config cluster (0xFC02): Preset recall/save/delete operations
 * - Segment endpoints (EP1-EP8): On/off, level, color control attributes
 */
static esp_err_t handle_set_attr_value(const esp_zb_zcl_set_attr_value_message_t *message)
{
    if (!message || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) return ESP_OK;

    uint8_t  endpoint = message->info.dst_endpoint;
    uint16_t cluster  = message->info.cluster;
    uint16_t attr_id  = message->attribute.id;
    void    *value    = message->attribute.data.value;

    ESP_LOGD(TAG, "Attr: EP=%d cluster=0x%04X attr=0x%04X", endpoint, cluster, attr_id);

    /* Custom cluster: device config (EP1 only) */
    if (cluster == ZB_CLUSTER_DEVICE_CONFIG) {
        extern void reboot_cb(uint8_t param);  /* From zigbee_signal_handlers.c */

        if (attr_id == ZB_ATTR_GLOBAL_TRANSITION_MS) {
            uint16_t ms = *(uint16_t *)value;
            led_renderer_set_global_transition_ms(ms);
            config_storage_save_global_transition_ms(ms);
            ESP_LOGI(TAG, "global_transition_ms -> %u ms", ms);
            return ESP_OK;
        }

        /* Strip type (0=SK6812, 1=WS2812B) — requires reboot */
        if (attr_id == ZB_ATTR_STRIP1_TYPE || attr_id == ZB_ATTR_STRIP2_TYPE) {
            uint8_t type = *(uint8_t *)value;
            if (type > 1) {
                ESP_LOGW(TAG, "Invalid strip type %u (0=SK6812, 1=WS2812B)", type);
                return ESP_OK;
            }
            uint8_t strip = (attr_id == ZB_ATTR_STRIP2_TYPE) ? 1 : 0;
            extern uint8_t g_strip_type[2];
            g_strip_type[strip] = type;
            config_storage_save_strip_type(strip, type);
            ESP_LOGI(TAG, "Strip%d type -> %s (saving, reboot in 1s)",
                     strip, type == 1 ? "WS2812B" : "SK6812");
            esp_zb_scheduler_alarm(reboot_cb, 0, 1000);
            return ESP_OK;
        }

        /* Max current (mA, 0=unlimited) — applies immediately */
        if (attr_id == ZB_ATTR_STRIP1_MAX_CURRENT || attr_id == ZB_ATTR_STRIP2_MAX_CURRENT) {
            uint16_t ma = *(uint16_t *)value;
            uint8_t strip = (attr_id == ZB_ATTR_STRIP2_MAX_CURRENT) ? 1 : 0;
            extern uint16_t g_strip_max_current[2];
            g_strip_max_current[strip] = ma;
            config_storage_save_strip_max_current(strip, ma);
            led_renderer_recalc_power_scale();
            ESP_LOGI(TAG, "Strip%d max_current -> %u mA", strip, ma);
            return ESP_OK;
        }

        /* Restart */
        if (attr_id == ZB_ATTR_RESTART) {
            zgb_ctrl_handle_restart();
            return ESP_OK;
        }

        /* Factory reset */
        if (attr_id == ZB_ATTR_FACTORY_RESET) {
            extern void zigbee_full_factory_reset(void);
            zgb_ctrl_handle_factory_reset(*(uint8_t *)value, zigbee_full_factory_reset);
            return ESP_OK;
        }

        /* Strip count (requires reboot) */
        uint16_t new_count = *(uint16_t *)value;
        if (new_count >= 1 && new_count <= 500) {
            uint8_t strip = (attr_id == ZB_ATTR_STRIP2_COUNT) ? 1 : 0;
            ESP_LOGI(TAG, "Strip%d count -> %u (saving, reboot in 1s)", strip, new_count);
            config_storage_save_strip_count(strip, new_count);
            esp_zb_scheduler_alarm(reboot_cb, 0, 1000);
        }
        return ESP_OK;
    }

    /* Custom cluster: segment geometry (EP1 only) */
    if (cluster == ZB_CLUSTER_SEGMENT_CONFIG) {
        if (attr_id < ZB_ATTR_SEG_BASE + MAX_SEGMENTS * ZB_SEG_ATTRS_PER_SEG) {
            int offset  = attr_id - ZB_ATTR_SEG_BASE;
            int seg_idx = offset / ZB_SEG_ATTRS_PER_SEG;
            int field   = offset % ZB_SEG_ATTRS_PER_SEG;
            segment_geom_t *geom = segment_geom_get();
            if (field == 0) {
                geom[seg_idx].start = *(uint16_t *)value;
                ESP_LOGI(TAG, "Seg%d start -> %u", seg_idx + 1, geom[seg_idx].start);
            } else if (field == 1) {
                geom[seg_idx].count = *(uint16_t *)value;
                ESP_LOGI(TAG, "Seg%d count -> %u", seg_idx + 1, geom[seg_idx].count);
            } else {
                uint8_t v = *(uint8_t *)value;
                geom[seg_idx].strip_id = (v >= 2) ? 1 : 0;
                ESP_LOGI(TAG, "Seg%d strip -> %u", seg_idx + 1, geom[seg_idx].strip_id);
            }
            segment_manager_save();
            update_leds();
        }
        return ESP_OK;
    }

    /* Custom cluster: preset configuration (EP1 only) */
    if (cluster == ZB_CLUSTER_PRESET_CONFIG) {
        /* Handle slot-based attributes (Phase 3) */
        if (attr_id == ZB_ATTR_RECALL_SLOT) {
            uint8_t slot = *(uint8_t *)value;
            handle_recall_slot_write(slot);
            return ESP_OK;
        } else if (attr_id == ZB_ATTR_SAVE_SLOT) {
            uint8_t slot = *(uint8_t *)value;
            handle_save_slot_write(slot);
            return ESP_OK;
        } else if (attr_id == ZB_ATTR_DELETE_SLOT) {
            uint8_t slot = *(uint8_t *)value;
            handle_delete_slot_write(slot);
            return ESP_OK;
        } else if (attr_id == ZB_ATTR_SAVE_NAME) {
            uint8_t *char_str = (uint8_t *)value;
            handle_save_name_write(char_str);
            return ESP_OK;
        }

        /* Handle deprecated name-based attributes (backwards compatibility) */
        handle_deprecated_preset_write(attr_id, value);
        return ESP_OK;
    }

    /* EP9 "all segments" master — on/off, level, and CT writes propagate to all segments.
     * HS color is handled by polling in led_render_cb (SDK delivers no callback for it). */
    if (endpoint == ZB_ALL_EP) {
        segment_light_t *state = segment_state_get();

        if (cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
                bool new_on = *(bool *)value;
                ESP_LOGI(TAG, "All segs on/off -> %s", new_on ? "ON" : "OFF");
                for (int i = 0; i < MAX_SEGMENTS; i++) {
                    bool was_on = state[i].on;
                    state[i].on = new_on;
                    if (new_on && !was_on) {
                        transition_start(&state[i].level_trans, 0, 0);
                        transition_start(&state[i].level_trans, state[i].level,
                                         led_renderer_get_global_transition_ms());
                    } else if (!new_on && was_on) {
                        transition_start(&state[i].level_trans, 0,
                                         led_renderer_get_global_transition_ms());
                    }
                }
                update_leds();
                schedule_save();
            }
        } else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL) {
            if (attr_id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID) {
                uint8_t new_level = *(uint8_t *)value;
                ESP_LOGI(TAG, "All segs level -> %d", new_level);
                for (int i = 0; i < MAX_SEGMENTS; i++) {
                    state[i].level = new_level;
                    transition_start(&state[i].level_trans, new_level,
                                     led_renderer_get_global_transition_ms());
                    /* Sync segment EP ZCL stores so the render loop level poll
                     * doesn't revert the level back to the old value next tick. */
                    uint8_t ep_i = (uint8_t)(ZB_SEGMENT_EP_BASE + i);
                    esp_zb_zcl_set_attribute_val(ep_i, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
                        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                        ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID, &new_level, false);
                }
                update_leds();
                schedule_save();
            }
        } else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL) {
            switch (attr_id) {
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID: {
                uint16_t enh_hue = *(uint16_t *)value;
                uint16_t hue = (uint16_t)((uint32_t)enh_hue * 360 / 65535);
                for (int i = 0; i < MAX_SEGMENTS; i++) {
                    state[i].hue = hue;
                    state[i].color_mode = 0;
                    start_hue_transition(&state[i].hue_trans, hue,
                                         led_renderer_get_global_transition_ms());
                }
                break;
            }
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID: {
                uint8_t new_sat = *(uint8_t *)value;
                for (int i = 0; i < MAX_SEGMENTS; i++) {
                    state[i].saturation = new_sat;
                    transition_start(&state[i].sat_trans, new_sat,
                                     led_renderer_get_global_transition_ms());
                }
                break;
            }
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID: {
                uint16_t new_ct = *(uint16_t *)value;
                ESP_LOGI(TAG, "All segs CT -> %u mireds", new_ct);
                uint8_t mode2 = 2;
                for (int i = 0; i < MAX_SEGMENTS; i++) {
                    state[i].color_temp = new_ct;
                    state[i].color_mode = 2;
                    transition_start(&state[i].ct_trans, new_ct,
                                     led_renderer_get_global_transition_ms());
                    /* Sync segment EP ZCL stores so the render loop's unconditional
                     * color_mode read doesn't revert the mode back to HS next tick. */
                    uint8_t ep_i = (uint8_t)(ZB_SEGMENT_EP_BASE + i);
                    esp_zb_zcl_set_attribute_val(ep_i, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                        ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID, &mode2, false);
                    esp_zb_zcl_set_attribute_val(ep_i, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                        ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID, &new_ct, false);
                }
                update_leds();
                schedule_save();
                break;
            }
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID: {
                uint8_t new_mode = *(uint8_t *)value;
                for (int i = 0; i < MAX_SEGMENTS; i++) {
                    state[i].color_mode = new_mode;
                    uint8_t ep_i = (uint8_t)(ZB_SEGMENT_EP_BASE + i);
                    esp_zb_zcl_set_attribute_val(ep_i, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                        ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID, &new_mode, false);
                }
                update_leds();
                break;
            }
            default:
                break;
            }
        }
        return ESP_OK;
    }

    /* Segment light endpoints (EP1-EP8) */
    if (endpoint >= ZB_SEGMENT_EP_BASE && endpoint < ZB_SEGMENT_EP_BASE + MAX_SEGMENTS) {
        int seg = endpoint - ZB_SEGMENT_EP_BASE;
        segment_light_t *state = segment_state_get();
        bool needs_update = false;

        if (cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
                bool new_on = *(bool *)value;
                bool was_on = state[seg].on;
                state[seg].on = new_on;
                ESP_LOGI(TAG, "Seg%d on/off -> %s", seg + 1, state[seg].on ? "ON" : "OFF");

                if (new_on && !was_on) {
                    /* Turning ON: Start from 0 (dark) and fade to target level */
                    transition_start(&state[seg].level_trans, 0, 0);  /* Instant to 0 */
                    transition_start(&state[seg].level_trans, state[seg].level, led_renderer_get_global_transition_ms());
                } else if (!new_on && was_on) {
                    /* Turning OFF: Fade from current level to 0 */
                    transition_start(&state[seg].level_trans, 0, led_renderer_get_global_transition_ms());
                }
                needs_update = true;
            } else if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_START_UP_ON_OFF) {
                state[seg].startup_on_off = *(uint8_t *)value;
                ESP_LOGI(TAG, "Seg%d startup_on_off -> 0x%02X", seg + 1, state[seg].startup_on_off);
                schedule_save();
            }
        } else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL) {
            if (attr_id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID) {
                state[seg].level = *(uint8_t *)value;
                ESP_LOGI(TAG, "Seg%d level -> %d", seg + 1, state[seg].level);
                /* Start transition to new level with global duration */
                transition_start(&state[seg].level_trans, state[seg].level, led_renderer_get_global_transition_ms());
                needs_update = true;
            }
        } else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL) {
            switch (attr_id) {
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID: {
                uint16_t enh_hue = *(uint16_t *)value;
                state[seg].hue = (uint16_t)((uint32_t)enh_hue * 360 / 65535);
                state[seg].color_mode = 0;
                /* Smooth hue transition with shortest-arc calculation */
                start_hue_transition(&state[seg].hue_trans, state[seg].hue, led_renderer_get_global_transition_ms());
                break;
            }
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID:
                state[seg].saturation = *(uint8_t *)value;
                /* Smooth saturation transition */
                transition_start(&state[seg].sat_trans, state[seg].saturation, led_renderer_get_global_transition_ms());
                break;
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID:
                state[seg].color_temp = *(uint16_t *)value;
                state[seg].color_mode = 2;
                ESP_LOGI(TAG, "Seg%d CT -> %u mireds", seg + 1, state[seg].color_temp);
                transition_start(&state[seg].ct_trans, state[seg].color_temp, led_renderer_get_global_transition_ms());
                needs_update = true;
                break;
            case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID:
                state[seg].color_mode = *(uint8_t *)value;
                ESP_LOGI(TAG, "Seg%d color_mode -> %d", seg + 1, state[seg].color_mode);
                needs_update = true;
                break;
            default:
                break;
            }
        }

        if (needs_update) { update_leds(); schedule_save(); }
    }

    return ESP_OK;
}

/* ================================================================== */
/*  Public action handler                                              */
/* ================================================================== */

esp_err_t zigbee_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    /* Route OTA callbacks to OTA component */
    esp_err_t ota_ret = zigbee_ota_action_handler(callback_id, message);
    if (ota_ret != ESP_ERR_NOT_SUPPORTED) {
        return ota_ret;  /* OTA component handled it */
    }

    esp_err_t ret = ESP_OK;
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        ret = handle_set_attr_value((const esp_zb_zcl_set_attr_value_message_t *)message);
        break;
    default:
        ESP_LOGD(TAG, "Unhandled callback: 0x%x", callback_id);
        break;
    }
    return ret;
}
