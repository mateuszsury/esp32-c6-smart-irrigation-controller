#include "ota_update.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/task.h"
#include "irrigation_core.h"
#include "relay_board.h"

#define OTA_HTTP_BUFFER_SIZE 2048
#define OTA_URL_MAX_LEN 256

static const char *TAG = "ota_update";

struct ota_update_upload {
    const esp_partition_t *partition;
    esp_ota_handle_t ota;
    size_t total_size;
    size_t written;
    bool active;
};

typedef struct {
    QueueHandle_t queue;
    bool in_progress;
    char status[24];
    char last_error[96];
} ota_update_ctx_t;

static ota_update_ctx_t s_ota = {
    .status = "idle",
    .last_error = "ok",
};

static void set_status(const char *status)
{
    snprintf(s_ota.status, sizeof(s_ota.status), "%s", status != NULL ? status : "unknown");
    ESP_LOGI(TAG, "status=%s", s_ota.status);
}

static void set_error(const char *message, esp_err_t err)
{
    if (message == NULL) {
        message = "ota failed";
    }
    if (err == ESP_OK) {
        snprintf(s_ota.last_error, sizeof(s_ota.last_error), "%s", message);
    } else {
        snprintf(s_ota.last_error, sizeof(s_ota.last_error), "%s: %s", message, esp_err_to_name(err));
    }
    set_status("failed");
    ESP_LOGE(TAG, "%s", s_ota.last_error);
}

static void force_all_off(void)
{
    if (s_ota.queue != NULL) {
        irrigation_event_t event = {
            .type = IRRIGATION_EVENT_ALL_OFF,
            .source = IRRIGATION_SOURCE_SYSTEM,
        };
        (void)xQueueSend(s_ota.queue, &event, pdMS_TO_TICKS(100));
    }
    (void)relay_board_all_off();
    vTaskDelay(pdMS_TO_TICKS(250));
}

static esp_err_t begin_update(size_t total_size, struct ota_update_upload **out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (s_ota.in_progress) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        set_error("no OTA partition available", ESP_ERR_NOT_FOUND);
        return ESP_ERR_NOT_FOUND;
    }
    if (total_size > 0 && total_size > partition->size) {
        set_error("OTA image is larger than update partition", ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    struct ota_update_upload *handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        set_error("cannot allocate OTA handle", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    s_ota.in_progress = true;
    snprintf(s_ota.last_error, sizeof(s_ota.last_error), "ok");
    set_status("preparing");
    force_all_off();

    esp_err_t err = esp_ota_begin(partition, total_size > 0 ? total_size : OTA_SIZE_UNKNOWN, &handle->ota);
    if (err != ESP_OK) {
        free(handle);
        s_ota.in_progress = false;
        set_error("esp_ota_begin failed", err);
        return err;
    }

    handle->partition = partition;
    handle->total_size = total_size;
    handle->active = true;
    *out = handle;
    set_status("writing");
    ESP_LOGI(TAG, "writing OTA to partition %s at 0x%" PRIx32 " size=%" PRIu32,
             partition->label,
             partition->address,
             partition->size);
    return ESP_OK;
}

static esp_err_t complete_update(struct ota_update_upload *handle)
{
    if (handle == NULL || !handle->active) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->written == 0 || (handle->total_size > 0 && handle->written != handle->total_size)) {
        set_error("OTA image size mismatch", ESP_ERR_INVALID_SIZE);
        esp_ota_abort(handle->ota);
        handle->active = false;
        free(handle);
        s_ota.in_progress = false;
        return ESP_ERR_INVALID_SIZE;
    }

    set_status("verifying");
    esp_err_t err = esp_ota_end(handle->ota);
    if (err != ESP_OK) {
        handle->active = false;
        free(handle);
        s_ota.in_progress = false;
        set_error("esp_ota_end failed", err);
        return err;
    }

    esp_app_desc_t app_desc = {0};
    err = esp_ota_get_partition_description(handle->partition, &app_desc);
    if (err != ESP_OK) {
        handle->active = false;
        free(handle);
        s_ota.in_progress = false;
        set_error("cannot read uploaded app descriptor", err);
        return err;
    }
    ESP_LOGI(TAG, "uploaded app project=%s version=%s idf=%s",
             app_desc.project_name,
             app_desc.version,
             app_desc.idf_ver);

    err = esp_ota_set_boot_partition(handle->partition);
    if (err != ESP_OK) {
        handle->active = false;
        free(handle);
        s_ota.in_progress = false;
        set_error("esp_ota_set_boot_partition failed", err);
        return err;
    }

    handle->active = false;
    free(handle);
    set_status("rebooting");
    return ESP_OK;
}

static void ota_url_task(void *arg)
{
    char *url = (char *)arg;
    struct ota_update_upload *upload = NULL;
    char *buffer = NULL;
    esp_http_client_handle_t client = NULL;

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    client = esp_http_client_init(&config);
    if (client == NULL) {
        set_error("esp_http_client_init failed", ESP_FAIL);
        goto done;
    }

    set_status("downloading");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        set_error("cannot open OTA URL", err);
        goto done;
    }
    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (status_code < 200 || status_code >= 300) {
        set_error("OTA URL returned non-2xx status", ESP_FAIL);
        goto done;
    }
    if (content_length <= 0) {
        set_error("OTA URL missing Content-Length", ESP_ERR_INVALID_SIZE);
        goto done;
    }

    err = begin_update((size_t)content_length, &upload);
    if (err != ESP_OK) {
        goto done;
    }

    buffer = malloc(OTA_HTTP_BUFFER_SIZE);
    if (buffer == NULL) {
        set_error("cannot allocate OTA download buffer", ESP_ERR_NO_MEM);
        goto done;
    }

    while (upload->written < upload->total_size) {
        int read_len = esp_http_client_read(client, buffer, OTA_HTTP_BUFFER_SIZE);
        if (read_len < 0) {
            set_error("OTA URL read failed", ESP_FAIL);
            goto done;
        }
        if (read_len == 0) {
            break;
        }
        err = ota_update_upload_write(upload, buffer, (size_t)read_len);
        if (err != ESP_OK) {
            goto done;
        }
    }

    err = ota_update_upload_finish(upload);
    upload = NULL;
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(750));
        esp_restart();
    }

done:
    if (upload != NULL) {
        ota_update_upload_abort(upload);
    } else if (strcmp(s_ota.status, "rebooting") != 0) {
        s_ota.in_progress = false;
    }
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(buffer);
    free(url);
    vTaskDelete(NULL);
}

esp_err_t ota_update_init(QueueHandle_t controller_queue)
{
    s_ota.queue = controller_queue;
    s_ota.in_progress = false;
    set_status("idle");
    snprintf(s_ota.last_error, sizeof(s_ota.last_error), "ok");
    return ESP_OK;
}

void ota_update_mark_app_valid_after_boot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "running OTA image is pending verification; marking valid");
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to mark OTA image valid: %s", esp_err_to_name(err));
        }
    }
}

bool ota_update_is_in_progress(void)
{
    return s_ota.in_progress;
}

const char *ota_update_status(void)
{
    return s_ota.status;
}

const char *ota_update_last_error(void)
{
    return s_ota.last_error;
}

esp_err_t ota_update_start_url(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(url) >= OTA_URL_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_ota.in_progress) {
        return ESP_ERR_INVALID_STATE;
    }
    char *copy = strdup(url);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreate(ota_url_task, "ota_url", 8192, copy, 8, NULL);
    if (ok != pdPASS) {
        free(copy);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ota_update_upload_begin(size_t total_size, ota_update_upload_handle_t *out_handle)
{
    struct ota_update_upload *upload = NULL;
    esp_err_t err = begin_update(total_size, &upload);
    if (err == ESP_OK) {
        *out_handle = upload;
    }
    return err;
}

esp_err_t ota_update_upload_write(ota_update_upload_handle_t handle, const void *data, size_t len)
{
    if (handle == NULL || data == NULL || len == 0 || !handle->active) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->total_size > 0 && handle->written + len > handle->total_size) {
        set_error("OTA write exceeds expected size", ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = esp_ota_write(handle->ota, data, len);
    if (err != ESP_OK) {
        set_error("esp_ota_write failed", err);
        return err;
    }
    handle->written += len;
    return ESP_OK;
}

esp_err_t ota_update_upload_finish(ota_update_upload_handle_t handle)
{
    return complete_update(handle);
}

void ota_update_upload_abort(ota_update_upload_handle_t handle)
{
    if (handle != NULL) {
        if (handle->active) {
            esp_ota_abort(handle->ota);
        }
        free(handle);
    }
    s_ota.in_progress = false;
    if (strcmp(s_ota.status, "failed") != 0) {
        set_status("idle");
    }
}
