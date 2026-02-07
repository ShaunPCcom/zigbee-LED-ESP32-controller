idf_component_register(
    SRCS "main.c"
         "led_driver.c"
         "zigbee_init.c"
         "zigbee_handlers.c"
         "board_led.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer nvs_flash esp-zigbee-lib
)
