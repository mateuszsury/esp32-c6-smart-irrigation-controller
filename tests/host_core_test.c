#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "irrigation_core.h"

typedef struct {
    bool state[IRRIGATION_MAX_LINES];
    int calls;
} fake_relays_t;

static int fake_relay_set(void *user_ctx, uint8_t line_id, bool on)
{
    fake_relays_t *relays = (fake_relays_t *)user_ctx;
    assert(line_id < IRRIGATION_MAX_LINES);
    relays->state[line_id] = on;
    relays->calls++;
    return 0;
}

static void test_interlock_turns_other_lines_off(void)
{
    irrigation_config_t config;
    irrigation_config_defaults(&config);
    config.interlock = true;

    fake_relays_t relays = {0};
    irrigation_core_t core;
    assert(irrigation_core_init(&core, &config, fake_relay_set, &relays) == IRRIGATION_OK);

    assert(irrigation_core_command_line(&core, 0, true, 10, 1000, IRRIGATION_SOURCE_UART) == IRRIGATION_OK);
    assert(irrigation_core_command_line(&core, 1, true, 10, 2000, IRRIGATION_SOURCE_UART) == IRRIGATION_OK);

    assert(!irrigation_core_line_is_on(&core, 0));
    assert(irrigation_core_line_is_on(&core, 1));
    assert(!relays.state[0]);
    assert(relays.state[1]);
}

static void test_timeout_turns_line_off(void)
{
    irrigation_config_t config;
    irrigation_config_defaults(&config);

    fake_relays_t relays = {0};
    irrigation_core_t core;
    assert(irrigation_core_init(&core, &config, fake_relay_set, &relays) == IRRIGATION_OK);

    assert(irrigation_core_command_line(&core, 0, true, 2, 1000, IRRIGATION_SOURCE_UART) == IRRIGATION_OK);
    irrigation_core_tick(&core, 2500);
    assert(irrigation_core_line_is_on(&core, 0));
    irrigation_core_tick(&core, 3000);
    assert(!irrigation_core_line_is_on(&core, 0));
}

static void test_disabled_line_is_rejected(void)
{
    irrigation_config_t config;
    irrigation_config_defaults(&config);
    config.lines[1].enabled = false;

    fake_relays_t relays = {0};
    irrigation_core_t core;
    assert(irrigation_core_init(&core, &config, fake_relay_set, &relays) == IRRIGATION_OK);

    assert(irrigation_core_command_line(&core, 1, true, 2, 1000, IRRIGATION_SOURCE_UART) == IRRIGATION_ERR_DISABLED_LINE);
    assert(!irrigation_core_line_is_on(&core, 1));
}

static void test_config_validation(void)
{
    irrigation_config_t config;
    char error[96] = {0};
    irrigation_config_defaults(&config);

    assert(irrigation_config_validate(&config, error, sizeof(error)) == IRRIGATION_OK);
    config.line_count = 9;
    assert(irrigation_config_validate(&config, error, sizeof(error)) == IRRIGATION_ERR_INVALID_CONFIG);
    assert(strstr(error, "line_count") != NULL);

    irrigation_config_defaults(&config);
    config.lines[1].gpio = config.lines[0].gpio;
    assert(irrigation_config_validate(&config, error, sizeof(error)) == IRRIGATION_ERR_INVALID_CONFIG);
    assert(strstr(error, "duplicates gpio") != NULL);

    irrigation_config_defaults(&config);
    config.lines[0].gpio = IRRIGATION_MODE_BUTTON_GPIO;
    assert(irrigation_config_validate(&config, error, sizeof(error)) == IRRIGATION_ERR_INVALID_CONFIG);
    assert(strstr(error, "mode button") != NULL);

    irrigation_config_defaults(&config);
    config.lines[0].gpio = 4;
    assert(irrigation_config_validate(&config, error, sizeof(error)) == IRRIGATION_ERR_INVALID_CONFIG);
    assert(strstr(error, "restricted relay gpio") != NULL);

    irrigation_config_defaults(&config);
    config.lines[0].schedule_count = 1;
    config.lines[0].schedules[0].weekdays_mask = 128;
    assert(irrigation_config_validate(&config, error, sizeof(error)) == IRRIGATION_ERR_INVALID_CONFIG);
    assert(strstr(error, "invalid weekdays") != NULL);
}

int main(void)
{
    test_interlock_turns_other_lines_off();
    test_timeout_turns_line_off();
    test_disabled_line_is_rejected();
    test_config_validation();
    puts("host_core_test: ok");
    return 0;
}
