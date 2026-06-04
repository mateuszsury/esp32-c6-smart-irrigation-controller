idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board_config config_store control_panel diag esp_coex irrigation_core mqtt_mode ota_update relay_board scheduler status_led zigbee_mode
)
