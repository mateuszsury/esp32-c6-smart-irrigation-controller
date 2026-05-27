#include <inttypes.h>
#include <stdio.h>

#include "board_config.h"
#include "config_store.h"
#include "control_panel.h"
#include "diag.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "irrigation_core.h"
#include "mqtt_mode.h"
#include "ota_update.h"
#include "relay_board.h"
#include "scheduler.h"
#include "status_led.h"
#include "zigbee_mode.h"

static const char *TAG = "main";

static irrigation_core_t s_core;
static QueueHandle_t s_controller_queue;
static irrigation_config_t s_boot_config;

static void publish_controller_state(void)
{
    mqtt_mode_publish_state(&s_core);
    zigbee_mode_publish_state(&s_core);
}

static void status_led_task(void *arg)
{
    (void)arg;
    while (true) {
        if (!control_panel_wifi_connected()) {
            status_led_set_pattern(control_panel_ap_active() ? STATUS_LED_PATTERN_PORTAL_YELLOW : STATUS_LED_PATTERN_CONNECTING_YELLOW);
        } else if (s_core.config.mode == IRRIGATION_MODE_ZIGBEE) {
            status_led_set_pattern(STATUS_LED_PATTERN_ZIGBEE_GREEN);
        } else {
            status_led_set_pattern(STATUS_LED_PATTERN_MQTT_BLUE);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static irrigation_schedule_entry_t *line_schedule_entry(uint8_t line_id, uint8_t schedule_id)
{
    if (line_id >= s_core.config.line_count || schedule_id >= IRRIGATION_MAX_SCHEDULES_PER_LINE) {
        return NULL;
    }
    irrigation_line_config_t *line = &s_core.config.lines[line_id];
    if (line->schedule_count <= schedule_id) {
        line->schedule_count = schedule_id + 1;
    }
    irrigation_schedule_entry_t *entry = &line->schedules[schedule_id];
    if (entry->weekdays_mask == 0) {
        entry->weekdays_mask = 127;
    }
    if (entry->duration_s == 0) {
        entry->duration_s = line->default_duration_s;
    }
    return entry;
}

static irrigation_result_t save_live_config(void)
{
    esp_err_t save_err = config_store_save(&s_core.config);
    if (save_err == ESP_OK) {
        scheduler_update_config(&s_core.config);
        return IRRIGATION_OK;
    }
    return IRRIGATION_ERR_INVALID_CONFIG;
}

static void configure_mode_button(void)
{
    gpio_config_t button = {
        .pin_bit_mask = 1ULL << IRRIGATION_MODE_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));
}

static bool mode_button_is_active(void)
{
    return gpio_get_level((gpio_num_t)IRRIGATION_MODE_BUTTON_GPIO) == IRRIGATION_MODE_BUTTON_ACTIVE_LEVEL;
}

static void maybe_toggle_mode_on_boot(irrigation_config_t *config)
{
    configure_mode_button();
    if (!mode_button_is_active()) {
        return;
    }

    ESP_LOGW(TAG, "mode button active at boot, waiting for long hold");
    vTaskDelay(pdMS_TO_TICKS(IRRIGATION_MODE_HOLD_MS));
    if (!mode_button_is_active()) {
        ESP_LOGI(TAG, "mode button released before threshold");
        return;
    }

    config->mode = irrigation_mode_toggle(config->mode);
    ESP_LOGW(TAG, "switching communication mode to %s", irrigation_mode_to_string(config->mode));
    esp_err_t err = config_store_save(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save toggled mode: %s", esp_err_to_name(err));
    }
}

static void controller_task(void *arg)
{
    (void)arg;
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK && wdt_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "controller watchdog registration failed: %s", esp_err_to_name(wdt_err));
    }
    irrigation_event_t event;
    uint64_t last_state_publish_ms = 0;

    while (true) {
        (void)esp_task_wdt_reset();
        if (xQueueReceive(s_controller_queue, &event, pdMS_TO_TICKS(250)) == pdTRUE) {
            irrigation_result_t result = IRRIGATION_OK;
            switch (event.type) {
            case IRRIGATION_EVENT_COMMAND_ON:
            case IRRIGATION_EVENT_SCHEDULE_START:
                if (ota_update_is_in_progress()) {
                    result = IRRIGATION_ERR_INVALID_STATE;
                    snprintf(s_core.last_error, sizeof(s_core.last_error), "OTA in progress");
                } else {
                    result = irrigation_core_command_line(&s_core, event.line_id, true, event.duration_s, now_ms(), event.source);
                }
                break;
            case IRRIGATION_EVENT_COMMAND_OFF:
                result = irrigation_core_command_line(&s_core, event.line_id, false, 0, now_ms(), event.source);
                break;
            case IRRIGATION_EVENT_ALL_OFF:
            case IRRIGATION_EVENT_COMM_LOST:
                result = irrigation_core_all_off(&s_core);
                break;
            case IRRIGATION_EVENT_TIMEOUT:
                irrigation_core_tick(&s_core, now_ms());
                break;
            case IRRIGATION_EVENT_CONFIG_UPDATE:
                result = irrigation_core_apply_config(&s_core, &s_core.config);
                break;
            case IRRIGATION_EVENT_SET_DURATION:
                if (event.line_id < s_core.config.line_count && event.duration_s > 0) {
                    s_core.config.lines[event.line_id].default_duration_s = event.duration_s;
                    irrigation_schedule_entry_t *entry = line_schedule_entry(event.line_id, 0);
                    if (entry != NULL) {
                        entry->duration_s = event.duration_s;
                    }
                    result = save_live_config();
                } else {
                    result = IRRIGATION_ERR_INVALID_ARG;
                }
                break;
            case IRRIGATION_EVENT_SET_INTERLOCK:
                s_core.config.interlock = event.duration_s != 0;
                result = save_live_config();
                break;
            case IRRIGATION_EVENT_SET_SCHEDULE_ENABLED:
                {
                    irrigation_schedule_entry_t *entry = line_schedule_entry(event.line_id, event.schedule_id);
                    if (entry != NULL) {
                        entry->enabled = event.duration_s != 0;
                        result = save_live_config();
                    } else {
                        result = IRRIGATION_ERR_INVALID_ARG;
                    }
                }
                break;
            case IRRIGATION_EVENT_SET_SCHEDULE_START_MINUTE:
                {
                    irrigation_schedule_entry_t *entry = line_schedule_entry(event.line_id, event.schedule_id);
                    if (entry != NULL && event.duration_s < 1440U) {
                        entry->hour = (uint8_t)(event.duration_s / 60U);
                        entry->minute = (uint8_t)(event.duration_s % 60U);
                        result = save_live_config();
                    } else {
                        result = IRRIGATION_ERR_INVALID_ARG;
                    }
                }
                break;
            case IRRIGATION_EVENT_SET_SCHEDULE_WEEKDAYS:
                {
                    irrigation_schedule_entry_t *entry = line_schedule_entry(event.line_id, event.schedule_id);
                    if (entry != NULL && event.weekdays_mask <= IRRIGATION_WEEKDAYS_ALL) {
                        entry->weekdays_mask = event.weekdays_mask;
                        result = save_live_config();
                    } else {
                        result = IRRIGATION_ERR_INVALID_ARG;
                    }
                }
                break;
            case IRRIGATION_EVENT_SET_SCHEDULE_WEEKDAY:
                {
                    irrigation_schedule_entry_t *entry = line_schedule_entry(event.line_id, event.schedule_id);
                    if (entry != NULL && event.weekdays_mask != 0 && event.weekdays_mask <= IRRIGATION_WEEKDAYS_ALL) {
                        uint8_t mask = entry->weekdays_mask <= IRRIGATION_WEEKDAYS_ALL ? entry->weekdays_mask : IRRIGATION_WEEKDAYS_ALL;
                        if (event.duration_s != 0) {
                            mask |= event.weekdays_mask;
                        } else {
                            mask &= (uint8_t)~event.weekdays_mask;
                        }
                        entry->weekdays_mask = mask;
                        result = save_live_config();
                    } else {
                        result = IRRIGATION_ERR_INVALID_ARG;
                    }
                }
                break;
            default:
                break;
            }

            if (result != IRRIGATION_OK) {
                ESP_LOGE(TAG, "controller event %d failed: %d (%s)", (int)event.type, (int)result, s_core.last_error);
            }
            publish_controller_state();
        }
        irrigation_core_tick(&s_core, now_ms());
        uint64_t current_ms = now_ms();
        if (current_ms - last_state_publish_ms >= 5000ULL) {
            last_state_publish_ms = current_ms;
            publish_controller_state();
        }
    }
}

static void log_boot_state(const irrigation_config_t *config)
{
    ESP_LOGI(TAG, "firmware=%s reset_reason=%d mode=%s lines=%u interlock=%s",
             IRRIGATION_FIRMWARE_VERSION,
             (int)esp_reset_reason(),
             irrigation_mode_to_string(config->mode),
             config->line_count,
             config->interlock ? "true" : "false");
    for (uint8_t i = 0; i < config->line_count; ++i) {
        ESP_LOGI(TAG, "line %u name=%s gpio=%d active_high=%s enabled=%s duration=%" PRIu32,
                 i + 1,
                 config->lines[i].name,
                 config->lines[i].gpio,
                 config->lines[i].active_high ? "true" : "false",
                 config->lines[i].enabled ? "true" : "false",
                 config->lines[i].default_duration_s);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(config_store_init());

    esp_err_t cfg_err = config_store_load(&s_boot_config);
    if (cfg_err != ESP_OK) {
        ESP_LOGW(TAG, "config load returned %s, continuing with defaults/fallback", esp_err_to_name(cfg_err));
    }

    maybe_toggle_mode_on_boot(&s_boot_config);
    log_boot_state(&s_boot_config);

    if (relay_board_init(&s_boot_config) != 0) {
        ESP_LOGE(TAG, "relay init failed, restarting");
        esp_restart();
    }
    (void)relay_board_all_off();

    ESP_ERROR_CHECK(irrigation_core_init(&s_core, &s_boot_config, relay_board_set_line, NULL) == IRRIGATION_OK ? ESP_OK : ESP_FAIL);

    s_controller_queue = xQueueCreate(16, sizeof(irrigation_event_t));
    if (s_controller_queue == NULL) {
        ESP_LOGE(TAG, "failed to create controller queue");
        esp_restart();
    }

    ESP_ERROR_CHECK(ota_update_init(s_controller_queue));
    esp_err_t led_err = status_led_start();
    if (led_err != ESP_OK) {
        ESP_LOGW(TAG, "status LED disabled: %s", esp_err_to_name(led_err));
    }
    xTaskCreate(controller_task, "irrigation_controller", 4096, NULL, 10, NULL);
    scheduler_start(&s_boot_config, s_controller_queue);
    diag_start(&s_core, s_controller_queue);
    ESP_ERROR_CHECK(control_panel_start(&s_core, s_controller_queue));
    xTaskCreate(status_led_task, "status_led_policy", 2048, NULL, 4, NULL);

    esp_err_t comm_err = ESP_OK;
    esp_err_t mqtt_err = ESP_OK;
    esp_err_t zigbee_err = ESP_OK;
    if (s_boot_config.mode == IRRIGATION_MODE_MQTT) {
        if (control_panel_wait_for_wifi(pdMS_TO_TICKS(15000))) {
            mqtt_err = mqtt_mode_start(&s_core.config, s_controller_queue);
        } else {
            ESP_LOGW(TAG, "Wi-Fi is not connected; MQTT will stay disabled until network settings are saved from the control panel");
            mqtt_err = ESP_ERR_TIMEOUT;
        }
        if (s_core.config.zigbee_router_with_mqtt) {
            zigbee_err = zigbee_mode_start(&s_core.config, s_controller_queue);
            if (zigbee_err == ESP_OK) {
                ESP_LOGI(TAG, "Zigbee router started in parallel with MQTT mode");
            }
        }
        comm_err = mqtt_err != ESP_OK ? mqtt_err : zigbee_err;
    } else {
        zigbee_err = zigbee_mode_start(&s_core.config, s_controller_queue);
        comm_err = zigbee_err;
    }

    if (comm_err != ESP_OK) {
        ESP_LOGE(TAG, "communication mode %s failed: %s; forcing all relays OFF",
                 irrigation_mode_to_string(s_boot_config.mode),
                 esp_err_to_name(comm_err));
        irrigation_event_t event = {
            .type = IRRIGATION_EVENT_COMM_LOST,
            .source = IRRIGATION_SOURCE_SYSTEM,
        };
        (void)xQueueSend(s_controller_queue, &event, pdMS_TO_TICKS(100));
    }
    ota_update_mark_app_valid_after_boot();
}
