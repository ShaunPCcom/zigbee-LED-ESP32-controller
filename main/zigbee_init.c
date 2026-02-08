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
#include "esp_log.h"
#include "ha/esp_zigbee_ha_standard.h"

extern uint16_t g_led_count;

static const char *TAG = "zb_init";

/**
 * @brief Create color control attribute list with HS, XY, and CT capabilities
 */
static esp_zb_attribute_list_t *create_color_cluster(void)
{
    esp_zb_attribute_list_t *color = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL);

    uint8_t hue = 0, sat = 0, cmode = 0, ecmode = 0;
    uint16_t ehue = 0, cx = 0x616B, cy = 0x607D;
    uint16_t ctemp = 250, ctemp_min = 153, ctemp_max = 370;

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

    /* Capabilities: HS | EnhancedHue | XY | ColorTemp */
    uint16_t caps = 0x0001 | 0x0002 | 0x0008 | 0x0010;
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
        /* 0xFC00: Device config — LED count */
        esp_zb_attribute_list_t *dev_cfg = esp_zb_zcl_attr_list_create(ZB_CLUSTER_DEVICE_CONFIG);
        uint16_t led_count_val = g_led_count;
        esp_zb_custom_cluster_add_custom_attr(dev_cfg, ZB_ATTR_LED_COUNT,
            ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &led_count_val);
        ESP_ERROR_CHECK(esp_zb_cluster_list_add_custom_cluster(cl, dev_cfg, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

        /* 0xFC01: Segment geometry — start + count for each segment */
        esp_zb_attribute_list_t *seg_cfg = esp_zb_zcl_attr_list_create(ZB_CLUSTER_SEGMENT_CONFIG);
        segment_geom_t *geom = segment_geom_get();
        for (int n = 0; n < MAX_SEGMENTS; n++) {
            uint16_t start_attr = ZB_ATTR_SEG_BASE + (uint16_t)(n * ZB_SEG_ATTRS_PER_SEG + 0);
            uint16_t count_attr = ZB_ATTR_SEG_BASE + (uint16_t)(n * ZB_SEG_ATTRS_PER_SEG + 1);
            esp_zb_custom_cluster_add_custom_attr(seg_cfg, start_attr,
                ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &geom[n].start);
            esp_zb_custom_cluster_add_custom_attr(seg_cfg, count_attr,
                ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &geom[n].count);
        }
        ESP_ERROR_CHECK(esp_zb_cluster_list_add_custom_cluster(cl, seg_cfg, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
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
