#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ota_update_upload *ota_update_upload_handle_t;

esp_err_t ota_update_init(QueueHandle_t controller_queue);
void ota_update_mark_app_valid_after_boot(void);
bool ota_update_is_in_progress(void);
const char *ota_update_status(void);
const char *ota_update_last_error(void);

esp_err_t ota_update_start_url(const char *url);
esp_err_t ota_update_upload_begin(size_t total_size, ota_update_upload_handle_t *out_handle);
esp_err_t ota_update_upload_write(ota_update_upload_handle_t handle, const void *data, size_t len);
esp_err_t ota_update_upload_finish(ota_update_upload_handle_t handle);
void ota_update_upload_abort(ota_update_upload_handle_t handle);

#ifdef __cplusplus
}
#endif
