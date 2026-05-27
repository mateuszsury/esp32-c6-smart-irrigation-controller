#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "irrigation_core.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mqtt_mode_start(const irrigation_config_t *config, QueueHandle_t controller_queue);
void mqtt_mode_publish_state(const irrigation_core_t *core);

#ifdef __cplusplus
}
#endif
