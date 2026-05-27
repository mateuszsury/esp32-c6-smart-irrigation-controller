idf_component_register(
    SRCS "ota_update.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_app_format esp_http_client esp_partition esp_system freertos irrigation_core relay_board
)
