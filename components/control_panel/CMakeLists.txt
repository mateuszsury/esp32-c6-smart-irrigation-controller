idf_component_register(
    SRCS "control_panel.c"
    INCLUDE_DIRS "include"
    REQUIRES config_store esp_event esp_http_server esp_netif esp_wifi irrigation_core json nvs_flash ota_update
)
