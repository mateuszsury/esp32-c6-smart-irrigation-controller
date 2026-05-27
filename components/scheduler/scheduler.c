#include "scheduler.h"

#include <inttypes.h>
#include <time.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "scheduler";

typedef struct {
    irrigation_config_t config;
    QueueHandle_t queue;
    SemaphoreHandle_t lock;
    int last_minute_key;
} scheduler_ctx_t;

static scheduler_ctx_t s_scheduler;

static bool time_is_valid(time_t now)
{
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    return tm_now.tm_year + 1900 >= 2024;
}

static void scheduler_task(void *arg)
{
    scheduler_ctx_t *ctx = (scheduler_ctx_t *)arg;
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK && wdt_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "scheduler watchdog registration failed: %s", esp_err_to_name(wdt_err));
    }
    while (true) {
        (void)esp_task_wdt_reset();
        time_t now = time(NULL);
        if (time_is_valid(now)) {
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            int minute_key = (tm_now.tm_yday * 24 * 60) + (tm_now.tm_hour * 60) + tm_now.tm_min;
            if (minute_key != ctx->last_minute_key) {
                ctx->last_minute_key = minute_key;
                uint8_t weekday_bit = (uint8_t)(1u << tm_now.tm_wday);

                if (ctx->lock != NULL) {
                    xSemaphoreTake(ctx->lock, portMAX_DELAY);
                }
                for (uint8_t line_id = 0; line_id < ctx->config.line_count; ++line_id) {
                    const irrigation_line_config_t *line = &ctx->config.lines[line_id];
                    for (uint8_t i = 0; i < line->schedule_count; ++i) {
                        const irrigation_schedule_entry_t *entry = &line->schedules[i];
                        if (entry->enabled && entry->hour == tm_now.tm_hour && entry->minute == tm_now.tm_min &&
                            (entry->weekdays_mask & weekday_bit) != 0) {
                            irrigation_event_t event = {
                                .type = IRRIGATION_EVENT_SCHEDULE_START,
                                .source = IRRIGATION_SOURCE_SCHEDULE,
                                .line_id = line_id,
                                .duration_s = entry->duration_s,
                            };
                            (void)xQueueSend(ctx->queue, &event, 0);
                            ESP_LOGI(TAG, "scheduled start line %u for %" PRIu32 "s", line_id + 1, entry->duration_s);
                        }
                    }
                }
                if (ctx->lock != NULL) {
                    xSemaphoreGive(ctx->lock);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void scheduler_start(const irrigation_config_t *config, QueueHandle_t controller_queue)
{
    if (config == NULL || controller_queue == NULL) {
        ESP_LOGE(TAG, "scheduler_start invalid args");
        return;
    }
    s_scheduler.config = *config;
    s_scheduler.queue = controller_queue;
    s_scheduler.last_minute_key = -1;
    s_scheduler.lock = xSemaphoreCreateMutex();
    xTaskCreate(scheduler_task, "irrigation_scheduler", 4096, &s_scheduler, 5, NULL);
}

void scheduler_update_config(const irrigation_config_t *config)
{
    if (config == NULL || s_scheduler.lock == NULL) {
        return;
    }
    xSemaphoreTake(s_scheduler.lock, portMAX_DELAY);
    s_scheduler.config = *config;
    xSemaphoreGive(s_scheduler.lock);
}
