set(MAIN_SRCS
    "main.cpp"
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
)

if("${IDF_TARGET}" STREQUAL "esp32c6")
    list(APPEND MAIN_SRCS "web_server.c" "config_api.c")
    set(C6_PRIV_REQUIRES web_server_base wifi_manager ota_check)
else()
    set(C6_PRIV_REQUIRES "")
endif()

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${C6_PRIV_REQUIRES}
    REQUIRES driver esp_timer nvs_flash esp-zigbee-lib transition_engine board_led zigbee_core crash_diag
)

if("${IDF_TARGET}" STREQUAL "esp32c6")
    target_add_binary_data(${COMPONENT_LIB} "../web/index.html" TEXT)
    target_add_binary_data(${COMPONENT_LIB} "../web/app.js"     TEXT)
    target_add_binary_data(${COMPONENT_LIB} "../web/style.css"  TEXT)
endif()
