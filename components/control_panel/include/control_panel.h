#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "irrigation_core.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t control_panel_start(irrigation_core_t *core, QueueHandle_t controller_queue);
bool control_panel_wifi_connected(void);
bool control_panel_ap_active(void);
bool control_panel_wait_for_wifi(TickType_t timeout_ticks);

#ifdef __cplusplus
}
#endif
