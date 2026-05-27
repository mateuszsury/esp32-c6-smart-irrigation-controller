idf_component_register(
    SRCS "config_store.c"
    INCLUDE_DIRS "include"
    REQUIRES irrigation_core nvs_flash
)
