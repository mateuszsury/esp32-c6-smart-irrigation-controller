#include "config_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CONFIG_NAMESPACE "irrigation"
#define CONFIG_BLOB_KEY "cfg"
#define CONFIG_MAGIC 0x49525247u
#define LEGACY_LINE3_RELAY_GPIO 4
#define LINE3_INDEX 2

static const char *TAG = "config_store";

typedef struct {
    uint32_t magic;
    uint32_t crc;
    irrigation_config_t config;
} stored_config_t;

typedef struct {
    uint32_t schema_version;
    irrigation_comm_mode_t mode;
    bool interlock;
    char wifi_ssid[33];
    char wifi_password[65];
    char mqtt_uri[128];
    char mqtt_username[65];
    char mqtt_password[65];
    char mqtt_prefix[48];
    uint8_t line_count;
    irrigation_line_config_t lines[IRRIGATION_MAX_LINES];
} irrigation_config_v3_t;

typedef struct {
    uint32_t magic;
    uint32_t crc;
    irrigation_config_v3_t config;
} stored_config_v3_t;

static uint32_t fnv1a32(const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static bool migrate_config(irrigation_config_t *config)
{
    bool migrated = false;

    for (uint8_t i = 0; i < config->line_count; ++i) {
        if (config->lines[i].active_high != IRRIGATION_DEFAULT_RELAY_ACTIVE_HIGH) {
            config->lines[i].active_high = IRRIGATION_DEFAULT_RELAY_ACTIVE_HIGH;
            migrated = true;
        }
    }

    if (config->line_count > LINE3_INDEX && config->lines[LINE3_INDEX].gpio == LEGACY_LINE3_RELAY_GPIO) {
        config->lines[LINE3_INDEX].gpio = IRRIGATION_DEFAULT_RELAY_GPIOS[LINE3_INDEX];
        migrated = true;
    }

    return migrated;
}

static void import_v3_config(irrigation_config_t *dst, const irrigation_config_v3_t *src)
{
    irrigation_config_defaults(dst);
    dst->mode = src->mode;
    dst->interlock = src->interlock;
    dst->zigbee_router_with_mqtt = IRRIGATION_DEFAULT_ZIGBEE_ROUTER_WITH_MQTT;
    memcpy(dst->wifi_ssid, src->wifi_ssid, sizeof(dst->wifi_ssid));
    memcpy(dst->wifi_password, src->wifi_password, sizeof(dst->wifi_password));
    memcpy(dst->mqtt_uri, src->mqtt_uri, sizeof(dst->mqtt_uri));
    memcpy(dst->mqtt_username, src->mqtt_username, sizeof(dst->mqtt_username));
    memcpy(dst->mqtt_password, src->mqtt_password, sizeof(dst->mqtt_password));
    memcpy(dst->mqtt_prefix, src->mqtt_prefix, sizeof(dst->mqtt_prefix));
    dst->line_count = src->line_count;
    memcpy(dst->lines, src->lines, sizeof(dst->lines));
}

esp_err_t config_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t config_store_load(irrigation_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    irrigation_config_defaults(config);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "NVS config missing, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t required = 0;
    err = nvs_get_blob(handle, CONFIG_BLOB_KEY, NULL, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        ESP_LOGW(TAG, "NVS blob missing, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        ESP_LOGE(TAG, "NVS blob read failed: %s", esp_err_to_name(err));
        return err;
    }

    char validation_error[96] = {0};

    if (required != sizeof(stored_config_t)) {
        nvs_close(handle);
        ESP_LOGE(TAG, "NVS blob has unexpected size %u, using defaults", (unsigned)required);
        irrigation_config_defaults(config);
        return ESP_ERR_INVALID_SIZE;
    }

    stored_config_t stored = {0};
    err = nvs_get_blob(handle, CONFIG_BLOB_KEY, &stored, &required);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS blob read failed: %s", esp_err_to_name(err));
        return err;
    }

    if (stored.config.schema_version == 3U) {
        stored_config_v3_t stored_v3 = {0};
        memcpy(&stored_v3, &stored, sizeof(stored_v3));
        uint32_t crc = fnv1a32(&stored_v3.config, sizeof(stored_v3.config));
        if (stored_v3.magic != CONFIG_MAGIC || stored_v3.crc != crc) {
            ESP_LOGE(TAG, "legacy stored config invalid, using defaults");
            irrigation_config_defaults(config);
            return ESP_ERR_INVALID_CRC;
        }
        import_v3_config(config, &stored_v3.config);
        bool migrated = migrate_config(config);
        if (irrigation_config_validate(config, validation_error, sizeof(validation_error)) != IRRIGATION_OK) {
            ESP_LOGE(TAG, "legacy stored config invalid after migration (%s), using defaults", validation_error);
            irrigation_config_defaults(config);
            return ESP_ERR_INVALID_CRC;
        }
        ESP_LOGW(TAG, "migrated NVS config schema 3 -> 4; zigbee_router_with_mqtt=%s",
                 config->zigbee_router_with_mqtt ? "true" : "false");
        esp_err_t save_err = config_store_save(config);
        if (save_err != ESP_OK) {
            ESP_LOGE(TAG, "failed to persist schema migration: %s", esp_err_to_name(save_err));
        }
        ESP_LOGI(TAG, "loaded config: mode=%s lines=%u interlock=%s",
                 irrigation_mode_to_string(config->mode),
                 config->line_count,
                 config->interlock ? "true" : "false");
        (void)migrated;
        return ESP_OK;
    }

    if (stored.config.schema_version == 4U) {
        *config = stored.config;
        config->schema_version = 5U;
        config->zigbee_router_with_mqtt = IRRIGATION_DEFAULT_ZIGBEE_ROUTER_WITH_MQTT;
        bool migrated = migrate_config(config);
        if (irrigation_config_validate(config, validation_error, sizeof(validation_error)) != IRRIGATION_OK) {
            ESP_LOGE(TAG, "stored schema 4 config invalid after migration (%s), using defaults", validation_error);
            irrigation_config_defaults(config);
            return ESP_ERR_INVALID_CRC;
        }
        ESP_LOGW(TAG, "migrated NVS config schema 4 -> 5; zigbee_router_with_mqtt=%s",
                 config->zigbee_router_with_mqtt ? "true" : "false");
        esp_err_t save_err = config_store_save(config);
        if (save_err != ESP_OK) {
            ESP_LOGE(TAG, "failed to persist schema migration: %s", esp_err_to_name(save_err));
        }
        ESP_LOGI(TAG, "loaded config: mode=%s lines=%u interlock=%s",
                 irrigation_mode_to_string(config->mode),
                 config->line_count,
                 config->interlock ? "true" : "false");
        (void)migrated;
        return ESP_OK;
    }

    uint32_t crc = fnv1a32(&stored.config, sizeof(stored.config));
    if (stored.magic != CONFIG_MAGIC || stored.crc != crc) {
        ESP_LOGE(TAG, "stored config invalid (%s), using defaults", validation_error[0] ? validation_error : "crc/magic");
        irrigation_config_defaults(config);
        return ESP_ERR_INVALID_CRC;
    }

    *config = stored.config;
    bool migrated = migrate_config(config);
    if (irrigation_config_validate(config, validation_error, sizeof(validation_error)) != IRRIGATION_OK) {
        ESP_LOGE(TAG, "stored config invalid (%s), using defaults", validation_error);
        irrigation_config_defaults(config);
        return ESP_ERR_INVALID_CRC;
    }
    if (migrated) {
        ESP_LOGW(TAG, "migrated board config: relay active_high=%s line3_gpio=%d",
                 IRRIGATION_DEFAULT_RELAY_ACTIVE_HIGH ? "true" : "false",
                 IRRIGATION_DEFAULT_RELAY_GPIOS[LINE3_INDEX]);
        esp_err_t save_err = config_store_save(config);
        if (save_err != ESP_OK) {
            ESP_LOGE(TAG, "failed to persist board config migration: %s", esp_err_to_name(save_err));
        }
    }
    ESP_LOGI(TAG, "loaded config: mode=%s lines=%u interlock=%s",
             irrigation_mode_to_string(config->mode),
             config->line_count,
             config->interlock ? "true" : "false");
    return ESP_OK;
}

esp_err_t config_store_save(const irrigation_config_t *config)
{
    char validation_error[96] = {0};
    if (irrigation_config_validate(config, validation_error, sizeof(validation_error)) != IRRIGATION_OK) {
        ESP_LOGE(TAG, "refusing invalid config: %s", validation_error);
        return ESP_ERR_INVALID_ARG;
    }

    stored_config_t stored = {
        .magic = CONFIG_MAGIC,
        .config = *config,
    };
    stored.crc = fnv1a32(&stored.config, sizeof(stored.config));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, CONFIG_BLOB_KEY, &stored, sizeof(stored));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t config_store_factory_reset(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t config_store_save_mode(irrigation_comm_mode_t mode)
{
    irrigation_config_t config;
    esp_err_t err = config_store_load(&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_CRC) {
        irrigation_config_defaults(&config);
    }
    config.mode = mode;
    return config_store_save(&config);
}
