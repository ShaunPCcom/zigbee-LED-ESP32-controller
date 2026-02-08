/**
 * @file zigbee_init.c
 * @brief Zigbee stack initialization for LED controller
 *
 * Creates a Zigbee Router device as a Color Dimmable Light
 */

#include "zigbee_init.h"
#include "zigbee_handlers.h"
#include "esp_log.h"
#include "esp_check.h"
#include "ha/esp_zigbee_ha_standard.h"

extern uint16_t g_led_count;

static const char *TAG = "zb_init";

/**
 * @brief Create cluster list for LED endpoint
 */
static esp_zb_cluster_list_t *create_led_clusters(void)
{
    /* Basic cluster - device information */
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE,
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);

    /* Add manufacturer details */
    uint8_t manufacturer[] = {3, 'D', 'I', 'Y'};  // ZCL string: length + data
    uint8_t model[] = {11, 'Z', 'B', '_', 'L', 'E', 'D', '_', 'C', 'T', 'R', 'L'};

    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, manufacturer);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model);

    /* Identify cluster - for device identification */
    esp_zb_identify_cluster_cfg_t identify_cfg = {
        .identify_time = ESP_ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *identify = esp_zb_identify_cluster_create(&identify_cfg);

    /* On/Off cluster */
    esp_zb_on_off_cluster_cfg_t on_off_cfg = {
        .on_off = ESP_ZB_ZCL_ON_OFF_ON_OFF_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *on_off = esp_zb_on_off_cluster_create(&on_off_cfg);

    /* Level Control cluster (brightness) */
    esp_zb_level_cluster_cfg_t level_cfg = {
        .current_level = 128,  // 50% brightness
    };
    esp_zb_attribute_list_t *level = esp_zb_level_cluster_create(&level_cfg);

    /* Color Control cluster - create manually with all required attributes */
    esp_zb_attribute_list_t *color = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL);

    // Current hue and saturation
    uint8_t current_hue = 0;
    uint8_t current_saturation = 0;
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID, &current_hue);
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID, &current_saturation);

    // Enhanced hue (16-bit version)
    uint16_t enhanced_hue = 0;
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID, &enhanced_hue);

    // Current X and Y coordinates (for CIE color space)
    uint16_t current_x = 0x616B;  // Default white
    uint16_t current_y = 0x607D;
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID, &current_x);
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID, &current_y);

    // Color temperature
    uint16_t color_temp = 250;  // ~4000K (neutral white)
    uint16_t color_temp_min = 153;  // 6500K (cold white)
    uint16_t color_temp_max = 370;  // 2700K (warm white)
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID, &color_temp);
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID, &color_temp_min);
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID, &color_temp_max);

    // Color mode and enhanced color mode — start in HS so Z2M presents the hue wheel
    uint8_t color_mode = 0;  // 0=HS, 1=XY, 2=ColorTemp
    uint8_t enhanced_color_mode = 0;
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID, &color_mode);
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_COLOR_MODE_ID, &enhanced_color_mode);

    // Color capabilities bitmask — bit 1 (Enhanced Hue) enables EnhancedMoveToHue commands
    uint16_t color_capabilities = 0x0001 | 0x0002 | 0x0008 | 0x0010;  // HS | EnhHue | XY | ColorTemp
    esp_zb_color_control_cluster_add_attr(color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_CAPABILITIES_ID, &color_capabilities);

    /* Custom configuration cluster 0xFC00 — LED count */
    esp_zb_attribute_list_t *custom_cfg = esp_zb_zcl_attr_list_create(ZB_CLUSTER_DEVICE_CONFIG);
    uint16_t led_count_val = g_led_count;
    esp_zb_custom_cluster_add_custom_attr(custom_cfg, ZB_ATTR_LED_COUNT,
        ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &led_count_val);

    /* Create cluster list and add all clusters */
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();

    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(cluster_list, basic,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(cluster_list, identify,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_on_off_cluster(cluster_list, on_off,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_level_cluster(cluster_list, level,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_color_control_cluster(cluster_list, color,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_custom_cluster(cluster_list, custom_cfg,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    return cluster_list;
}

/**
 * @brief Create cluster list for white channel endpoint (dimmable only)
 */
static esp_zb_cluster_list_t *create_white_clusters(void)
{
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE,
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);

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

    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(cluster_list, basic,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(cluster_list, identify,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_on_off_cluster(cluster_list, on_off,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_level_cluster(cluster_list, level,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    return cluster_list;
}

/**
 * @brief Register Zigbee endpoints
 */
static void zigbee_register_endpoints(void)
{
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();

    /* Endpoint 1: RGB strip (Color Dimmable Light) */
    esp_zb_endpoint_config_t ep1_cfg = {
        .endpoint = ZB_LED_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_COLOR_DIMMABLE_LIGHT_DEVICE_ID,
        .app_device_version = 0,
    };
    ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(ep_list, create_led_clusters(), ep1_cfg));

    /* Endpoint 2: White channel (Dimmable Light, no color) */
    esp_zb_endpoint_config_t ep2_cfg = {
        .endpoint = ZB_WHITE_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_DIMMABLE_LIGHT_DEVICE_ID,
        .app_device_version = 0,
    };
    ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(ep_list, create_white_clusters(), ep2_cfg));

    ESP_ERROR_CHECK(esp_zb_device_register(ep_list));

    ESP_LOGI(TAG, "Registered EP%d as Color Dimmable Light (RGB)", ZB_LED_ENDPOINT);
    ESP_LOGI(TAG, "Registered EP%d as Dimmable Light (White channel)", ZB_WHITE_ENDPOINT);
}

/**
 * @brief Zigbee stack main task
 */
static void zigbee_task(void *pvParameters)
{
    (void)pvParameters;

    /* Configure Zigbee platform (radio settings) */
    esp_zb_platform_config_t platform_cfg = {
        .radio_config.radio_mode = ZB_RADIO_MODE_NATIVE,
        .host_config.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform_cfg));

    /* Initialize Zigbee stack as Router */
    esp_zb_cfg_t zb_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg = {
            .max_children = 10,
        },
    };
    esp_zb_init(&zb_cfg);

    /* Register action handler before endpoint registration */
    esp_zb_core_action_handler_register(zigbee_action_handler);

    /* Register LED endpoint */
    zigbee_register_endpoints();

    /* Start Zigbee stack */
    ESP_ERROR_CHECK(esp_zb_start(false));

    /* Main loop - never returns */
    esp_zb_stack_main_loop();
}

esp_err_t zigbee_init(void)
{
    ESP_LOGI(TAG, "Initializing Zigbee stack as Router");

    /* Create Zigbee task */
    BaseType_t ret = xTaskCreate(zigbee_task, "zb_main", 8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Zigbee task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t zigbee_start(void)
{
    /* Network steering is handled automatically in signal handler */
    return ESP_OK;
}
