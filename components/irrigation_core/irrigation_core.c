#include "irrigation_core.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define IRRIGATION_SCHEMA_VERSION 5

static void set_error(irrigation_core_t *core, const char *message)
{
    if (core == NULL || message == NULL) {
        return;
    }
    snprintf(core->last_error, sizeof(core->last_error), "%s", message);
}

static bool relay_gpio_is_restricted(int gpio)
{
    switch (gpio) {
        case 4:  // strapping/JTAG
        case 5:  // strapping/JTAG
        case 6:  // JTAG
        case 7:  // JTAG
        case 8:  // strapping
        case 9:  // strapping, mode button
        case 12: // USB Serial/JTAG
        case 13: // USB Serial/JTAG
        case 15: // strapping
        case 16: // UART0
        case 17: // UART0
        case 24: // SPI flash/PSRAM
        case 25: // SPI flash/PSRAM
        case 26: // SPI flash/PSRAM
        case 27: // SPI flash/PSRAM/VDD_SPI
        case 28: // SPI flash/PSRAM
        case 29: // SPI flash/PSRAM
        case 30: // SPI flash/PSRAM
            return true;
        default:
            return false;
    }
}

const char *irrigation_mode_to_string(irrigation_comm_mode_t mode)
{
    return mode == IRRIGATION_MODE_ZIGBEE ? "zigbee" : "mqtt";
}

irrigation_comm_mode_t irrigation_mode_toggle(irrigation_comm_mode_t mode)
{
    return mode == IRRIGATION_MODE_ZIGBEE ? IRRIGATION_MODE_MQTT : IRRIGATION_MODE_ZIGBEE;
}

void irrigation_config_defaults(irrigation_config_t *config)
{
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->schema_version = IRRIGATION_SCHEMA_VERSION;
    config->mode = IRRIGATION_MODE_MQTT;
    config->interlock = IRRIGATION_DEFAULT_INTERLOCK;
    config->zigbee_router_with_mqtt = IRRIGATION_DEFAULT_ZIGBEE_ROUTER_WITH_MQTT;
    snprintf(config->mqtt_prefix, sizeof(config->mqtt_prefix), "irrigation");
    config->line_count = IRRIGATION_DEFAULT_LINE_COUNT;

    for (uint8_t i = 0; i < config->line_count; ++i) {
        irrigation_line_config_t *line = &config->lines[i];
        snprintf(line->name, sizeof(line->name), "%s", IRRIGATION_DEFAULT_LINE_NAMES[i]);
        line->gpio = IRRIGATION_DEFAULT_RELAY_GPIOS[i];
        line->active_high = IRRIGATION_DEFAULT_RELAY_ACTIVE_HIGH;
        line->enabled = true;
        line->default_duration_s = IRRIGATION_DEFAULT_DURATION_SECONDS;
    }
}

irrigation_result_t irrigation_config_validate(const irrigation_config_t *config, char *error, size_t error_len)
{
    if (config == NULL) {
        if (error != NULL && error_len > 0) {
            snprintf(error, error_len, "config is null");
        }
        return IRRIGATION_ERR_INVALID_ARG;
    }

    if (config->schema_version != IRRIGATION_SCHEMA_VERSION) {
        if (error != NULL && error_len > 0) {
            snprintf(error, error_len, "unsupported schema version %" PRIu32, config->schema_version);
        }
        return IRRIGATION_ERR_INVALID_CONFIG;
    }

    if (config->line_count == 0 || config->line_count > IRRIGATION_MAX_LINES) {
        if (error != NULL && error_len > 0) {
            snprintf(error, error_len, "line_count must be 1..%u", IRRIGATION_MAX_LINES);
        }
        return IRRIGATION_ERR_INVALID_CONFIG;
    }

    if (config->mode != IRRIGATION_MODE_MQTT && config->mode != IRRIGATION_MODE_ZIGBEE) {
        if (error != NULL && error_len > 0) {
            snprintf(error, error_len, "invalid communication mode");
        }
        return IRRIGATION_ERR_INVALID_CONFIG;
    }

    if (config->mqtt_prefix[0] == '\0') {
        if (error != NULL && error_len > 0) {
            snprintf(error, error_len, "mqtt_prefix must not be empty");
        }
        return IRRIGATION_ERR_INVALID_CONFIG;
    }

    for (uint8_t i = 0; i < config->line_count; ++i) {
        const irrigation_line_config_t *line = &config->lines[i];
        if (line->gpio < 0 || line->gpio > 30) {
            if (error != NULL && error_len > 0) {
                snprintf(error, error_len, "line %u has invalid gpio", i + 1);
            }
            return IRRIGATION_ERR_INVALID_CONFIG;
        }
        if (line->gpio == IRRIGATION_MODE_BUTTON_GPIO) {
            if (error != NULL && error_len > 0) {
                snprintf(error, error_len, "line %u gpio conflicts with mode button", i + 1);
            }
            return IRRIGATION_ERR_INVALID_CONFIG;
        }
        if (relay_gpio_is_restricted(line->gpio)) {
            if (error != NULL && error_len > 0) {
                snprintf(error, error_len, "line %u uses restricted relay gpio", i + 1);
            }
            return IRRIGATION_ERR_INVALID_CONFIG;
        }
        for (uint8_t k = 0; k < i; ++k) {
            if (config->lines[k].gpio == line->gpio) {
                if (error != NULL && error_len > 0) {
                    snprintf(error, error_len, "line %u duplicates gpio from line %u", i + 1, k + 1);
                }
                return IRRIGATION_ERR_INVALID_CONFIG;
            }
        }
        if (line->default_duration_s == 0) {
            if (error != NULL && error_len > 0) {
                snprintf(error, error_len, "line %u duration must be > 0", i + 1);
            }
            return IRRIGATION_ERR_INVALID_CONFIG;
        }
        if (line->schedule_count > IRRIGATION_MAX_SCHEDULES_PER_LINE) {
            if (error != NULL && error_len > 0) {
                snprintf(error, error_len, "line %u has too many schedules", i + 1);
            }
            return IRRIGATION_ERR_INVALID_CONFIG;
        }
        for (uint8_t j = 0; j < line->schedule_count; ++j) {
            const irrigation_schedule_entry_t *entry = &line->schedules[j];
            if (entry->hour > 23 || entry->minute > 59) {
                if (error != NULL && error_len > 0) {
                    snprintf(error, error_len, "line %u schedule %u has invalid time", i + 1, j + 1);
                }
                return IRRIGATION_ERR_INVALID_CONFIG;
            }
            if (entry->weekdays_mask > IRRIGATION_WEEKDAYS_ALL) {
                if (error != NULL && error_len > 0) {
                    snprintf(error, error_len, "line %u schedule %u has invalid weekdays", i + 1, j + 1);
                }
                return IRRIGATION_ERR_INVALID_CONFIG;
            }
            if (entry->enabled && entry->duration_s == 0) {
                if (error != NULL && error_len > 0) {
                    snprintf(error, error_len, "line %u schedule %u duration must be > 0", i + 1, j + 1);
                }
                return IRRIGATION_ERR_INVALID_CONFIG;
            }
        }
    }

    return IRRIGATION_OK;
}

static irrigation_result_t set_line(irrigation_core_t *core, uint8_t line_id, bool on)
{
    if (core->relay_set != NULL && core->relay_set(core->relay_user_ctx, line_id, on) != 0) {
        set_error(core, "relay callback failed");
        return IRRIGATION_ERR_RELAY;
    }
    core->lines[line_id].on = on;
    if (!on) {
        core->lines[line_id].deadline_ms = 0;
    }
    return IRRIGATION_OK;
}

irrigation_result_t irrigation_core_init(
    irrigation_core_t *core,
    const irrigation_config_t *config,
    irrigation_relay_set_fn_t relay_set,
    void *relay_user_ctx)
{
    if (core == NULL || config == NULL) {
        return IRRIGATION_ERR_INVALID_ARG;
    }

    memset(core, 0, sizeof(*core));
    core->relay_set = relay_set;
    core->relay_user_ctx = relay_user_ctx;

    return irrigation_core_apply_config(core, config);
}

irrigation_result_t irrigation_core_apply_config(irrigation_core_t *core, const irrigation_config_t *config)
{
    char error[96] = {0};
    if (core == NULL || config == NULL) {
        return IRRIGATION_ERR_INVALID_ARG;
    }

    irrigation_result_t result = irrigation_config_validate(config, error, sizeof(error));
    if (result != IRRIGATION_OK) {
        set_error(core, error);
        return result;
    }

    irrigation_core_all_off(core);
    memcpy(&core->config, config, sizeof(core->config));
    set_error(core, "ok");
    return IRRIGATION_OK;
}

irrigation_result_t irrigation_core_command_line(
    irrigation_core_t *core,
    uint8_t line_id,
    bool on,
    uint32_t duration_s,
    uint64_t now_ms,
    irrigation_command_source_t source)
{
    if (core == NULL || line_id >= core->config.line_count) {
        return IRRIGATION_ERR_INVALID_ARG;
    }

    if (!core->config.lines[line_id].enabled) {
        set_error(core, "line disabled");
        return IRRIGATION_ERR_DISABLED_LINE;
    }

    if (!on) {
        return set_line(core, line_id, false);
    }

    if (core->config.interlock) {
        for (uint8_t i = 0; i < core->config.line_count; ++i) {
            if (i != line_id && core->lines[i].on) {
                irrigation_result_t off_result = set_line(core, i, false);
                if (off_result != IRRIGATION_OK) {
                    return off_result;
                }
            }
        }
    }

    uint32_t effective_duration = duration_s > 0 ? duration_s : core->config.lines[line_id].default_duration_s;
    irrigation_result_t on_result = set_line(core, line_id, true);
    if (on_result != IRRIGATION_OK) {
        return on_result;
    }

    core->lines[line_id].deadline_ms = now_ms + ((uint64_t)effective_duration * 1000ULL);
    core->lines[line_id].last_source = source;
    set_error(core, "ok");
    return IRRIGATION_OK;
}

irrigation_result_t irrigation_core_all_off(irrigation_core_t *core)
{
    if (core == NULL) {
        return IRRIGATION_ERR_INVALID_ARG;
    }

    for (uint8_t i = 0; i < IRRIGATION_MAX_LINES; ++i) {
        irrigation_result_t result = set_line(core, i, false);
        if (result != IRRIGATION_OK) {
            return result;
        }
    }
    set_error(core, "ok");
    return IRRIGATION_OK;
}

void irrigation_core_tick(irrigation_core_t *core, uint64_t now_ms)
{
    if (core == NULL) {
        return;
    }

    for (uint8_t i = 0; i < core->config.line_count; ++i) {
        if (core->lines[i].on && core->lines[i].deadline_ms > 0 && now_ms >= core->lines[i].deadline_ms) {
            (void)set_line(core, i, false);
        }
    }
}

bool irrigation_core_line_is_on(const irrigation_core_t *core, uint8_t line_id)
{
    if (core == NULL || line_id >= core->config.line_count) {
        return false;
    }
    return core->lines[line_id].on;
}
