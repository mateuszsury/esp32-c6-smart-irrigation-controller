#include "zigbee_mode.h"

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_zigbee.h"
#include "esp_log.h"
#include "ezbee/zha.h"
#include "ezbee/zcl/zcl_common.h"
#include "ezbee/zcl/cluster/basic_desc.h"
#include "ezbee/zcl/cluster/custom.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "zigbee_mode";

#define ZIGBEE_PRIMARY_CHANNEL_MASK 0x07FFF800U
#define IRRIGATION_ZCL_CLUSTER_ID 0xFC00U
#define IRRIGATION_ZCL_ATTR_DURATION_S_ID 0x0001U
#define IRRIGATION_ZCL_ATTR_INTERLOCK_ID 0x0002U
#define IRRIGATION_ZCL_ATTR_LINE_COUNT_ID 0x0003U
#define IRRIGATION_ZCL_ATTR_SCHEDULE_ENABLED_ID 0x0010U
#define IRRIGATION_ZCL_ATTR_SCHEDULE_START_MINUTE_ID 0x0011U
#define IRRIGATION_ZCL_ATTR_SCHEDULE_WEEKDAYS_ID 0x0012U

static const uint8_t s_manufacturer_name[] = {5, 'L', 'o', 'c', 'a', 'l'};
static const uint8_t s_model_identifier[] = {30, 'E', 'S', 'P', '3', '2', '-', 'C', '6', ' ', 'I', 'r', 'r', 'i', 'g', 'a', 't', 'i', 'o', 'n', ' ', 'C', 'o', 'n', 't', 'r', 'o', 'l', 'l', 'e', 'r'};
static const uint8_t s_sw_build_id[] = {3, '1', '.', '0'};

typedef struct {
    irrigation_config_t config;
    QueueHandle_t queue;
    uint32_t duration_attrs[IRRIGATION_MAX_LINES];
    bool schedule_enabled_attrs[IRRIGATION_MAX_LINES];
    uint16_t schedule_start_minute_attrs[IRRIGATION_MAX_LINES];
    uint8_t schedule_weekdays_attrs[IRRIGATION_MAX_LINES];
    bool interlock_attr;
    uint8_t line_count_attr;
    bool started;
} zigbee_mode_ctx_t;

static zigbee_mode_ctx_t s_zigbee;

static esp_err_t init_zigbee_storage_partition(void)
{
    esp_err_t err = nvs_flash_init_partition("zb_storage");
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Zigbee NVS partition needs erase: %s", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase_partition("zb_storage"), TAG, "erase zb_storage failed");
        err = nvs_flash_init_partition("zb_storage");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Zigbee NVS partition init failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void start_commissioning(ezb_bdb_comm_mode_mask_t mode)
{
    ezb_err_t err = ezb_bdb_start_top_level_commissioning(mode);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "BDB commissioning mode 0x%02x failed to start: %d", mode, err);
    }
}

static bool zigbee_signal_handler(const ezb_app_signal_t *signal)
{
    ezb_app_signal_type_t type = ezb_app_signal_get_type(signal);
    const void *params = ezb_app_signal_get_params(signal);
    const ezb_bdb_signal_simple_params_t *simple = (const ezb_bdb_signal_simple_params_t *)params;
    uint8_t status = simple != NULL ? simple->status : EZB_BDB_STATUS_SUCCESS;

    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee stack initialized, starting BDB initialization");
        start_commissioning(EZB_BDB_MODE_INITIALIZATION);
        return true;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT:
        ESP_LOGI(TAG, "Zigbee device startup signal status=%u", status);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            start_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        }
        return true;

    case EZB_BDB_SIGNAL_STEERING:
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG,
                     "Zigbee joined network: PAN=0x%04hx channel=%u short=0x%04hx",
                     ezb_get_panid(),
                     ezb_get_current_channel(),
                     ezb_get_short_address());
        } else {
            ESP_LOGW(TAG, "Zigbee steering failed status=%u, retrying", status);
            start_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        }
        return true;

    default:
        ESP_LOGI(TAG, "Zigbee signal %s (0x%04x)", ezb_app_signal_to_string(type), type);
        return false;
    }
}

static void zcl_set_attr_value_handler(ezb_zcl_set_attr_value_message_t *message)
{
    if (message == NULL) {
        return;
    }

    if (message->info.cluster_id == IRRIGATION_ZCL_CLUSTER_ID &&
        message->info.cluster_role == EZB_ZCL_CLUSTER_SERVER) {
        if (message->info.dst_ep == 0 || message->info.dst_ep > s_zigbee.config.line_count ||
            message->in.attribute.data.value == NULL) {
            message->out.result = EZB_ZCL_STATUS_UNSUP_ATTRIB;
            return;
        }

        switch (message->in.attribute.id) {
        case IRRIGATION_ZCL_ATTR_DURATION_S_ID:
            if (message->in.attribute.data.type != EZB_ZCL_ATTR_TYPE_UINT32) {
                message->out.result = EZB_ZCL_STATUS_INVALID_VALUE;
                return;
            }
            {
                uint32_t duration_s = *(uint32_t *)message->in.attribute.data.value;
                if (duration_s == 0 || duration_s > 86400U) {
                    message->out.result = EZB_ZCL_STATUS_INVALID_VALUE;
                    return;
                }
                uint8_t line_id = (uint8_t)(message->info.dst_ep - 1);
                irrigation_event_t event = {
                    .type = IRRIGATION_EVENT_SET_DURATION,
                    .source = IRRIGATION_SOURCE_ZIGBEE,
                    .line_id = line_id,
                    .duration_s = duration_s,
                };
                s_zigbee.duration_attrs[line_id] = duration_s;
                s_zigbee.config.lines[line_id].default_duration_s = duration_s;
                if (s_zigbee.config.lines[line_id].schedule_count == 0) {
                    s_zigbee.config.lines[line_id].schedule_count = 1;
                }
                s_zigbee.config.lines[line_id].schedules[0].duration_s = duration_s;
                (void)xQueueSend(s_zigbee.queue, &event, pdMS_TO_TICKS(100));
                ESP_LOGI(TAG, "Zigbee duration endpoint %u -> %" PRIu32 "s", message->info.dst_ep, duration_s);
                message->out.result = EZB_ZCL_STATUS_SUCCESS;
                return;
            }

        case IRRIGATION_ZCL_ATTR_INTERLOCK_ID:
            if (message->in.attribute.data.type != EZB_ZCL_ATTR_TYPE_BOOL) {
                message->out.result = EZB_ZCL_STATUS_INVALID_VALUE;
                return;
            }
            {
                bool interlock = *(bool *)message->in.attribute.data.value;
                irrigation_event_t event = {
                    .type = IRRIGATION_EVENT_SET_INTERLOCK,
                    .source = IRRIGATION_SOURCE_ZIGBEE,
                    .duration_s = interlock ? 1U : 0U,
                };
                s_zigbee.interlock_attr = interlock;
                s_zigbee.config.interlock = interlock;
                (void)xQueueSend(s_zigbee.queue, &event, pdMS_TO_TICKS(100));
                ESP_LOGI(TAG, "Zigbee interlock -> %s", interlock ? "true" : "false");
                message->out.result = EZB_ZCL_STATUS_SUCCESS;
                return;
            }

        case IRRIGATION_ZCL_ATTR_SCHEDULE_ENABLED_ID:
            if (message->in.attribute.data.type != EZB_ZCL_ATTR_TYPE_BOOL) {
                message->out.result = EZB_ZCL_STATUS_INVALID_VALUE;
                return;
            }
            {
                bool enabled = *(bool *)message->in.attribute.data.value;
                uint8_t line_id = (uint8_t)(message->info.dst_ep - 1);
                irrigation_event_t event = {
                    .type = IRRIGATION_EVENT_SET_SCHEDULE_ENABLED,
                    .source = IRRIGATION_SOURCE_ZIGBEE,
                    .line_id = line_id,
                    .schedule_id = 0,
                    .duration_s = enabled ? 1U : 0U,
                };
                s_zigbee.schedule_enabled_attrs[line_id] = enabled;
                (void)xQueueSend(s_zigbee.queue, &event, pdMS_TO_TICKS(100));
                ESP_LOGI(TAG, "Zigbee schedule enabled endpoint %u -> %s", message->info.dst_ep, enabled ? "true" : "false");
                message->out.result = EZB_ZCL_STATUS_SUCCESS;
                return;
            }

        case IRRIGATION_ZCL_ATTR_SCHEDULE_START_MINUTE_ID:
            if (message->in.attribute.data.type != EZB_ZCL_ATTR_TYPE_UINT16) {
                message->out.result = EZB_ZCL_STATUS_INVALID_VALUE;
                return;
            }
            {
                uint16_t start_minute = *(uint16_t *)message->in.attribute.data.value;
                if (start_minute >= 1440U) {
                    message->out.result = EZB_ZCL_STATUS_INVALID_VALUE;
                    return;
                }
                uint8_t line_id = (uint8_t)(message->info.dst_ep - 1);
                irrigation_event_t event = {
                    .type = IRRIGATION_EVENT_SET_SCHEDULE_START_MINUTE,
                    .source = IRRIGATION_SOURCE_ZIGBEE,
                    .line_id = line_id,
                    .schedule_id = 0,
                    .duration_s = start_minute,
                };
                s_zigbee.schedule_start_minute_attrs[line_id] = start_minute;
                (void)xQueueSend(s_zigbee.queue, &event, pdMS_TO_TICKS(100));
                ESP_LOGI(TAG, "Zigbee schedule start endpoint %u -> %u", message->info.dst_ep, start_minute);
                message->out.result = EZB_ZCL_STATUS_SUCCESS;
                return;
            }

        case IRRIGATION_ZCL_ATTR_SCHEDULE_WEEKDAYS_ID:
            if (message->in.attribute.data.type != EZB_ZCL_ATTR_TYPE_UINT8) {
                message->out.result = EZB_ZCL_STATUS_INVALID_VALUE;
                return;
            }
            {
                uint8_t weekdays = *(uint8_t *)message->in.attribute.data.value;
                if (weekdays > IRRIGATION_WEEKDAYS_ALL) {
                    message->out.result = EZB_ZCL_STATUS_INVALID_VALUE;
                    return;
                }
                uint8_t line_id = (uint8_t)(message->info.dst_ep - 1);
                irrigation_event_t event = {
                    .type = IRRIGATION_EVENT_SET_SCHEDULE_WEEKDAYS,
                    .source = IRRIGATION_SOURCE_ZIGBEE,
                    .line_id = line_id,
                    .schedule_id = 0,
                    .weekdays_mask = weekdays,
                };
                s_zigbee.schedule_weekdays_attrs[line_id] = weekdays;
                (void)xQueueSend(s_zigbee.queue, &event, pdMS_TO_TICKS(100));
                ESP_LOGI(TAG, "Zigbee schedule weekdays endpoint %u -> %u", message->info.dst_ep, weekdays);
                message->out.result = EZB_ZCL_STATUS_SUCCESS;
                return;
            }

        default:
            message->out.result = EZB_ZCL_STATUS_UNSUP_ATTRIB;
            return;
        }
    }

    if (message->info.status != EZB_ZCL_STATUS_SUCCESS ||
        message->info.cluster_id != EZB_ZCL_CLUSTER_ID_ON_OFF ||
        message->info.cluster_role != EZB_ZCL_CLUSTER_SERVER ||
        message->in.attribute.id != EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID ||
        message->in.attribute.data.type != EZB_ZCL_ATTR_TYPE_BOOL ||
        message->in.attribute.data.value == NULL) {
        message->out.result = EZB_ZCL_STATUS_UNSUP_ATTRIB;
        return;
    }

    if (message->info.dst_ep == 0 || message->info.dst_ep > s_zigbee.config.line_count) {
        message->out.result = EZB_ZCL_STATUS_UNSUP_ATTRIB;
        return;
    }

    bool on = *(bool *)message->in.attribute.data.value;
    irrigation_event_t event = {
        .type = on ? IRRIGATION_EVENT_COMMAND_ON : IRRIGATION_EVENT_COMMAND_OFF,
        .source = IRRIGATION_SOURCE_ZIGBEE,
        .line_id = (uint8_t)(message->info.dst_ep - 1),
    };
    (void)xQueueSend(s_zigbee.queue, &event, pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Zigbee On/Off endpoint %u -> %s", message->info.dst_ep, on ? "ON" : "OFF");
    message->out.result = EZB_ZCL_STATUS_SUCCESS;
}

static void zigbee_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID:
        zcl_set_attr_value_handler((ezb_zcl_set_attr_value_message_t *)message);
        break;
    default:
        ESP_LOGD(TAG, "Unhandled Zigbee ZCL action 0x%04" PRIx32, callback_id);
        break;
    }
}

static void add_basic_identity(ezb_af_ep_desc_t endpoint)
{
    ezb_zcl_cluster_desc_t basic = ezb_af_endpoint_get_cluster_desc(endpoint, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    if (basic == NULL) {
        ESP_LOGW(TAG, "basic cluster not found while setting identity");
        return;
    }
    (void)ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, s_manufacturer_name);
    (void)ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, s_model_identifier);
    (void)ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_SW_BUILD_ID_ID, s_sw_build_id);
}

static esp_err_t add_irrigation_cluster(ezb_af_ep_desc_t endpoint, uint8_t line_id)
{
    ezb_zcl_custom_cluster_config_t custom_cfg = {
        .cluster_id = IRRIGATION_ZCL_CLUSTER_ID,
    };
    ezb_zcl_cluster_desc_t cluster = ezb_zcl_custom_create_cluster_desc(&custom_cfg, EZB_ZCL_CLUSTER_SERVER);
    if (cluster == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_zigbee.duration_attrs[line_id] = s_zigbee.config.lines[line_id].default_duration_s;
    const irrigation_schedule_entry_t *schedule = s_zigbee.config.lines[line_id].schedule_count > 0 ?
        &s_zigbee.config.lines[line_id].schedules[0] : NULL;
    s_zigbee.schedule_enabled_attrs[line_id] = schedule != NULL && schedule->enabled;
    s_zigbee.schedule_start_minute_attrs[line_id] = schedule != NULL ? (uint16_t)(schedule->hour * 60U + schedule->minute) : 360U;
    s_zigbee.schedule_weekdays_attrs[line_id] = schedule != NULL ? schedule->weekdays_mask : IRRIGATION_WEEKDAYS_ALL;
    ezb_err_t err = ezb_zcl_custom_cluster_desc_add_manuf_attr(
        cluster,
        IRRIGATION_ZCL_ATTR_DURATION_S_ID,
        EZB_ZCL_ATTR_TYPE_UINT32,
        EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE | EZB_ZCL_ATTR_ACCESS_REPORTING,
        EZB_ZCL_ESP_MANUF_CODE,
        &s_zigbee.duration_attrs[line_id]);
    if (err != EZB_ERR_NONE) {
        return esp_zigbee_err_to_esp(err);
    }

    err = ezb_zcl_custom_cluster_desc_add_manuf_attr(
        cluster,
        IRRIGATION_ZCL_ATTR_INTERLOCK_ID,
        EZB_ZCL_ATTR_TYPE_BOOL,
        EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE | EZB_ZCL_ATTR_ACCESS_REPORTING,
        EZB_ZCL_ESP_MANUF_CODE,
        &s_zigbee.interlock_attr);
    if (err != EZB_ERR_NONE) {
        return esp_zigbee_err_to_esp(err);
    }

    err = ezb_zcl_custom_cluster_desc_add_manuf_attr(
        cluster,
        IRRIGATION_ZCL_ATTR_LINE_COUNT_ID,
        EZB_ZCL_ATTR_TYPE_UINT8,
        EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING,
        EZB_ZCL_ESP_MANUF_CODE,
        &s_zigbee.line_count_attr);
    if (err != EZB_ERR_NONE) {
        return esp_zigbee_err_to_esp(err);
    }

    err = ezb_zcl_custom_cluster_desc_add_manuf_attr(
        cluster,
        IRRIGATION_ZCL_ATTR_SCHEDULE_ENABLED_ID,
        EZB_ZCL_ATTR_TYPE_BOOL,
        EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE | EZB_ZCL_ATTR_ACCESS_REPORTING,
        EZB_ZCL_ESP_MANUF_CODE,
        &s_zigbee.schedule_enabled_attrs[line_id]);
    if (err != EZB_ERR_NONE) {
        return esp_zigbee_err_to_esp(err);
    }

    err = ezb_zcl_custom_cluster_desc_add_manuf_attr(
        cluster,
        IRRIGATION_ZCL_ATTR_SCHEDULE_START_MINUTE_ID,
        EZB_ZCL_ATTR_TYPE_UINT16,
        EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE | EZB_ZCL_ATTR_ACCESS_REPORTING,
        EZB_ZCL_ESP_MANUF_CODE,
        &s_zigbee.schedule_start_minute_attrs[line_id]);
    if (err != EZB_ERR_NONE) {
        return esp_zigbee_err_to_esp(err);
    }

    err = ezb_zcl_custom_cluster_desc_add_manuf_attr(
        cluster,
        IRRIGATION_ZCL_ATTR_SCHEDULE_WEEKDAYS_ID,
        EZB_ZCL_ATTR_TYPE_UINT8,
        EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE | EZB_ZCL_ATTR_ACCESS_REPORTING,
        EZB_ZCL_ESP_MANUF_CODE,
        &s_zigbee.schedule_weekdays_attrs[line_id]);
    if (err != EZB_ERR_NONE) {
        return esp_zigbee_err_to_esp(err);
    }

    err = ezb_af_endpoint_add_cluster_desc(endpoint, cluster);
    return err == EZB_ERR_NONE ? ESP_OK : esp_zigbee_err_to_esp(err);
}

static esp_err_t register_data_model(void)
{
    ezb_af_device_desc_t device = ezb_af_create_device_desc();
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (uint8_t i = 0; i < s_zigbee.config.line_count; ++i) {
        ezb_zha_on_off_light_config_t light_cfg = EZB_ZHA_ON_OFF_LIGHT_CONFIG();
        light_cfg.on_off_cfg.on_off = false;

        ezb_af_ep_desc_t endpoint = ezb_zha_create_on_off_light(i + 1, &light_cfg);
        if (endpoint == NULL) {
            return ESP_ERR_NO_MEM;
        }
        add_basic_identity(endpoint);

        esp_err_t custom_err = add_irrigation_cluster(endpoint, i);
        if (custom_err != ESP_OK) {
            return custom_err;
        }

        ezb_err_t err = ezb_af_device_add_endpoint_desc(device, endpoint);
        if (err != EZB_ERR_NONE) {
            return esp_zigbee_err_to_esp(err);
        }
        ESP_LOGI(TAG, "registered Zigbee endpoint %u for %s", i + 1, s_zigbee.config.lines[i].name);
    }

    ezb_err_t err = ezb_af_device_desc_register(device);
    if (err != EZB_ERR_NONE) {
        return esp_zigbee_err_to_esp(err);
    }
    ezb_zcl_core_action_handler_register(zigbee_action_handler);
    return ESP_OK;
}

static void zigbee_start_failed(const char *step, esp_err_t err, bool initialized)
{
    ESP_LOGE(TAG, "Zigbee start failed at %s: %s", step, esp_err_to_name(err));
    if (s_zigbee.queue != NULL) {
        irrigation_event_t event = {
            .type = IRRIGATION_EVENT_COMM_LOST,
            .source = IRRIGATION_SOURCE_SYSTEM,
        };
        (void)xQueueSend(s_zigbee.queue, &event, pdMS_TO_TICKS(100));
    }
    if (initialized) {
        esp_zigbee_deinit();
    }
    vTaskDelete(NULL);
}

static void zigbee_main_task(void *arg)
{
    (void)arg;
    esp_err_t err = ESP_OK;
    bool initialized = false;

    esp_zigbee_config_t config = {
        .device_config = {
            .device_type = EZB_NWK_DEVICE_TYPE_ROUTER,
            .install_code_policy = false,
            .zczr_config = {
                .max_children = 10,
            },
        },
        .platform_config = {
            .storage_partition_name = "zb_storage",
            .radio_config = {
                .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,
            },
        },
    };

    err = init_zigbee_storage_partition();
    if (err != ESP_OK) {
        zigbee_start_failed("storage init", err, initialized);
    }

    err = esp_zigbee_init(&config);
    if (err != ESP_OK) {
        zigbee_start_failed("stack init", err, initialized);
    }
    initialized = true;

    err = esp_zigbee_err_to_esp(ezb_bdb_set_primary_channel_set(ZIGBEE_PRIMARY_CHANNEL_MASK));
    if (err != ESP_OK) {
        zigbee_start_failed("channel mask", err, initialized);
    }

    err = register_data_model();
    if (err != ESP_OK) {
        zigbee_start_failed("data model", err, initialized);
    }

    err = esp_zigbee_err_to_esp(ezb_app_signal_add_handler(zigbee_signal_handler));
    if (err != ESP_OK) {
        zigbee_start_failed("signal handler", err, initialized);
    }

    err = esp_zigbee_start(false);
    if (err != ESP_OK) {
        zigbee_start_failed("stack start", err, initialized);
    }

    ESP_LOGI(TAG, "Zigbee router started with %u line endpoint(s)", s_zigbee.config.line_count);
    s_zigbee.started = true;
    err = esp_zigbee_launch_mainloop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Zigbee mainloop stopped: %s", esp_err_to_name(err));
    }
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

esp_err_t zigbee_mode_start(const irrigation_config_t *config, QueueHandle_t controller_queue)
{
    if (config == NULL || controller_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_zigbee, 0, sizeof(s_zigbee));
    s_zigbee.config = *config;
    s_zigbee.queue = controller_queue;
    s_zigbee.interlock_attr = config->interlock;
    s_zigbee.line_count_attr = config->line_count;

    if (xTaskCreate(zigbee_main_task, "zigbee_main", 8192, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void zigbee_mode_publish_state(const irrigation_core_t *core)
{
    if (core == NULL || !s_zigbee.started) {
        return;
    }

    s_zigbee.interlock_attr = core->config.interlock;
    s_zigbee.line_count_attr = core->config.line_count;

    for (uint8_t i = 0; i < core->config.line_count; ++i) {
        uint8_t ep = i + 1;
        bool on = irrigation_core_line_is_on(core, i);
        s_zigbee.duration_attrs[i] = core->config.lines[i].default_duration_s;
        const irrigation_schedule_entry_t *schedule = core->config.lines[i].schedule_count > 0 ?
            &core->config.lines[i].schedules[0] : NULL;
        s_zigbee.schedule_enabled_attrs[i] = schedule != NULL && schedule->enabled;
        s_zigbee.schedule_start_minute_attrs[i] = schedule != NULL ? (uint16_t)(schedule->hour * 60U + schedule->minute) : 360U;
        s_zigbee.schedule_weekdays_attrs[i] = schedule != NULL ? schedule->weekdays_mask : IRRIGATION_WEEKDAYS_ALL;

        (void)ezb_zcl_set_attr_value(
            ep,
            EZB_ZCL_CLUSTER_ID_ON_OFF,
            EZB_ZCL_CLUSTER_SERVER,
            EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
            EZB_ZCL_STD_MANUF_CODE,
            &on,
            false);

        (void)ezb_zcl_set_attr_value(
            ep,
            IRRIGATION_ZCL_CLUSTER_ID,
            EZB_ZCL_CLUSTER_SERVER,
            IRRIGATION_ZCL_ATTR_DURATION_S_ID,
            EZB_ZCL_ESP_MANUF_CODE,
            &s_zigbee.duration_attrs[i],
            false);

        (void)ezb_zcl_set_attr_value(
            ep,
            IRRIGATION_ZCL_CLUSTER_ID,
            EZB_ZCL_CLUSTER_SERVER,
            IRRIGATION_ZCL_ATTR_INTERLOCK_ID,
            EZB_ZCL_ESP_MANUF_CODE,
            &s_zigbee.interlock_attr,
            false);

        (void)ezb_zcl_set_attr_value(
            ep,
            IRRIGATION_ZCL_CLUSTER_ID,
            EZB_ZCL_CLUSTER_SERVER,
            IRRIGATION_ZCL_ATTR_LINE_COUNT_ID,
            EZB_ZCL_ESP_MANUF_CODE,
            &s_zigbee.line_count_attr,
            false);

        (void)ezb_zcl_set_attr_value(
            ep,
            IRRIGATION_ZCL_CLUSTER_ID,
            EZB_ZCL_CLUSTER_SERVER,
            IRRIGATION_ZCL_ATTR_SCHEDULE_ENABLED_ID,
            EZB_ZCL_ESP_MANUF_CODE,
            &s_zigbee.schedule_enabled_attrs[i],
            false);

        (void)ezb_zcl_set_attr_value(
            ep,
            IRRIGATION_ZCL_CLUSTER_ID,
            EZB_ZCL_CLUSTER_SERVER,
            IRRIGATION_ZCL_ATTR_SCHEDULE_START_MINUTE_ID,
            EZB_ZCL_ESP_MANUF_CODE,
            &s_zigbee.schedule_start_minute_attrs[i],
            false);

        (void)ezb_zcl_set_attr_value(
            ep,
            IRRIGATION_ZCL_CLUSTER_ID,
            EZB_ZCL_CLUSTER_SERVER,
            IRRIGATION_ZCL_ATTR_SCHEDULE_WEEKDAYS_ID,
            EZB_ZCL_ESP_MANUF_CODE,
            &s_zigbee.schedule_weekdays_attrs[i],
            false);
    }
}
