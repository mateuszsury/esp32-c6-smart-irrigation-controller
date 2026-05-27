#include "relay_board.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "relay_board";
static irrigation_config_t s_config;
static bool s_initialized;

static int write_line(uint8_t line_id, bool on)
{
    if (!s_initialized || line_id >= s_config.line_count) {
        return 0;
    }

    const irrigation_line_config_t *line = &s_config.lines[line_id];
    int level = on ? (line->active_high ? 1 : 0) : (line->active_high ? 0 : 1);
    esp_err_t err = gpio_set_level((gpio_num_t)line->gpio, level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_level failed for line %u gpio %d: %s", line_id + 1, line->gpio, esp_err_to_name(err));
        return -1;
    }
    return 0;
}

int relay_board_init(const irrigation_config_t *config)
{
    if (config == NULL) {
        return -1;
    }

    s_config = *config;
    for (uint8_t i = 0; i < s_config.line_count; ++i) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << s_config.lines[i].gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&io_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "gpio_config failed for line %u gpio %d: %s", i + 1, s_config.lines[i].gpio, esp_err_to_name(err));
            return -1;
        }
    }

    s_initialized = true;
    return relay_board_all_off();
}

int relay_board_all_off(void)
{
    if (!s_initialized) {
        return 0;
    }

    for (uint8_t i = 0; i < s_config.line_count; ++i) {
        if (write_line(i, false) != 0) {
            return -1;
        }
    }
    ESP_LOGI(TAG, "all relay outputs forced OFF");
    return 0;
}

int relay_board_set_line(void *user_ctx, uint8_t line_id, bool on)
{
    (void)user_ctx;
    return write_line(line_id, on);
}
