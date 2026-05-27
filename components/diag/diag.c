#include "diag.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_store.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/task.h"

static const char *TAG = "diag";

typedef struct {
    const irrigation_core_t *core;
    QueueHandle_t queue;
} diag_ctx_t;

static diag_ctx_t s_diag;
static irrigation_config_t s_diag_config;

static void strip_eol(char *line)
{
    line[strcspn(line, "\r\n")] = '\0';
}

static void print_status(const irrigation_core_t *core)
{
    if (core == NULL) {
        return;
    }
    printf("\nmode=%s firmware=%s reset_reason=%d interlock=%s last_error=%s\n",
           irrigation_mode_to_string(core->config.mode),
           IRRIGATION_FIRMWARE_VERSION,
           (int)esp_reset_reason(),
           core->config.interlock ? "true" : "false",
           core->last_error);
    for (uint8_t i = 0; i < core->config.line_count; ++i) {
        printf("line_%u name=%s gpio=%d enabled=%s state=%s duration=%" PRIu32 "\n",
               i + 1,
               core->config.lines[i].name,
               core->config.lines[i].gpio,
               core->config.lines[i].enabled ? "true" : "false",
               irrigation_core_line_is_on(core, i) ? "on" : "off",
               core->config.lines[i].default_duration_s);
    }
}

static irrigation_config_t *config_copy_from_core(const irrigation_core_t *core)
{
    if (core == NULL) {
        return NULL;
    }
    memcpy(&s_diag_config, &core->config, sizeof(s_diag_config));
    return &s_diag_config;
}

static void diag_task(void *arg)
{
    diag_ctx_t *ctx = (diag_ctx_t *)arg;
    char line[256];
    printf("\nIrrigation diag console. Commands: status, off, on <n>, lineoff <n>, wifi <ssid> <password>, mqtt <uri> <user> <password> [prefix], mode mqtt|zigbee, reset-config\n");
    while (true) {
        printf("> ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        strip_eol(line);

        if (strncmp(line, "status", 6) == 0) {
            print_status(ctx->core);
        } else if (strncmp(line, "off", 3) == 0) {
            irrigation_event_t event = {
                .type = IRRIGATION_EVENT_ALL_OFF,
                .source = IRRIGATION_SOURCE_UART,
            };
            (void)xQueueSend(ctx->queue, &event, pdMS_TO_TICKS(100));
        } else if (strncmp(line, "on ", 3) == 0) {
            int line_number = atoi(line + 3);
            if (line_number > 0) {
                irrigation_event_t event = {
                    .type = IRRIGATION_EVENT_COMMAND_ON,
                    .source = IRRIGATION_SOURCE_UART,
                    .line_id = (uint8_t)(line_number - 1),
                };
                (void)xQueueSend(ctx->queue, &event, pdMS_TO_TICKS(100));
            }
        } else if (strncmp(line, "lineoff ", 8) == 0) {
            int line_number = atoi(line + 8);
            if (line_number > 0) {
                irrigation_event_t event = {
                    .type = IRRIGATION_EVENT_COMMAND_OFF,
                    .source = IRRIGATION_SOURCE_UART,
                    .line_id = (uint8_t)(line_number - 1),
                };
                (void)xQueueSend(ctx->queue, &event, pdMS_TO_TICKS(100));
            }
        } else if (strncmp(line, "wifi ", 5) == 0) {
            char ssid[33] = {0};
            char password[65] = {0};
            if (sscanf(line + 5, "%32s %64s", ssid, password) == 2) {
                irrigation_config_t *config = config_copy_from_core(ctx->core);
                if (config == NULL) {
                    ESP_LOGE(TAG, "cannot update Wi-Fi settings without core state");
                    continue;
                }
                snprintf(config->wifi_ssid, sizeof(config->wifi_ssid), "%s", ssid);
                snprintf(config->wifi_password, sizeof(config->wifi_password), "%s", password);
                ESP_LOGW(TAG, "Wi-Fi settings update requested from UART");
                if (config_store_save(config) == ESP_OK) {
                    esp_restart();
                } else {
                    ESP_LOGE(TAG, "failed to save Wi-Fi settings");
                }
            } else {
                printf("usage: wifi <ssid> <password>\n");
            }
        } else if (strncmp(line, "mqtt ", 5) == 0) {
            char uri[128] = {0};
            char username[65] = {0};
            char password[65] = {0};
            char prefix[48] = {0};
            int count = sscanf(line + 5, "%127s %64s %64s %47s", uri, username, password, prefix);
            if (count >= 3) {
                irrigation_config_t *config = config_copy_from_core(ctx->core);
                if (config == NULL) {
                    ESP_LOGE(TAG, "cannot update MQTT settings without core state");
                    continue;
                }
                snprintf(config->mqtt_uri, sizeof(config->mqtt_uri), "%s", uri);
                snprintf(config->mqtt_username, sizeof(config->mqtt_username), "%s", username);
                snprintf(config->mqtt_password, sizeof(config->mqtt_password), "%s", password);
                if (count >= 4 && prefix[0] != '\0') {
                    snprintf(config->mqtt_prefix, sizeof(config->mqtt_prefix), "%s", prefix);
                }
                ESP_LOGW(TAG, "MQTT settings update requested from UART");
                if (config_store_save(config) == ESP_OK) {
                    esp_restart();
                } else {
                    ESP_LOGE(TAG, "failed to save MQTT settings");
                }
            } else {
                printf("usage: mqtt <uri> <user> <password> [prefix]\n");
            }
        } else if (strncmp(line, "mode mqtt", 9) == 0 || strncmp(line, "mode zigbee", 11) == 0) {
            irrigation_config_t *config = config_copy_from_core(ctx->core);
            if (config == NULL) {
                ESP_LOGE(TAG, "cannot update mode without core state");
                continue;
            }
            config->mode = strncmp(line, "mode zigbee", 11) == 0 ? IRRIGATION_MODE_ZIGBEE : IRRIGATION_MODE_MQTT;
            irrigation_event_t event = {
                .type = IRRIGATION_EVENT_ALL_OFF,
                .source = IRRIGATION_SOURCE_UART,
            };
            (void)xQueueSend(ctx->queue, &event, pdMS_TO_TICKS(100));
            ESP_LOGW(TAG, "mode change requested from UART: %s", irrigation_mode_to_string(config->mode));
            if (config_store_save(config) == ESP_OK) {
                esp_restart();
            } else {
                ESP_LOGE(TAG, "failed to save mode change");
            }
        } else if (strncmp(line, "reset-config", 12) == 0) {
            ESP_LOGW(TAG, "factory reset requested from UART");
            (void)config_store_factory_reset();
            esp_restart();
        }
    }
}

void diag_start(const irrigation_core_t *core, QueueHandle_t controller_queue)
{
    s_diag.core = core;
    s_diag.queue = controller_queue;
    xTaskCreate(diag_task, "irrigation_diag", 8192, &s_diag, 3, NULL);
}
