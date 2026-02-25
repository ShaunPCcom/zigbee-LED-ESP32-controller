idf_component_register(
    SRCS "main.cpp"
         "led_driver.c"
         "zigbee_init.c"
         "zigbee_signal_handlers.c"
         "zigbee_attr_handler.c"
         "led_renderer.c"
         "color_engine.c"
         "preset_handler.c"
         "config_storage.c"
         "segment_manager.c"
         "preset_manager.c"
         "led_cli.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer nvs_flash esp-zigbee-lib transition_engine board_led zigbee_core
)
