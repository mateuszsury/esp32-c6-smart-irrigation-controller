#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "irrigation_core.h"

#ifdef __cplusplus
extern "C" {
#endif

void scheduler_start(const irrigation_config_t *config, QueueHandle_t controller_queue);
void scheduler_update_config(const irrigation_config_t *config);

#ifdef __cplusplus
}
#endif
