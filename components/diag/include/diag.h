#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "irrigation_core.h"

#ifdef __cplusplus
extern "C" {
#endif

void diag_start(const irrigation_core_t *core, QueueHandle_t controller_queue);

#ifdef __cplusplus
}
#endif
