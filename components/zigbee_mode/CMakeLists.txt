idf_component_register(
    SRCS "zigbee_mode.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp-zigbee-lib irrigation_core nvs_flash
)
