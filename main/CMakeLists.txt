idf_component_register(
    SRCS "main.c"
         "led_driver.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer nvs_flash
)
