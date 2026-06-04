#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IRRIGATION_MODE_MQTT = 0,
    IRRIGATION_MODE_ZIGBEE = 1,
} irrigation_comm_mode_t;

typedef enum {
    IRRIGATION_SOURCE_MQTT = 0,
    IRRIGATION_SOURCE_ZIGBEE,
    IRRIGATION_SOURCE_SCHEDULE,
    IRRIGATION_SOURCE_UART,
    IRRIGATION_SOURCE_SYSTEM,
} irrigation_command_source_t;

typedef enum {
    IRRIGATION_EVENT_COMMAND_ON = 0,
    IRRIGATION_EVENT_COMMAND_OFF,
    IRRIGATION_EVENT_SCHEDULE_START,
    IRRIGATION_EVENT_TIMEOUT,
    IRRIGATION_EVENT_CONFIG_UPDATE,
    IRRIGATION_EVENT_COMM_LOST,
    IRRIGATION_EVENT_ALL_OFF,
    IRRIGATION_EVENT_SET_DURATION,
    IRRIGATION_EVENT_SET_INTERLOCK,
    IRRIGATION_EVENT_SET_SCHEDULE_ENABLED,
    IRRIGATION_EVENT_SET_SCHEDULE_START_MINUTE,
    IRRIGATION_EVENT_SET_SCHEDULE_WEEKDAYS,
    IRRIGATION_EVENT_SET_SCHEDULE_WEEKDAY,
} irrigation_event_type_t;

typedef enum {
    IRRIGATION_OK = 0,
    IRRIGATION_ERR_INVALID_ARG = -1,
    IRRIGATION_ERR_INVALID_CONFIG = -2,
    IRRIGATION_ERR_DISABLED_LINE = -3,
    IRRIGATION_ERR_RELAY = -4,
    IRRIGATION_ERR_INVALID_STATE = -5,
} irrigation_result_t;

typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t weekdays_mask;
    uint32_t duration_s;
    bool enabled;
} irrigation_schedule_entry_t;

typedef struct {
    char name[32];
    int gpio;
    bool active_high;
    bool enabled;
    uint32_t default_duration_s;
    uint8_t schedule_count;
    irrigation_schedule_entry_t schedules[IRRIGATION_MAX_SCHEDULES_PER_LINE];
} irrigation_line_config_t;

typedef struct {
    uint32_t schema_version;
    irrigation_comm_mode_t mode;
    bool interlock;
    bool zigbee_router_with_mqtt;
    char wifi_ssid[33];
    char wifi_password[65];
    char mqtt_uri[128];
    char mqtt_username[65];
    char mqtt_password[65];
    char mqtt_prefix[48];
    char mqtt_device_id[32];
    uint8_t line_count;
    irrigation_line_config_t lines[IRRIGATION_MAX_LINES];
} irrigation_config_t;

typedef struct {
    irrigation_event_type_t type;
    irrigation_command_source_t source;
    uint8_t line_id;
    uint8_t schedule_id;
    uint8_t weekdays_mask;
    uint32_t duration_s;
} irrigation_event_t;

typedef int (*irrigation_relay_set_fn_t)(void *user_ctx, uint8_t line_id, bool on);

#define IRRIGATION_WEEKDAY_SUN 0x01u
#define IRRIGATION_WEEKDAY_MON 0x02u
#define IRRIGATION_WEEKDAY_TUE 0x04u
#define IRRIGATION_WEEKDAY_WED 0x08u
#define IRRIGATION_WEEKDAY_THU 0x10u
#define IRRIGATION_WEEKDAY_FRI 0x20u
#define IRRIGATION_WEEKDAY_SAT 0x40u
#define IRRIGATION_WEEKDAYS_ALL 0x7Fu

typedef struct {
    bool on;
    uint64_t deadline_ms;
    irrigation_command_source_t last_source;
} irrigation_line_state_t;

typedef struct {
    irrigation_config_t config;
    irrigation_line_state_t lines[IRRIGATION_MAX_LINES];
    irrigation_relay_set_fn_t relay_set;
    void *relay_user_ctx;
    char last_error[96];
} irrigation_core_t;

void irrigation_config_defaults(irrigation_config_t *config);
irrigation_result_t irrigation_config_validate(const irrigation_config_t *config, char *error, size_t error_len);
const char *irrigation_mode_to_string(irrigation_comm_mode_t mode);
irrigation_comm_mode_t irrigation_mode_toggle(irrigation_comm_mode_t mode);

irrigation_result_t irrigation_core_init(
    irrigation_core_t *core,
    const irrigation_config_t *config,
    irrigation_relay_set_fn_t relay_set,
    void *relay_user_ctx);

irrigation_result_t irrigation_core_apply_config(irrigation_core_t *core, const irrigation_config_t *config);
irrigation_result_t irrigation_core_command_line(
    irrigation_core_t *core,
    uint8_t line_id,
    bool on,
    uint32_t duration_s,
    uint64_t now_ms,
    irrigation_command_source_t source);
irrigation_result_t irrigation_core_all_off(irrigation_core_t *core);
void irrigation_core_tick(irrigation_core_t *core, uint64_t now_ms);
bool irrigation_core_line_is_on(const irrigation_core_t *core, uint8_t line_id);

#ifdef __cplusplus
}
#endif
