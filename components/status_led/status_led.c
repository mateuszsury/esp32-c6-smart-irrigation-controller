#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#define STATUS_LED_RESOLUTION_HZ (10 * 1000 * 1000)
#define STATUS_LED_PERIOD_MS 2200U
#define STATUS_LED_STEP_MS 50U

static const char *TAG = "status_led";

static led_strip_handle_t s_strip;
static volatile status_led_pattern_t s_pattern = STATUS_LED_PATTERN_OFF;
static bool s_started;

static uint8_t pulse_brightness(uint32_t tick_ms)
{
    uint32_t phase = tick_ms % STATUS_LED_PERIOD_MS;
    uint32_t half = STATUS_LED_PERIOD_MS / 2U;
    uint32_t rising = phase <= half ? phase : (STATUS_LED_PERIOD_MS - phase);
    uint32_t value = 5U + ((rising * 35U) / half);
    return value > 40U ? 40U : (uint8_t)value;
}

static void scale_color(uint8_t base_r, uint8_t base_g, uint8_t base_b, uint8_t brightness, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = (uint8_t)(((uint32_t)base_r * brightness) / 255U);
    *g = (uint8_t)(((uint32_t)base_g * brightness) / 255U);
    *b = (uint8_t)(((uint32_t)base_b * brightness) / 255U);
}

static void set_pixel(uint8_t red, uint8_t green, uint8_t blue)
{
    if (s_strip == NULL) {
        return;
    }
    if (led_strip_set_pixel(s_strip, 0, red, green, blue) == ESP_OK) {
        (void)led_strip_refresh(s_strip);
    }
}

static void status_led_task(void *arg)
{
    (void)arg;
    uint32_t tick_ms = 0;
    status_led_pattern_t previous = STATUS_LED_PATTERN_OFF;

    while (true) {
        status_led_pattern_t pattern = s_pattern;
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;

        if (pattern != previous) {
            tick_ms = 0;
            previous = pattern;
        }

        switch (pattern) {
        case STATUS_LED_PATTERN_MQTT_BLUE:
            scale_color(0, 0, 255, pulse_brightness(tick_ms), &r, &g, &b);
            break;
        case STATUS_LED_PATTERN_ZIGBEE_GREEN:
            scale_color(0, 255, 0, pulse_brightness(tick_ms), &r, &g, &b);
            break;
        case STATUS_LED_PATTERN_PORTAL_YELLOW:
            scale_color(255, 180, 0, pulse_brightness(tick_ms), &r, &g, &b);
            break;
        case STATUS_LED_PATTERN_CONNECTING_YELLOW:
            scale_color(255, 180, 0, 32, &r, &g, &b);
            break;
        case STATUS_LED_PATTERN_OFF:
        default:
            break;
        }

        set_pixel(r, g, b);
        tick_ms += STATUS_LED_STEP_MS;
        vTaskDelay(pdMS_TO_TICKS(STATUS_LED_STEP_MS));
    }
}

esp_err_t status_led_start(void)
{
    if (!IRRIGATION_STATUS_LED_ENABLED || s_started) {
        return ESP_OK;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = IRRIGATION_STATUS_LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = STATUS_LED_RESOLUTION_HZ,
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "status LED init failed on GPIO%d: %s", IRRIGATION_STATUS_LED_GPIO, esp_err_to_name(err));
        return err;
    }

    (void)led_strip_clear(s_strip);
    s_started = true;
    if (xTaskCreate(status_led_task, "status_led", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "status LED task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "status LED enabled on GPIO%d", IRRIGATION_STATUS_LED_GPIO);
    return ESP_OK;
}

void status_led_set_pattern(status_led_pattern_t pattern)
{
    s_pattern = pattern;
}
