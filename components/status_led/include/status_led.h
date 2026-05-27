#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATUS_LED_PATTERN_OFF = 0,
    STATUS_LED_PATTERN_MQTT_BLUE,
    STATUS_LED_PATTERN_ZIGBEE_GREEN,
    STATUS_LED_PATTERN_PORTAL_YELLOW,
    STATUS_LED_PATTERN_CONNECTING_YELLOW,
} status_led_pattern_t;

esp_err_t status_led_start(void);
void status_led_set_pattern(status_led_pattern_t pattern);

#ifdef __cplusplus
}
#endif
