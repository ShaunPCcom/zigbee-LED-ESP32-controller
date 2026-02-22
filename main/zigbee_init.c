/**
 * @file zigbee_init.c
 * @brief Zigbee stack initialization for LED controller
 *
 * Creates a Zigbee Router with 8 Extended Color Light endpoints (EP1-EP8).
 * Each endpoint represents one virtual segment of the LED strip.
 * Color mode HS/XY → RGB channels; CT mode → White channel.
 * EP1 also hosts custom clusters for device config (0xFC00) and
 * segment geometry (0xFC01: start+count per segment).
 */

#include "zigbee_init.h"
#include "zigbee_handlers.h"
#include "board_config.h"
#include "segment_manager.h"
#include "preset_manager.h"
#include "version.h"
#include "esp_log.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zigbee_ota.h"
#include <string.h>

extern uint16_t g_strip_count[2];

static const char *TAG = "zb_init";

/* Static buffer for device config cluster attributes */
static uint16_t s_global_transition_ms_attr = 1000;  /* default 1000ms */

/* Static buffers for preset cluster attributes */
static uint8_t s_preset_count_attr = 0;
static uint8_t s_active_preset_attr[17] = {0};       /* DEPRECATED */
static uint8_t s_recall_preset_attr[17] = {0};       /* DEPRECATED */
static uint8_t s_save_preset_attr[17] = {0};         /* DEPRECATED */
static uint8_t s_delete_preset_attr[17] = {0};       /* DEPRECATED */
static uint8_t s_preset_name_attrs[MAX_PRESET_SLOTS][17] = {{0}};
static uint8_t s_recall_slot_attr = 0xFF;            /* 0xFF = no pending action */
static uint8_t s_save_slot_attr = 0xFF;              /* 0xFF = no pending action */
static uint8_t s_delete_slot_attr = 0xFF;            /* 0xFF = no pending action */
static uint8_t s_save_name_attr[17] = {0};           /* CharString for next save */

/**
 * @brief Create color control attribute list with HS, XY, and CT capabilities
 */
static esp_zb_attribute_list_t *create_color_cluster(void)
{
    esp_zb_attribute_list_t *color = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL);

    uint8_t hue = 0, sat = 0, cmode = 0, ecmode = 0;
    uint16_t ehue = 0, cx = 0x616B, cy = 0x607D;
    uint16_t ctemp = 250, ctemp_min = 153, ctemp_max = 370;

    /* Hue/saturation attributes exist for SDK compatibility but are not used (XY mode only) */
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID, &hue);
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID, &sat);
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID, &ehue);
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID, &cx);
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID, &cy);
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID, &ctemp);
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID, &ctemp_min);
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID, &ctemp_max);
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID, &cmode);
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_COLOR_MODE_ID, &ecmode);

    /* Capabilities: Enhanced Hue | ColorTemp (basic HS disabled - use 16-bit enhanced hue only) */
    uint16_t caps = 0x0002 | 0x0010;
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_CAPABILITIES_ID, &caps);

    return color;
}

/**
 * @brief Create cluster list for a segment endpoint
 *
 * @param seg_idx  Segment index 0-7. Index 0 (EP1) gets the custom config clusters.
 */
static esp_zb_cluster_list_t *create_segment_clusters(int seg_idx)
{
    segment_light_t *state = segment_state_get();
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE,
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);

    /* Add manufacturer/model only on the primary endpoint */
    if (seg_idx == 0) {
        uint8_t manufacturer[] = {3, 'D', 'I', 'Y'};
        uint8_t model[] = {11, 'Z', 'B', '_', 'L', 'E', 'D', '_', 'C', 'T', 'R', 'L'};
        esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, manufacturer);
        esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model);
    }

    esp_zb_identify_cluster_cfg_t identify_cfg = {
        .identify_time = ESP_ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *identify = esp_zb_identify_cluster_create(&identify_cfg);

    esp_zb_on_off_cluster_cfg_t on_off_cfg = {
        .on_off = ESP_ZB_ZCL_ON_OFF_ON_OFF_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *on_off = esp_zb_on_off_cluster_create(&on_off_cfg);
    /* StartUpOnOff: power-on behavior (0=off, 1=on, 2=toggle, DEFAULT_STARTUP_ON_OFF=previous) */
    uint8_t startup_val = state[seg_idx].startup_on_off;
    esp_zb_on_off_cluster_add_attr(on_off, ESP_ZB_ZCL_ATTR_ON_OFF_START_UP_ON_OFF, &startup_val);

    esp_zb_level_cluster_cfg_t level_cfg = {
        .current_level = 128,
    };
    esp_zb_attribute_list_t *level = esp_zb_level_cluster_create(&level_cfg);

    esp_zb_attribute_list_t *color = create_color_cluster();

    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(cl, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(cl, identify, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_on_off_cluster(cl, on_off, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_level_cluster(cl, level, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_color_control_cluster(cl, color, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    /* Custom clusters on EP1 only */
    if (seg_idx == 0) {
        /* 0xFC00: Device config — per-strip LED counts and global transition time */
        esp_zb_attribute_list_t *dev_cfg = esp_zb_zcl_attr_list_create(ZB_CLUSTER_DEVICE_CONFIG);
        uint16_t s0 = g_strip_count[0], s1 = g_strip_count[1];
        s_global_transition_ms_attr = zigbee_handlers_get_global_transition_ms();
        esp_zb_custom_cluster_add_custom_attr(dev_cfg, ZB_ATTR_LED_COUNT,
            ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s0);
        esp_zb_custom_cluster_add_custom_attr(dev_cfg, ZB_ATTR_STRIP1_COUNT,
            ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s0);
        esp_zb_custom_cluster_add_custom_attr(dev_cfg, ZB_ATTR_STRIP2_COUNT,
            ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s1);
        esp_zb_custom_cluster_add_custom_attr(dev_cfg, ZB_ATTR_GLOBAL_TRANSITION_MS,
            ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_global_transition_ms_attr);
        ESP_ERROR_CHECK(esp_zb_cluster_list_add_custom_cluster(cl, dev_cfg, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

        /* 0xFC01: Segment geometry — start + count + strip for each segment */
        esp_zb_attribute_list_t *seg_cfg = esp_zb_zcl_attr_list_create(ZB_CLUSTER_SEGMENT_CONFIG);
        segment_geom_t *geom = segment_geom_get();
        for (int n = 0; n < MAX_SEGMENTS; n++) {
            uint16_t base = ZB_ATTR_SEG_BASE + (uint16_t)(n * ZB_SEG_ATTRS_PER_SEG);
            uint8_t zcl_strip = (uint8_t)(geom[n].strip_id + 1);  /* 1-indexed for ZCL */
            esp_zb_custom_cluster_add_custom_attr(seg_cfg, base + 0,
                ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &geom[n].start);
            esp_zb_custom_cluster_add_custom_attr(seg_cfg, base + 1,
                ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &geom[n].count);
            esp_zb_custom_cluster_add_custom_attr(seg_cfg, base + 2,
                ESP_ZB_ZCL_ATTR_TYPE_U8, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &zcl_strip);
        }
        ESP_ERROR_CHECK(esp_zb_cluster_list_add_custom_cluster(cl, seg_cfg, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

        /* 0xFC02: Preset configuration — save/recall segment states */
        esp_zb_attribute_list_t *preset_cfg = esp_zb_zcl_attr_list_create(ZB_CLUSTER_PRESET_CONFIG);

        /* Initialize preset count and names from preset manager */
        s_preset_count_attr = (uint8_t)preset_manager_count();
        for (int n = 0; n < MAX_PRESET_SLOTS; n++) {
            char name[PRESET_NAME_MAX + 1];
            if (preset_manager_get_slot_name((uint8_t)n, name, sizeof(name)) == ESP_OK) {
                size_t len = strlen(name);
                s_preset_name_attrs[n][0] = (uint8_t)len;
                memcpy(&s_preset_name_attrs[n][1], name, len);
            } else {
                /* Empty slot: set default name */
                char default_name[PRESET_NAME_MAX + 1];
                int dlen = snprintf(default_name, sizeof(default_name), "Preset %d", n + 1);
                s_preset_name_attrs[n][0] = (uint8_t)dlen;
                memcpy(&s_preset_name_attrs[n][1], default_name, dlen);
            }
        }

        /* Initialize active preset (deprecated) */
        const char *active = preset_manager_get_active();
        if (active && active[0] != '\0') {
            size_t len = strlen(active);
            s_active_preset_attr[0] = (uint8_t)len;
            memcpy(&s_active_preset_attr[1], active, len);
        }

        /* Add attributes: deprecated name-based attrs for backwards compatibility */
        esp_zb_custom_cluster_add_custom_attr(preset_cfg, ZB_ATTR_PRESET_COUNT,
            ESP_ZB_ZCL_ATTR_TYPE_U8, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, &s_preset_count_attr);
        esp_zb_custom_cluster_add_custom_attr(preset_cfg, ZB_ATTR_ACTIVE_PRESET,
            ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, s_active_preset_attr);
        esp_zb_custom_cluster_add_custom_attr(preset_cfg, ZB_ATTR_RECALL_PRESET,
            ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING, ESP_ZB_ZCL_ATTR_ACCESS_WRITE_ONLY, s_recall_preset_attr);
        esp_zb_custom_cluster_add_custom_attr(preset_cfg, ZB_ATTR_SAVE_PRESET,
            ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING, ESP_ZB_ZCL_ATTR_ACCESS_WRITE_ONLY, s_save_preset_attr);
        esp_zb_custom_cluster_add_custom_attr(preset_cfg, ZB_ATTR_DELETE_PRESET,
            ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING, ESP_ZB_ZCL_ATTR_ACCESS_WRITE_ONLY, s_delete_preset_attr);

        /* Add preset name attributes (read-only, slots 0-7) */
        for (int n = 0; n < MAX_PRESET_SLOTS; n++) {
            esp_zb_custom_cluster_add_custom_attr(preset_cfg, ZB_ATTR_PRESET_NAME_BASE + n,
                ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, s_preset_name_attrs[n]);
        }

        /* Add new slot-based attributes (Phase 3) */
        esp_zb_custom_cluster_add_custom_attr(preset_cfg, ZB_ATTR_RECALL_SLOT,
            ESP_ZB_ZCL_ATTR_TYPE_U8, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_recall_slot_attr);
        esp_zb_custom_cluster_add_custom_attr(preset_cfg, ZB_ATTR_SAVE_SLOT,
            ESP_ZB_ZCL_ATTR_TYPE_U8, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_save_slot_attr);
        esp_zb_custom_cluster_add_custom_attr(preset_cfg, ZB_ATTR_DELETE_SLOT,
            ESP_ZB_ZCL_ATTR_TYPE_U8, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_delete_slot_attr);
        esp_zb_custom_cluster_add_custom_attr(preset_cfg, ZB_ATTR_SAVE_NAME,
            ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, s_save_name_attr);

        ESP_ERROR_CHECK(esp_zb_cluster_list_add_custom_cluster(cl, preset_cfg, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

        /* Add OTA cluster to primary endpoint (EP1) */
        zigbee_ota_config_t ota_cfg = ZIGBEE_OTA_CONFIG_DEFAULT();
        ota_cfg.manufacturer_code = 0x131B;  /* Espressif */
        ota_cfg.image_type = 0x0002;         /* LED Controller (different from LD2450) */
        ota_cfg.current_file_version = FIRMWARE_VERSION;
        ota_cfg.hw_version = 1;
        ota_cfg.query_interval_minutes = 1440;  /* Check every 24 hours */
        ESP_ERROR_CHECK(zigbee_ota_init(cl, ZB_SEGMENT_EP_BASE, &ota_cfg));
        ESP_LOGI(TAG, "OTA cluster initialized on EP%d (%s)", ZB_SEGMENT_EP_BASE, FIRMWARE_VERSION_STRING);
    }

    return cl;
}

/**
 * @brief Register Zigbee endpoints (EP1-EP8, one per segment)
 */
static void zigbee_register_endpoints(void)
{
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();

    for (int i = 0; i < MAX_SEGMENTS; i++) {
        esp_zb_endpoint_config_t ep_cfg = {
            .endpoint = (uint8_t)(ZB_SEGMENT_EP_BASE + i),
            .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
            .app_device_id = 0x0210,  /* Extended Color Light (HS + XY + CT) */
            .app_device_version = 0,
        };
        ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(ep_list, create_segment_clusters(i), ep_cfg));
    }

    ESP_ERROR_CHECK(esp_zb_device_register(ep_list));

    ESP_LOGI(TAG, "Registered EP%d-%d as Extended Color Light (segments 1-%d)",
             ZB_SEGMENT_EP_BASE, ZB_SEGMENT_EP_BASE + MAX_SEGMENTS - 1, MAX_SEGMENTS);
}

/**
 * @brief Zigbee stack main task
 */
static void zigbee_task(void *pvParameters)
{
    (void)pvParameters;

    esp_zb_platform_config_t platform_cfg = {
        .radio_config.radio_mode = ZB_RADIO_MODE_NATIVE,
        .host_config.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform_cfg));

    esp_zb_cfg_t zb_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg = {
            .max_children = 10,
        },
    };
    esp_zb_init(&zb_cfg);

    esp_zb_core_action_handler_register(zigbee_action_handler);
    zigbee_register_endpoints();

    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

esp_err_t zigbee_init(void)
{
    ESP_LOGI(TAG, "Initializing Zigbee stack as Router");

    BaseType_t ret = xTaskCreate(zigbee_task, "zb_main", 8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Zigbee task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t zigbee_start(void)
{
    return ESP_OK;
}
