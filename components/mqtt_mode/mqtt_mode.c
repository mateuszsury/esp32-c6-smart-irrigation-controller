#include "mqtt_mode.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "ota_update.h"

static const char *TAG = "mqtt_mode";

typedef struct {
    irrigation_config_t config;
    QueueHandle_t queue;
    esp_mqtt_client_handle_t client;
    char device_id[24];
    char base_topic[96];
    char availability_topic[128];
    bool connected;
} mqtt_mode_ctx_t;

static mqtt_mode_ctx_t s_mqtt;

typedef struct {
    const char *slug;
    const char *name;
    uint8_t bit;
} schedule_day_t;

static const schedule_day_t SCHEDULE_DAYS[] = {
    {"mon", "Monday", IRRIGATION_WEEKDAY_MON},
    {"tue", "Tuesday", IRRIGATION_WEEKDAY_TUE},
    {"wed", "Wednesday", IRRIGATION_WEEKDAY_WED},
    {"thu", "Thursday", IRRIGATION_WEEKDAY_THU},
    {"fri", "Friday", IRRIGATION_WEEKDAY_FRI},
    {"sat", "Saturday", IRRIGATION_WEEKDAY_SAT},
    {"sun", "Sunday", IRRIGATION_WEEKDAY_SUN},
};

static void build_topic(char *out, size_t out_len, const char *suffix)
{
    snprintf(out, out_len, "%s/%s/%s", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, suffix);
}

static const irrigation_schedule_entry_t *first_schedule(const irrigation_line_config_t *line)
{
    return line->schedule_count > 0 ? &line->schedules[0] : NULL;
}

static void schedule_time_string(const irrigation_schedule_entry_t *entry, char *out, size_t out_len)
{
    uint8_t hour = entry != NULL ? entry->hour : 6;
    uint8_t minute = entry != NULL ? entry->minute : 0;
    snprintf(out, out_len, "%02u:%02u", hour, minute);
}

static uint8_t schedule_weekdays_mask(const irrigation_schedule_entry_t *entry)
{
    if (entry == NULL) {
        return IRRIGATION_WEEKDAYS_ALL;
    }
    return entry->weekdays_mask <= IRRIGATION_WEEKDAYS_ALL ? entry->weekdays_mask : IRRIGATION_WEEKDAYS_ALL;
}

static void mqtt_publish_discovery(void)
{
    char topic[192];
    char command_topic[128];
    char state_topic[128];
    char payload[768];

    for (uint8_t i = 0; i < s_mqtt.config.line_count; ++i) {
        snprintf(command_topic, sizeof(command_topic), "%s/%s/line/%u/set", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, i + 1);
        snprintf(state_topic, sizeof(state_topic), "%s/%s/line/%u/state", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, i + 1);
        snprintf(topic, sizeof(topic), "homeassistant/switch/%s/line_%u/config", s_mqtt.device_id, i + 1);
        snprintf(payload,
                 sizeof(payload),
                 "{\"name\":\"%s\",\"unique_id\":\"%s_line_%u\",\"command_topic\":\"%s\","
                 "\"state_topic\":\"%s\",\"availability_topic\":\"%s\",\"payload_on\":\"ON\","
                 "\"payload_off\":\"OFF\",\"device\":{\"identifiers\":[\"%s\"],"
                 "\"name\":\"ESP32-C6 Irrigation\",\"manufacturer\":\"local\",\"model\":\"ESP32-C6\"}}",
                 s_mqtt.config.lines[i].name,
                 s_mqtt.device_id,
                 i + 1,
                 command_topic,
                 state_topic,
                 s_mqtt.availability_topic,
                 s_mqtt.device_id);
        esp_mqtt_client_publish(s_mqtt.client, topic, payload, 0, 1, 1);

        snprintf(command_topic, sizeof(command_topic), "%s/%s/line/%u/duration/set", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, i + 1);
        snprintf(state_topic, sizeof(state_topic), "%s/%s/line/%u/duration/state", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, i + 1);
        snprintf(topic, sizeof(topic), "homeassistant/number/%s/line_%u_duration/config", s_mqtt.device_id, i + 1);
        snprintf(payload,
                 sizeof(payload),
                 "{\"name\":\"%s duration\",\"unique_id\":\"%s_line_%u_duration\",\"command_topic\":\"%s\","
                 "\"state_topic\":\"%s\",\"availability_topic\":\"%s\",\"min\":1,\"max\":86400,\"step\":1,"
                 "\"unit_of_measurement\":\"s\",\"mode\":\"box\","
                 "\"device\":{\"identifiers\":[\"%s\"]}}",
                 s_mqtt.config.lines[i].name,
                 s_mqtt.device_id,
                 i + 1,
                 command_topic,
                 state_topic,
                 s_mqtt.availability_topic,
                 s_mqtt.device_id);
        esp_mqtt_client_publish(s_mqtt.client, topic, payload, 0, 1, 1);
        char value[16];
        snprintf(value, sizeof(value), "%" PRIu32, s_mqtt.config.lines[i].default_duration_s);
        esp_mqtt_client_publish(s_mqtt.client, state_topic, value, 0, 1, 1);

        snprintf(command_topic, sizeof(command_topic), "%s/%s/line/%u/schedule/enabled/set", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, i + 1);
        snprintf(state_topic, sizeof(state_topic), "%s/%s/line/%u/schedule/enabled/state", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, i + 1);
        snprintf(topic, sizeof(topic), "homeassistant/switch/%s/line_%u_schedule_enabled/config", s_mqtt.device_id, i + 1);
        snprintf(payload,
                 sizeof(payload),
                 "{\"name\":\"%s schedule enabled\",\"unique_id\":\"%s_line_%u_schedule_enabled\","
                 "\"command_topic\":\"%s\",\"state_topic\":\"%s\",\"availability_topic\":\"%s\","
                 "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"device\":{\"identifiers\":[\"%s\"]}}",
                 s_mqtt.config.lines[i].name,
                 s_mqtt.device_id,
                 i + 1,
                 command_topic,
                 state_topic,
                 s_mqtt.availability_topic,
                 s_mqtt.device_id);
        esp_mqtt_client_publish(s_mqtt.client, topic, payload, 0, 1, 1);

        snprintf(command_topic, sizeof(command_topic), "%s/%s/line/%u/schedule/time/set", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, i + 1);
        snprintf(state_topic, sizeof(state_topic), "%s/%s/line/%u/schedule/time/state", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, i + 1);
        snprintf(topic, sizeof(topic), "homeassistant/text/%s/line_%u_schedule_time/config", s_mqtt.device_id, i + 1);
        snprintf(payload,
                 sizeof(payload),
                 "{\"name\":\"%s schedule time\",\"unique_id\":\"%s_line_%u_schedule_time\","
                 "\"command_topic\":\"%s\",\"state_topic\":\"%s\",\"availability_topic\":\"%s\","
                 "\"pattern\":\"^([01][0-9]|2[0-3]):[0-5][0-9]$\","
                 "\"device\":{\"identifiers\":[\"%s\"]}}",
                 s_mqtt.config.lines[i].name,
                 s_mqtt.device_id,
                 i + 1,
                 command_topic,
                 state_topic,
                 s_mqtt.availability_topic,
                 s_mqtt.device_id);
        esp_mqtt_client_publish(s_mqtt.client, topic, payload, 0, 1, 1);

        snprintf(topic, sizeof(topic), "homeassistant/number/%s/line_%u_schedule_weekdays/config", s_mqtt.device_id, i + 1);
        esp_mqtt_client_publish(s_mqtt.client, topic, "", 0, 1, 1);

        for (size_t d = 0; d < sizeof(SCHEDULE_DAYS) / sizeof(SCHEDULE_DAYS[0]); ++d) {
            snprintf(command_topic,
                     sizeof(command_topic),
                     "%s/%s/line/%u/schedule/day/%s/set",
                     s_mqtt.config.mqtt_prefix,
                     s_mqtt.device_id,
                     i + 1,
                     SCHEDULE_DAYS[d].slug);
            snprintf(state_topic,
                     sizeof(state_topic),
                     "%s/%s/line/%u/schedule/day/%s/state",
                     s_mqtt.config.mqtt_prefix,
                     s_mqtt.device_id,
                     i + 1,
                     SCHEDULE_DAYS[d].slug);
            snprintf(topic,
                     sizeof(topic),
                     "homeassistant/switch/%s/line_%u_schedule_%s/config",
                     s_mqtt.device_id,
                     i + 1,
                     SCHEDULE_DAYS[d].slug);
            snprintf(payload,
                     sizeof(payload),
                     "{\"name\":\"%s schedule %s\",\"unique_id\":\"%s_line_%u_schedule_%s\","
                     "\"command_topic\":\"%s\",\"state_topic\":\"%s\",\"availability_topic\":\"%s\","
                     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"device\":{\"identifiers\":[\"%s\"]}}",
                     s_mqtt.config.lines[i].name,
                     SCHEDULE_DAYS[d].name,
                     s_mqtt.device_id,
                     i + 1,
                     SCHEDULE_DAYS[d].slug,
                     command_topic,
                     state_topic,
                     s_mqtt.availability_topic,
                     s_mqtt.device_id);
            esp_mqtt_client_publish(s_mqtt.client, topic, payload, 0, 1, 1);
        }
    }

    snprintf(topic, sizeof(topic), "homeassistant/switch/%s/interlock/config", s_mqtt.device_id);
    snprintf(payload,
             sizeof(payload),
             "{\"name\":\"Interlock\",\"unique_id\":\"%s_interlock\",\"command_topic\":\"%s/%s/interlock/set\","
             "\"state_topic\":\"%s/%s/interlock/state\","
             "\"availability_topic\":\"%s\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
             "\"device\":{\"identifiers\":[\"%s\"]}}",
             s_mqtt.device_id,
             s_mqtt.config.mqtt_prefix,
             s_mqtt.device_id,
             s_mqtt.config.mqtt_prefix,
             s_mqtt.device_id,
             s_mqtt.availability_topic,
             s_mqtt.device_id);
    esp_mqtt_client_publish(s_mqtt.client, topic, payload, 0, 1, 1);
    char interlock_state_topic[128];
    build_topic(interlock_state_topic, sizeof(interlock_state_topic), "interlock/state");
    esp_mqtt_client_publish(s_mqtt.client, interlock_state_topic, s_mqtt.config.interlock ? "ON" : "OFF", 0, 1, 1);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/mode/config", s_mqtt.device_id);
    snprintf(payload,
             sizeof(payload),
             "{\"name\":\"Mode\",\"unique_id\":\"%s_mode\",\"state_topic\":\"%s/%s/mode\","
             "\"availability_topic\":\"%s\",\"device\":{\"identifiers\":[\"%s\"]}}",
             s_mqtt.device_id,
             s_mqtt.config.mqtt_prefix,
             s_mqtt.device_id,
             s_mqtt.availability_topic,
             s_mqtt.device_id);
    esp_mqtt_client_publish(s_mqtt.client, topic, payload, 0, 1, 1);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/uptime/config", s_mqtt.device_id);
    snprintf(payload,
             sizeof(payload),
             "{\"name\":\"Uptime\",\"unique_id\":\"%s_uptime\",\"state_topic\":\"%s/%s/uptime\","
             "\"availability_topic\":\"%s\",\"device_class\":\"duration\",\"unit_of_measurement\":\"s\","
             "\"device\":{\"identifiers\":[\"%s\"]}}",
             s_mqtt.device_id,
             s_mqtt.config.mqtt_prefix,
             s_mqtt.device_id,
             s_mqtt.availability_topic,
             s_mqtt.device_id);
    esp_mqtt_client_publish(s_mqtt.client, topic, payload, 0, 1, 1);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/rssi/config", s_mqtt.device_id);
    snprintf(payload,
             sizeof(payload),
             "{\"name\":\"RSSI\",\"unique_id\":\"%s_rssi\",\"state_topic\":\"%s/%s/rssi\","
             "\"availability_topic\":\"%s\",\"device_class\":\"signal_strength\",\"unit_of_measurement\":\"dBm\","
             "\"device\":{\"identifiers\":[\"%s\"]}}",
             s_mqtt.device_id,
             s_mqtt.config.mqtt_prefix,
             s_mqtt.device_id,
             s_mqtt.availability_topic,
             s_mqtt.device_id);
    esp_mqtt_client_publish(s_mqtt.client, topic, payload, 0, 1, 1);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/last_error/config", s_mqtt.device_id);
    snprintf(payload,
             sizeof(payload),
             "{\"name\":\"Last error\",\"unique_id\":\"%s_last_error\",\"state_topic\":\"%s/%s/last_error\","
             "\"availability_topic\":\"%s\",\"device\":{\"identifiers\":[\"%s\"]}}",
             s_mqtt.device_id,
             s_mqtt.config.mqtt_prefix,
             s_mqtt.device_id,
             s_mqtt.availability_topic,
             s_mqtt.device_id);
    esp_mqtt_client_publish(s_mqtt.client, topic, payload, 0, 1, 1);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/ota_status/config", s_mqtt.device_id);
    snprintf(payload,
             sizeof(payload),
             "{\"name\":\"OTA status\",\"unique_id\":\"%s_ota_status\",\"state_topic\":\"%s/%s/ota/state\","
             "\"availability_topic\":\"%s\",\"device\":{\"identifiers\":[\"%s\"]}}",
             s_mqtt.device_id,
             s_mqtt.config.mqtt_prefix,
             s_mqtt.device_id,
             s_mqtt.availability_topic,
             s_mqtt.device_id);
    esp_mqtt_client_publish(s_mqtt.client, topic, payload, 0, 1, 1);

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/ota_error/config", s_mqtt.device_id);
    snprintf(payload,
             sizeof(payload),
             "{\"name\":\"OTA error\",\"unique_id\":\"%s_ota_error\",\"state_topic\":\"%s/%s/ota/error\","
             "\"availability_topic\":\"%s\",\"device\":{\"identifiers\":[\"%s\"]}}",
             s_mqtt.device_id,
             s_mqtt.config.mqtt_prefix,
             s_mqtt.device_id,
             s_mqtt.availability_topic,
             s_mqtt.device_id);
    esp_mqtt_client_publish(s_mqtt.client, topic, payload, 0, 1, 1);

    snprintf(topic, sizeof(topic), "homeassistant/text/%s/ota_url/config", s_mqtt.device_id);
    snprintf(payload,
             sizeof(payload),
             "{\"name\":\"OTA URL\",\"unique_id\":\"%s_ota_url\",\"command_topic\":\"%s/%s/ota/url/set\","
             "\"state_topic\":\"%s/%s/ota/url/state\",\"availability_topic\":\"%s\","
             "\"device\":{\"identifiers\":[\"%s\"]}}",
             s_mqtt.device_id,
             s_mqtt.config.mqtt_prefix,
             s_mqtt.device_id,
             s_mqtt.config.mqtt_prefix,
             s_mqtt.device_id,
             s_mqtt.availability_topic,
             s_mqtt.device_id);
    esp_mqtt_client_publish(s_mqtt.client, topic, payload, 0, 1, 1);

    esp_mqtt_client_publish(s_mqtt.client, s_mqtt.availability_topic, "online", 0, 1, 1);

    char mode_topic[128];
    build_topic(mode_topic, sizeof(mode_topic), "mode");
    esp_mqtt_client_publish(s_mqtt.client, mode_topic, irrigation_mode_to_string(s_mqtt.config.mode), 0, 1, 1);
}

void mqtt_mode_publish_state(const irrigation_core_t *core)
{
    if (core == NULL || s_mqtt.client == NULL || !s_mqtt.connected) {
        return;
    }

    esp_mqtt_client_publish(s_mqtt.client, s_mqtt.availability_topic, "online", 0, 1, 1);

    char topic[128];
    char value[64];
    for (uint8_t i = 0; i < core->config.line_count; ++i) {
        const irrigation_schedule_entry_t *entry = first_schedule(&core->config.lines[i]);
        uint8_t weekdays_mask = schedule_weekdays_mask(entry);
        snprintf(topic, sizeof(topic), "%s/%s/line/%u/state", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, i + 1);
        esp_mqtt_client_publish(s_mqtt.client, topic, irrigation_core_line_is_on(core, i) ? "ON" : "OFF", 0, 1, 1);

        snprintf(topic, sizeof(topic), "%s/%s/line/%u/duration/state", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, i + 1);
        snprintf(value, sizeof(value), "%" PRIu32, core->config.lines[i].default_duration_s);
        esp_mqtt_client_publish(s_mqtt.client, topic, value, 0, 1, 1);

        snprintf(topic, sizeof(topic), "%s/%s/line/%u/schedule/enabled/state", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, i + 1);
        esp_mqtt_client_publish(s_mqtt.client, topic, entry != NULL && entry->enabled ? "ON" : "OFF", 0, 1, 1);

        snprintf(topic, sizeof(topic), "%s/%s/line/%u/schedule/time/state", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, i + 1);
        schedule_time_string(entry, value, sizeof(value));
        esp_mqtt_client_publish(s_mqtt.client, topic, value, 0, 1, 1);

        snprintf(topic, sizeof(topic), "%s/%s/line/%u/schedule/weekdays/state", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, i + 1);
        snprintf(value, sizeof(value), "%u", weekdays_mask);
        esp_mqtt_client_publish(s_mqtt.client, topic, value, 0, 1, 1);

        for (size_t d = 0; d < sizeof(SCHEDULE_DAYS) / sizeof(SCHEDULE_DAYS[0]); ++d) {
            snprintf(topic,
                     sizeof(topic),
                     "%s/%s/line/%u/schedule/day/%s/state",
                     s_mqtt.config.mqtt_prefix,
                     s_mqtt.device_id,
                     i + 1,
                     SCHEDULE_DAYS[d].slug);
            esp_mqtt_client_publish(
                s_mqtt.client,
                topic,
                (weekdays_mask & SCHEDULE_DAYS[d].bit) != 0 ? "ON" : "OFF",
                0,
                1,
                1);
        }
    }

    build_topic(topic, sizeof(topic), "interlock/state");
    esp_mqtt_client_publish(s_mqtt.client, topic, core->config.interlock ? "ON" : "OFF", 0, 1, 1);

    build_topic(topic, sizeof(topic), "mode");
    esp_mqtt_client_publish(s_mqtt.client, topic, irrigation_mode_to_string(core->config.mode), 0, 1, 1);

    build_topic(topic, sizeof(topic), "uptime");
    snprintf(value, sizeof(value), "%" PRIu64, (uint64_t)(esp_timer_get_time() / 1000000ULL));
    esp_mqtt_client_publish(s_mqtt.client, topic, value, 0, 1, 1);

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        build_topic(topic, sizeof(topic), "rssi");
        snprintf(value, sizeof(value), "%d", ap.rssi);
        esp_mqtt_client_publish(s_mqtt.client, topic, value, 0, 1, 1);
    }

    build_topic(topic, sizeof(topic), "last_error");
    esp_mqtt_client_publish(s_mqtt.client, topic, core->last_error, 0, 1, 1);

    build_topic(topic, sizeof(topic), "ota/state");
    esp_mqtt_client_publish(s_mqtt.client, topic, ota_update_status(), 0, 1, 1);

    build_topic(topic, sizeof(topic), "ota/error");
    esp_mqtt_client_publish(s_mqtt.client, topic, ota_update_last_error(), 0, 1, 1);
}

static void mqtt_subscribe_commands(void)
{
    char topic[128];
    build_topic(topic, sizeof(topic), "line/+/set");
    esp_mqtt_client_subscribe(s_mqtt.client, topic, 1);
    build_topic(topic, sizeof(topic), "line/+/duration/set");
    esp_mqtt_client_subscribe(s_mqtt.client, topic, 1);
    build_topic(topic, sizeof(topic), "line/+/schedule/enabled/set");
    esp_mqtt_client_subscribe(s_mqtt.client, topic, 1);
    build_topic(topic, sizeof(topic), "line/+/schedule/time/set");
    esp_mqtt_client_subscribe(s_mqtt.client, topic, 1);
    build_topic(topic, sizeof(topic), "line/+/schedule/weekdays/set");
    esp_mqtt_client_subscribe(s_mqtt.client, topic, 1);
    build_topic(topic, sizeof(topic), "line/+/schedule/day/+/set");
    esp_mqtt_client_subscribe(s_mqtt.client, topic, 1);
    build_topic(topic, sizeof(topic), "interlock/set");
    esp_mqtt_client_subscribe(s_mqtt.client, topic, 1);
    build_topic(topic, sizeof(topic), "all_off/set");
    esp_mqtt_client_subscribe(s_mqtt.client, topic, 1);
    build_topic(topic, sizeof(topic), "ota/url/set");
    esp_mqtt_client_subscribe(s_mqtt.client, topic, 1);
}

static void handle_mqtt_command(const char *topic, const char *data)
{
    char expected[128];
    build_topic(expected, sizeof(expected), "all_off/set");
    if (strcmp(topic, expected) == 0) {
        irrigation_event_t event = {
            .type = IRRIGATION_EVENT_ALL_OFF,
            .source = IRRIGATION_SOURCE_MQTT,
        };
        (void)xQueueSend(s_mqtt.queue, &event, pdMS_TO_TICKS(100));
        return;
    }

    build_topic(expected, sizeof(expected), "interlock/set");
    if (strcmp(topic, expected) == 0) {
        irrigation_event_t event = {
            .type = IRRIGATION_EVENT_SET_INTERLOCK,
            .source = IRRIGATION_SOURCE_MQTT,
            .duration_s = (strcasecmp(data, "ON") == 0 || strcmp(data, "1") == 0) ? 1 : 0,
        };
        (void)xQueueSend(s_mqtt.queue, &event, pdMS_TO_TICKS(100));
        return;
    }

    build_topic(expected, sizeof(expected), "ota/url/set");
    if (strcmp(topic, expected) == 0) {
        esp_err_t err = ota_update_start_url(data);
        if (err == ESP_OK) {
            char state_topic[128];
            build_topic(state_topic, sizeof(state_topic), "ota/url/state");
            esp_mqtt_client_publish(s_mqtt.client, state_topic, data, 0, 1, 1);
        } else {
            char error_topic[128];
            build_topic(error_topic, sizeof(error_topic), "ota/error");
            esp_mqtt_client_publish(s_mqtt.client, error_topic, esp_err_to_name(err), 0, 1, 1);
        }
        return;
    }

    for (uint8_t i = 0; i < s_mqtt.config.line_count; ++i) {
        snprintf(expected, sizeof(expected), "%s/%s/line/%u/set", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, i + 1);
        if (strcmp(topic, expected) == 0) {
            bool requested_on = strcasecmp(data, "ON") == 0 || strcmp(data, "1") == 0;
            if (requested_on && ota_update_is_in_progress()) {
                return;
            }
            irrigation_event_t event = {
                .source = IRRIGATION_SOURCE_MQTT,
                .line_id = i,
                .type = requested_on ? IRRIGATION_EVENT_COMMAND_ON : IRRIGATION_EVENT_COMMAND_OFF,
            };
            (void)xQueueSend(s_mqtt.queue, &event, pdMS_TO_TICKS(100));
            return;
        }
        snprintf(expected, sizeof(expected), "%s/%s/line/%u/duration/set", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, i + 1);
        if (strcmp(topic, expected) == 0) {
            long seconds = strtol(data, NULL, 10);
            if (seconds > 0 && seconds <= 86400) {
                irrigation_event_t event = {
                    .type = IRRIGATION_EVENT_SET_DURATION,
                    .source = IRRIGATION_SOURCE_MQTT,
                    .line_id = i,
                    .duration_s = (uint32_t)seconds,
                };
                (void)xQueueSend(s_mqtt.queue, &event, pdMS_TO_TICKS(100));
            }
            return;
        }
        snprintf(expected, sizeof(expected), "%s/%s/line/%u/schedule/enabled/set", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, i + 1);
        if (strcmp(topic, expected) == 0) {
            irrigation_event_t event = {
                .type = IRRIGATION_EVENT_SET_SCHEDULE_ENABLED,
                .source = IRRIGATION_SOURCE_MQTT,
                .line_id = i,
                .schedule_id = 0,
                .duration_s = (strcasecmp(data, "ON") == 0 || strcmp(data, "1") == 0) ? 1 : 0,
            };
            (void)xQueueSend(s_mqtt.queue, &event, pdMS_TO_TICKS(100));
            return;
        }
        snprintf(expected, sizeof(expected), "%s/%s/line/%u/schedule/time/set", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, i + 1);
        if (strcmp(topic, expected) == 0) {
            unsigned hour = 0;
            unsigned minute = 0;
            if (sscanf(data, "%u:%u", &hour, &minute) == 2 && hour < 24 && minute < 60) {
                irrigation_event_t event = {
                    .type = IRRIGATION_EVENT_SET_SCHEDULE_START_MINUTE,
                    .source = IRRIGATION_SOURCE_MQTT,
                    .line_id = i,
                    .schedule_id = 0,
                    .duration_s = (uint32_t)(hour * 60U + minute),
                };
                (void)xQueueSend(s_mqtt.queue, &event, pdMS_TO_TICKS(100));
            }
            return;
        }
        snprintf(expected, sizeof(expected), "%s/%s/line/%u/schedule/weekdays/set", s_mqtt.config.mqtt_prefix, s_mqtt.device_id, i + 1);
        if (strcmp(topic, expected) == 0) {
            long mask = strtol(data, NULL, 10);
            if (mask >= 0 && mask <= IRRIGATION_WEEKDAYS_ALL) {
                irrigation_event_t event = {
                    .type = IRRIGATION_EVENT_SET_SCHEDULE_WEEKDAYS,
                    .source = IRRIGATION_SOURCE_MQTT,
                    .line_id = i,
                    .schedule_id = 0,
                    .weekdays_mask = (uint8_t)mask,
                };
                (void)xQueueSend(s_mqtt.queue, &event, pdMS_TO_TICKS(100));
            }
            return;
        }
        for (size_t d = 0; d < sizeof(SCHEDULE_DAYS) / sizeof(SCHEDULE_DAYS[0]); ++d) {
            snprintf(expected,
                     sizeof(expected),
                     "%s/%s/line/%u/schedule/day/%s/set",
                     s_mqtt.config.mqtt_prefix,
                     s_mqtt.device_id,
                     i + 1,
                     SCHEDULE_DAYS[d].slug);
            if (strcmp(topic, expected) == 0) {
                irrigation_event_t event = {
                    .type = IRRIGATION_EVENT_SET_SCHEDULE_WEEKDAY,
                    .source = IRRIGATION_SOURCE_MQTT,
                    .line_id = i,
                    .schedule_id = 0,
                    .weekdays_mask = SCHEDULE_DAYS[d].bit,
                    .duration_s = (strcasecmp(data, "ON") == 0 || strcmp(data, "1") == 0) ? 1 : 0,
                };
                (void)xQueueSend(s_mqtt.queue, &event, pdMS_TO_TICKS(100));
                return;
            }
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    if (event_id == MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "MQTT connected");
        s_mqtt.connected = true;
        esp_mqtt_client_publish(s_mqtt.client, s_mqtt.availability_topic, "online", 0, 1, 1);
        mqtt_publish_discovery();
        mqtt_subscribe_commands();
    } else if (event_id == MQTT_EVENT_DATA) {
        char topic[160] = {0};
        char data[300] = {0};
        size_t topic_len = event->topic_len < (int)sizeof(topic) - 1 ? (size_t)event->topic_len : sizeof(topic) - 1;
        size_t data_len = event->data_len < (int)sizeof(data) - 1 ? (size_t)event->data_len : sizeof(data) - 1;
        memcpy(topic, event->topic, topic_len);
        memcpy(data, event->data, data_len);
        handle_mqtt_command(topic, data);
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "MQTT disconnected");
        s_mqtt.connected = false;
    }
}

esp_err_t mqtt_mode_start(const irrigation_config_t *config, QueueHandle_t controller_queue)
{
    if (config == NULL || controller_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(config->mqtt_uri) == 0) {
        ESP_LOGE(TAG, "MQTT mode requires mqtt_uri configured from the control panel");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_mqtt, 0, sizeof(s_mqtt));
    s_mqtt.config = *config;
    s_mqtt.queue = controller_queue;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_mqtt.device_id, sizeof(s_mqtt.device_id), "irrigation_%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_mqtt.availability_topic, sizeof(s_mqtt.availability_topic), "%s/%s/availability", s_mqtt.config.mqtt_prefix, s_mqtt.device_id);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_mqtt.config.mqtt_uri,
        .credentials.username = s_mqtt.config.mqtt_username,
        .credentials.authentication.password = s_mqtt.config.mqtt_password,
        .session.last_will.topic = s_mqtt.availability_topic,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
    };

    s_mqtt.client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt.client == NULL) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt.client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    return esp_mqtt_client_start(s_mqtt.client);
}
