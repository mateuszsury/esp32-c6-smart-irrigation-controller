idf_component_register(
    SRCS "mqtt_mode.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_hw_support esp_wifi mqtt irrigation_core ota_update
)
