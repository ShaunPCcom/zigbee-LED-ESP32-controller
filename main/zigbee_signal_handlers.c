/**
 * @file zigbee_signal_handlers.c
 * @brief LED-specific Zigbee lifecycle callbacks.
 *
 * Registers project-specific hooks with the shared zigbee_signal_handler
 * (from zigbee_core). All common network lifecycle logic lives there.
 */

#include "zigbee_signal_handler.h"
#include "zigbee_init.h"
#include "board_config.h"
#include "led_renderer.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"

#define DIAG_REPORT_MIN_INTERVAL   0    /* report immediately on change */
#define DIAG_REPORT_MAX_INTERVAL   300  /* 5-minute keepalive */

static const char *TAG = "zb_led_hooks";

/* LED driver functions */
extern void led_driver_clear(uint8_t strip);
extern void led_driver_refresh(void);

/* ================================================================== */
/*  Diagnostic reporting (LED-specific cluster/attr IDs)              */
/* ================================================================== */

static void configure_diag_report(uint16_t attr_id, uint16_t max_interval)
{
    esp_zb_zcl_reporting_info_t rpt = {0};
    rpt.direction    = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
    rpt.ep           = ZB_SEGMENT_EP_BASE;
    rpt.cluster_id   = ZB_CLUSTER_DEVICE_CONFIG;
    rpt.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE;
    rpt.attr_id      = attr_id;
    rpt.u.send_info.min_interval     = DIAG_REPORT_MIN_INTERVAL;
    rpt.u.send_info.max_interval     = max_interval;
    rpt.u.send_info.def_min_interval = DIAG_REPORT_MIN_INTERVAL;
    rpt.u.send_info.def_max_interval = max_interval;
    rpt.u.send_info.delta.u32        = 0;
    rpt.dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    rpt.manuf_code     = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
    esp_zb_zcl_update_reporting_info(&rpt);
}

static void configure_diag_reporting(void)
{
    configure_diag_report(ZB_ATTR_BOOT_COUNT,      DIAG_REPORT_MAX_INTERVAL);
    configure_diag_report(ZB_ATTR_RESET_REASON,    DIAG_REPORT_MAX_INTERVAL);
    configure_diag_report(ZB_ATTR_LAST_UPTIME_SEC, DIAG_REPORT_MAX_INTERVAL);
    configure_diag_report(ZB_ATTR_MIN_FREE_HEAP,   DIAG_REPORT_MAX_INTERVAL);
    ESP_LOGI(TAG, "Crash diag reporting configured");
}

/* ================================================================== */
/*  Project lifecycle callbacks                                        */
/* ================================================================== */

static void led_on_stack_init(void)
{
    sync_zcl_from_state();
    led_renderer_start();
}

static void led_on_joined(void)
{
    configure_diag_reporting();
    esp_zb_scheduler_alarm(restore_leds_cb, 0, 5500);
}

static void led_on_left(void)
{
    led_driver_clear(0);
    led_driver_clear(1);
    led_driver_refresh();
}

/* ================================================================== */
/*  Registration                                                       */
/* ================================================================== */

void zigbee_signal_handlers_setup(void)
{
    static const zigbee_signal_hooks_t hooks = {
        .on_stack_init = led_on_stack_init,
        .on_joined     = led_on_joined,
        .on_left       = led_on_left,
        .nvs_namespace = "led_cfg",
    };
    zigbee_signal_handler_register(&hooks);
}
