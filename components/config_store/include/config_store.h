#pragma once

#include "esp_err.h"
#include "irrigation_core.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t config_store_init(void);
esp_err_t config_store_load(irrigation_config_t *config);
esp_err_t config_store_save(const irrigation_config_t *config);
esp_err_t config_store_factory_reset(void);
esp_err_t config_store_save_mode(irrigation_comm_mode_t mode);

#ifdef __cplusplus
}
#endif
