#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "irrigation_core.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t zigbee_mode_start(const irrigation_config_t *config, QueueHandle_t controller_queue);
void zigbee_mode_publish_state(const irrigation_core_t *core);
bool zigbee_mode_is_joined(void);

#ifdef __cplusplus
}
#endif
