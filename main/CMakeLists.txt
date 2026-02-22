idf_component_register(
    SRCS "main.cpp"
         "led_driver.c"
         "zigbee_init.c"
         "zigbee_handlers.c"
         "config_storage.c"
         "segment_manager.c"
         "preset_manager.c"
         "led_cli.c"
         "color_conversion.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer nvs_flash esp-zigbee-lib transition_engine board_led zigbee_core
)
