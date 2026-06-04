#include "control_panel.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "config_store.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "ota_update.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define PORTAL_AP_IP "192.168.4.1"

static const char *TAG = "control_panel";

typedef struct {
    const char *key;
    const char *label;
    uint8_t bit;
} panel_weekday_t;

static const panel_weekday_t PANEL_WEEKDAYS[] = {
    {"mon", "Mon", IRRIGATION_WEEKDAY_MON},
    {"tue", "Tue", IRRIGATION_WEEKDAY_TUE},
    {"wed", "Wed", IRRIGATION_WEEKDAY_WED},
    {"thu", "Thu", IRRIGATION_WEEKDAY_THU},
    {"fri", "Fri", IRRIGATION_WEEKDAY_FRI},
    {"sat", "Sat", IRRIGATION_WEEKDAY_SAT},
    {"sun", "Sun", IRRIGATION_WEEKDAY_SUN},
};

typedef struct {
    irrigation_core_t *core;
    QueueHandle_t queue;
    httpd_handle_t server;
    EventGroupHandle_t wifi_events;
    esp_netif_t *sta_netif;
    esp_netif_t *ap_netif;
    bool wifi_started;
    bool ap_started;
    bool event_loop_ready;
    int retry_count;
    char ap_ssid[33];
} control_panel_ctx_t;

static control_panel_ctx_t s_panel;

static void url_decode(char *s)
{
    char *src = s;
    char *dst = s;
    while (*src) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = {src[1], src[2], 0};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static void copy_trunc(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= dst_len) {
        len = dst_len - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static bool form_value(const char *body, const char *key, char *out, size_t out_len)
{
    if (body == NULL || key == NULL || out == NULL || out_len == 0) {
        return false;
    }
    char needle[40];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(body, needle);
    if (p == NULL) {
        return false;
    }
    p += strlen(needle);
    const char *end = strchr(p, '&');
    size_t len = end == NULL ? strlen(p) : (size_t)(end - p);
    if (len >= out_len) {
        len = out_len - 1;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    url_decode(out);
    return true;
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t len)
{
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t remaining = req->content_len;
    size_t offset = 0;
    while (remaining > 0 && offset + 1 < len) {
        int received = httpd_req_recv(req, buf + offset, (remaining < len - offset - 1) ? remaining : len - offset - 1);
        if (received <= 0) {
            return ESP_FAIL;
        }
        offset += (size_t)received;
        remaining -= (size_t)received;
    }
    buf[offset] = '\0';
    return ESP_OK;
}

static void send_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "", 0);
}

static void enqueue_line(uint8_t line_id, bool on, irrigation_command_source_t source)
{
    irrigation_event_t event = {
        .type = on ? IRRIGATION_EVENT_COMMAND_ON : IRRIGATION_EVENT_COMMAND_OFF,
        .source = source,
        .line_id = line_id,
    };
    (void)xQueueSend(s_panel.queue, &event, pdMS_TO_TICKS(100));
}

static void send_plain_error(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, message);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const irrigation_core_t *core = s_panel.core;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Irrigation Controller</title><style>"
        "body{font-family:system-ui,Segoe UI,Arial,sans-serif;margin:0;background:#f5f7f9;color:#182026}"
        "main{max-width:960px;margin:auto;padding:20px}.bar{display:flex;justify-content:space-between;gap:16px;align-items:center}"
        "section{background:#fff;border:1px solid #d9e1e8;border-radius:8px;padding:16px;margin:14px 0}"
        "h1{font-size:24px;margin:0}h2{font-size:17px;margin:0 0 12px}label{display:block;font-size:13px;margin:8px 0 4px}"
        "input,select{width:100%;box-sizing:border-box;padding:9px;border:1px solid #b8c4ce;border-radius:6px}"
        "button{padding:9px 12px;border:0;border-radius:6px;background:#1769aa;color:#fff;cursor:pointer}.off{background:#5f6b76}.danger{background:#b33a3a}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}.line{border:1px solid #d9e1e8;border-radius:8px;padding:12px}"
        ".muted{color:#607080;font-size:13px}.row{display:flex;gap:8px;flex-wrap:wrap}.row form{display:inline}"
        ".days{display:grid;grid-template-columns:repeat(auto-fit,minmax(70px,1fr));gap:6px;margin-top:4px}"
        ".days label{display:flex;align-items:center;gap:5px;margin:0;font-size:13px}.days input{width:auto}</style></head><body><main>");

    char chunk[1400];
    snprintf(chunk, sizeof(chunk),
             "<div class='bar'><div><h1>ESP32-C6 Irrigation</h1><div class='muted'>Firmware %s</div></div>"
             "<div class='muted'>Mode: %s<br>Wi-Fi: %s</div></div>",
             IRRIGATION_FIRMWARE_VERSION,
             irrigation_mode_to_string(core->config.mode),
             control_panel_wifi_connected() ? "connected" : "portal");
    httpd_resp_sendstr_chunk(req, chunk);

    httpd_resp_sendstr_chunk(req, "<section><h2>Lines</h2><div class='grid'>");
    for (uint8_t i = 0; i < core->config.line_count; ++i) {
        const irrigation_line_config_t *line = &core->config.lines[i];
        snprintf(chunk, sizeof(chunk),
                 "<div class='line'><b>%s</b><div class='muted'>GPIO %d, %s, %lus</div>"
                 "<div class='muted'>State: %s</div><div class='row'>"
                 "<form method='post' action='/line'><input type='hidden' name='id' value='%u'><input type='hidden' name='state' value='on'><button>ON</button></form>"
                 "<form method='post' action='/line'><input type='hidden' name='id' value='%u'><input type='hidden' name='state' value='off'><button class='off'>OFF</button></form>"
                 "</div></div>",
                 line->name,
                 line->gpio,
                 line->enabled ? "enabled" : "disabled",
                 (unsigned long)line->default_duration_s,
                 irrigation_core_line_is_on(core, i) ? "ON" : "OFF",
                 i,
                 i);
        httpd_resp_sendstr_chunk(req, chunk);
    }
    httpd_resp_sendstr_chunk(req, "</div><p><form method='post' action='/all-off'><button class='danger'>All OFF</button></form></p></section>");

    snprintf(chunk, sizeof(chunk),
             "<section><h2>OTA update</h2><div class='muted'>Status: %s<br>Last error: %s</div>"
             "<form method='post' action='/ota/url'><label>Firmware URL</label>"
             "<input name='url' placeholder='http://your-server.local/irrigation_controller.bin'>"
             "<p><button>Update from URL</button></p></form>"
             "<label>Firmware .bin upload</label><input id='ota_file' type='file' accept='.bin,application/octet-stream'>"
             "<p><button type='button' onclick='otaUpload()'>Upload binary</button></p>"
             "<pre id='ota_result' class='muted'></pre>"
             "<script>async function otaUpload(){const f=document.getElementById('ota_file').files[0];"
             "if(!f){document.getElementById('ota_result').textContent='select .bin first';return;}"
             "const r=await fetch('/ota/upload',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});"
             "document.getElementById('ota_result').textContent=await r.text();}</script></section>",
             ota_update_status(),
             ota_update_last_error());
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
             "<section><h2>Network and MQTT</h2><form method='post' action='/network'>"
             "<div class='grid'><div><label>Wi-Fi SSID</label><input name='ssid' value='%s'></div>"
             "<div><label>Wi-Fi password</label><input name='password' type='password' placeholder='leave blank to keep'></div>"
             "<div><label>MQTT URI</label><input name='mqtt_uri' value='%s' placeholder='mqtt://192.168.1.10:1883'></div>"
             "<div><label>MQTT username</label><input name='mqtt_username' value='%s'></div>"
             "<div><label>MQTT password</label><input name='mqtt_password' type='password' placeholder='leave blank to keep'></div>"
             "<div><label>MQTT prefix</label><input name='mqtt_prefix' value='%s'></div>"
             "<div><label>MQTT device ID</label><input name='mqtt_device_id' value='%s' placeholder='auto from MAC'></div>"
             "</div><p><button>Save and reboot</button></p></form></section>",
             core->config.wifi_ssid,
             core->config.mqtt_uri,
             core->config.mqtt_username,
             core->config.mqtt_prefix,
             core->config.mqtt_device_id);
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
             "<section><h2>Mode and safety</h2><form method='post' action='/mode'><label>Communication mode</label>"
             "<select name='mode'><option value='mqtt'%s>MQTT</option><option value='zigbee'%s>Zigbee</option></select>"
             "<label><input type='checkbox' name='interlock' value='1' %s style='width:auto'> Interlock</label>"
             "<label><input type='checkbox' name='zigbee_router_with_mqtt' value='1' %s style='width:auto'> Zigbee router active also in MQTT mode</label>"
             "<p><button>Save and reboot</button></p></form></section>",
             core->config.mode == IRRIGATION_MODE_MQTT ? " selected" : "",
             core->config.mode == IRRIGATION_MODE_ZIGBEE ? " selected" : "",
             core->config.interlock ? "checked" : "",
             core->config.zigbee_router_with_mqtt ? "checked" : "");
    httpd_resp_sendstr_chunk(req, chunk);

    httpd_resp_sendstr_chunk(req,
        "<section><h2>Line configuration</h2><form method='post' action='/lines'>"
        "<div class='grid'><div><label>Line count 1..8</label><input name='line_count' type='number' min='1' max='8' value='");
    snprintf(chunk, sizeof(chunk), "%u", core->config.line_count);
    httpd_resp_sendstr_chunk(req, chunk);
    httpd_resp_sendstr_chunk(req, "'></div></div>");
    for (uint8_t i = 0; i < IRRIGATION_MAX_LINES; ++i) {
        const irrigation_line_config_t *line = &core->config.lines[i];
        snprintf(chunk, sizeof(chunk),
                 "<div class='grid'><div><label>Line %u name</label><input name='name%u' value='%s'></div>"
                 "<div><label>Line %u GPIO</label><input name='gpio%u' type='number' min='0' max='30' value='%d'></div>"
                 "<div><label>Duration s</label><input name='duration%u' type='number' min='1' max='86400' value='%" PRIu32 "'></div></div>",
                 i + 1,
                 i,
                 line->name[0] ? line->name : "",
                 i + 1,
                 i,
                 line->gpio,
                 i,
                 line->default_duration_s == 0 ? IRRIGATION_DEFAULT_DURATION_SECONDS : line->default_duration_s);
        httpd_resp_sendstr_chunk(req, chunk);
        if (i < core->config.line_count) {
            httpd_resp_sendstr_chunk(req, "<div class='grid'>");
            for (uint8_t j = 0; j < IRRIGATION_MAX_SCHEDULES_PER_LINE; ++j) {
                const irrigation_schedule_entry_t *entry = &line->schedules[j];
                snprintf(chunk,
                         sizeof(chunk),
                         "<div class='line'><b>Line %u schedule %u</b>"
                         "<label><input type='checkbox' name='sch_e_%u_%u' value='1' %s style='width:auto'> enabled</label>"
                         "<label>Hour</label><input name='sch_h_%u_%u' type='number' min='0' max='23' value='%u'>"
                         "<label>Minute</label><input name='sch_m_%u_%u' type='number' min='0' max='59' value='%u'>"
                         "<label>Weekdays</label><div class='days'>",
                         i + 1,
                         j + 1,
                         i,
                         j,
                         entry->enabled ? "checked" : "",
                         i,
                         j,
                         entry->hour,
                         i,
                         j,
                         entry->minute);
                httpd_resp_sendstr_chunk(req, chunk);
                uint8_t weekdays_mask = entry->weekdays_mask <= IRRIGATION_WEEKDAYS_ALL ? entry->weekdays_mask : IRRIGATION_WEEKDAYS_ALL;
                for (size_t d = 0; d < sizeof(PANEL_WEEKDAYS) / sizeof(PANEL_WEEKDAYS[0]); ++d) {
                    snprintf(chunk,
                             sizeof(chunk),
                             "<label><input type='checkbox' name='sch_w_%u_%u_%s' value='1' %s>%s</label>",
                             i,
                             j,
                             PANEL_WEEKDAYS[d].key,
                             (weekdays_mask & PANEL_WEEKDAYS[d].bit) != 0 ? "checked" : "",
                             PANEL_WEEKDAYS[d].label);
                    httpd_resp_sendstr_chunk(req, chunk);
                }
                snprintf(chunk,
                         sizeof(chunk),
                         "</div><label>Duration s</label><input name='sch_d_%u_%u' type='number' min='1' max='86400' value='%" PRIu32 "'></div>",
                         i,
                         j,
                         entry->duration_s == 0 ? line->default_duration_s : entry->duration_s);
                httpd_resp_sendstr_chunk(req, chunk);
            }
            httpd_resp_sendstr_chunk(req, "</div>");
        }
    }
    httpd_resp_sendstr_chunk(req, "<p><button>Save and reboot</button></p></form></section></main></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    const irrigation_core_t *core = s_panel.core;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mode", irrigation_mode_to_string(core->config.mode));
    cJSON_AddBoolToObject(root, "wifi_connected", control_panel_wifi_connected());
    cJSON_AddBoolToObject(root, "ap_active", control_panel_ap_active());
    cJSON_AddBoolToObject(root, "interlock", core->config.interlock);
    cJSON_AddBoolToObject(root, "zigbee_router_with_mqtt", core->config.zigbee_router_with_mqtt);
    cJSON_AddStringToObject(root, "mqtt_device_id", core->config.mqtt_device_id);
    cJSON_AddStringToObject(root, "last_error", core->last_error);
    cJSON *ota = cJSON_AddObjectToObject(root, "ota");
    cJSON_AddStringToObject(ota, "status", ota_update_status());
    cJSON_AddStringToObject(ota, "last_error", ota_update_last_error());
    cJSON_AddBoolToObject(ota, "in_progress", ota_update_is_in_progress());
    cJSON *lines = cJSON_AddArrayToObject(root, "lines");
    for (uint8_t i = 0; i < core->config.line_count; ++i) {
        cJSON *line = cJSON_CreateObject();
        cJSON_AddNumberToObject(line, "id", i);
        cJSON_AddStringToObject(line, "name", core->config.lines[i].name);
        cJSON_AddNumberToObject(line, "gpio", core->config.lines[i].gpio);
        cJSON_AddBoolToObject(line, "on", irrigation_core_line_is_on(core, i));
        cJSON_AddItemToArray(lines, line);
    }
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t line_post_handler(httpd_req_t *req)
{
    char body[160];
    char value[32];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }
    if (!form_value(body, "id", value, sizeof(value))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing id");
        return ESP_OK;
    }
    int id = atoi(value);
    bool on = form_value(body, "state", value, sizeof(value)) && strcmp(value, "on") == 0;
    if (on && ota_update_is_in_progress()) {
        send_plain_error(req, "409 Conflict", "OTA in progress");
        return ESP_OK;
    }
    if (id < 0 || id >= s_panel.core->config.line_count) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad id");
        return ESP_OK;
    }
    enqueue_line((uint8_t)id, on, IRRIGATION_SOURCE_SYSTEM);
    send_redirect(req);
    return ESP_OK;
}

static esp_err_t ota_url_post_handler(httpd_req_t *req)
{
    char body[320];
    char url[256];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }
    if (!form_value(body, "url", url, sizeof(url)) || url[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing url");
        return ESP_OK;
    }
    esp_err_t err = ota_update_start_url(url);
    if (err != ESP_OK) {
        send_plain_error(req, "409 Conflict", esp_err_to_name(err));
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "OTA from URL started. Device will reboot after a valid image is written.\n");
    return ESP_OK;
}

static esp_err_t ota_upload_post_handler(httpd_req_t *req)
{
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty upload");
        return ESP_OK;
    }

    ota_update_upload_handle_t handle = NULL;
    esp_err_t err = ota_update_upload_begin(req->content_len, &handle);
    if (err != ESP_OK) {
        send_plain_error(req, "409 Conflict", esp_err_to_name(err));
        return ESP_OK;
    }

    char *buf = malloc(2048);
    if (buf == NULL) {
        ota_update_upload_abort(handle);
        return ESP_ERR_NO_MEM;
    }

    size_t remaining = req->content_len;
    while (remaining > 0) {
        size_t to_read = remaining > 2048 ? 2048 : remaining;
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            free(buf);
            ota_update_upload_abort(handle);
            return ESP_FAIL;
        }
        err = ota_update_upload_write(handle, buf, (size_t)received);
        if (err != ESP_OK) {
            free(buf);
            ota_update_upload_abort(handle);
            send_plain_error(req, "400 Bad Request", esp_err_to_name(err));
            return ESP_OK;
        }
        remaining -= (size_t)received;
    }
    free(buf);

    err = ota_update_upload_finish(handle);
    if (err != ESP_OK) {
        send_plain_error(req, "400 Bad Request", esp_err_to_name(err));
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "OTA image accepted. Rebooting.\n");
    vTaskDelay(pdMS_TO_TICKS(750));
    esp_restart();
    return ESP_OK;
}

static esp_err_t all_off_post_handler(httpd_req_t *req)
{
    irrigation_event_t event = {
        .type = IRRIGATION_EVENT_ALL_OFF,
        .source = IRRIGATION_SOURCE_SYSTEM,
    };
    (void)xQueueSend(s_panel.queue, &event, pdMS_TO_TICKS(100));
    send_redirect(req);
    return ESP_OK;
}

static esp_err_t network_post_handler(httpd_req_t *req)
{
    char body[768];
    char value[160];
    irrigation_config_t *config = calloc(1, sizeof(*config));
    if (config == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *config = s_panel.core->config;
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        free(config);
        return ESP_FAIL;
    }
    if (form_value(body, "ssid", value, sizeof(value))) {
        copy_trunc(config->wifi_ssid, sizeof(config->wifi_ssid), value);
    }
    if (form_value(body, "password", value, sizeof(value)) && value[0] != '\0') {
        copy_trunc(config->wifi_password, sizeof(config->wifi_password), value);
    }
    if (form_value(body, "mqtt_uri", value, sizeof(value))) {
        copy_trunc(config->mqtt_uri, sizeof(config->mqtt_uri), value);
    }
    if (form_value(body, "mqtt_username", value, sizeof(value))) {
        copy_trunc(config->mqtt_username, sizeof(config->mqtt_username), value);
    }
    if (form_value(body, "mqtt_password", value, sizeof(value)) && value[0] != '\0') {
        copy_trunc(config->mqtt_password, sizeof(config->mqtt_password), value);
    }
    if (form_value(body, "mqtt_prefix", value, sizeof(value)) && value[0] != '\0') {
        copy_trunc(config->mqtt_prefix, sizeof(config->mqtt_prefix), value);
    }
    if (form_value(body, "mqtt_device_id", value, sizeof(value))) {
        copy_trunc(config->mqtt_device_id, sizeof(config->mqtt_device_id), value);
    }
    esp_err_t save_err = config_store_save(config);
    free(config);
    ESP_ERROR_CHECK(save_err);
    send_redirect(req);
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
    return ESP_OK;
}

static esp_err_t mode_post_handler(httpd_req_t *req)
{
    char body[256];
    char value[64];
    irrigation_config_t *config = calloc(1, sizeof(*config));
    if (config == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *config = s_panel.core->config;
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        free(config);
        return ESP_FAIL;
    }
    if (form_value(body, "mode", value, sizeof(value))) {
        config->mode = strcmp(value, "zigbee") == 0 ? IRRIGATION_MODE_ZIGBEE : IRRIGATION_MODE_MQTT;
    }
    config->interlock = form_value(body, "interlock", value, sizeof(value));
    config->zigbee_router_with_mqtt = form_value(body, "zigbee_router_with_mqtt", value, sizeof(value));
    esp_err_t save_err = config_store_save(config);
    free(config);
    ESP_ERROR_CHECK(save_err);
    send_redirect(req);
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
    return ESP_OK;
}

static esp_err_t lines_post_handler(httpd_req_t *req)
{
    char key[32];
    char value[96];
    irrigation_config_t *config = calloc(1, sizeof(*config));
    if (config == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *config = s_panel.core->config;
    if (req->content_len > 8192) {
        free(config);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form too large");
        return ESP_OK;
    }
    char *body = calloc(1, req->content_len + 1);
    if (body == NULL) {
        free(config);
        return ESP_ERR_NO_MEM;
    }
    if (read_body(req, body, req->content_len + 1) != ESP_OK) {
        free(body);
        free(config);
        return ESP_FAIL;
    }
    if (form_value(body, "line_count", value, sizeof(value))) {
        int count = atoi(value);
        if (count >= 1 && count <= IRRIGATION_MAX_LINES) {
            config->line_count = (uint8_t)count;
        }
    }
    for (uint8_t i = 0; i < IRRIGATION_MAX_LINES; ++i) {
        snprintf(key, sizeof(key), "name%u", i);
        if (form_value(body, key, value, sizeof(value)) && value[0] != '\0') {
            copy_trunc(config->lines[i].name, sizeof(config->lines[i].name), value);
        } else if (config->lines[i].name[0] == '\0') {
            snprintf(config->lines[i].name, sizeof(config->lines[i].name), "line_%u", i + 1);
        }
        snprintf(key, sizeof(key), "gpio%u", i);
        if (form_value(body, key, value, sizeof(value))) {
            config->lines[i].gpio = atoi(value);
        }
        snprintf(key, sizeof(key), "duration%u", i);
        if (form_value(body, key, value, sizeof(value))) {
            long duration = strtol(value, NULL, 10);
            config->lines[i].default_duration_s = duration > 0 ? (uint32_t)duration : IRRIGATION_DEFAULT_DURATION_SECONDS;
        }
        config->lines[i].enabled = true;
        config->lines[i].active_high = IRRIGATION_DEFAULT_RELAY_ACTIVE_HIGH;
        config->lines[i].schedule_count = 0;
        for (uint8_t j = 0; j < IRRIGATION_MAX_SCHEDULES_PER_LINE; ++j) {
            irrigation_schedule_entry_t *entry = &config->lines[i].schedules[j];
            memset(entry, 0, sizeof(*entry));

            snprintf(key, sizeof(key), "sch_e_%u_%u", i, j);
            bool enabled = form_value(body, key, value, sizeof(value));

            snprintf(key, sizeof(key), "sch_h_%u_%u", i, j);
            if (form_value(body, key, value, sizeof(value))) {
                entry->hour = (uint8_t)atoi(value);
            }
            snprintf(key, sizeof(key), "sch_m_%u_%u", i, j);
            if (form_value(body, key, value, sizeof(value))) {
                entry->minute = (uint8_t)atoi(value);
            }
            entry->weekdays_mask = 0;
            for (size_t d = 0; d < sizeof(PANEL_WEEKDAYS) / sizeof(PANEL_WEEKDAYS[0]); ++d) {
                snprintf(key, sizeof(key), "sch_w_%u_%u_%s", i, j, PANEL_WEEKDAYS[d].key);
                if (form_value(body, key, value, sizeof(value))) {
                    entry->weekdays_mask |= PANEL_WEEKDAYS[d].bit;
                }
            }
            snprintf(key, sizeof(key), "sch_d_%u_%u", i, j);
            if (form_value(body, key, value, sizeof(value))) {
                long duration = strtol(value, NULL, 10);
                entry->duration_s = duration > 0 ? (uint32_t)duration : config->lines[i].default_duration_s;
            } else {
                entry->duration_s = config->lines[i].default_duration_s;
            }
            entry->enabled = enabled;
            if (enabled || entry->hour != 0 || entry->minute != 0) {
                config->lines[i].schedule_count = j + 1;
            }
        }
    }
    esp_err_t save_err = config_store_save(config);
    free(body);
    free(config);
    ESP_ERROR_CHECK(save_err);
    send_redirect(req);
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
    return ESP_OK;
}

static void register_handlers(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler},
        {.uri = "/line", .method = HTTP_POST, .handler = line_post_handler},
        {.uri = "/all-off", .method = HTTP_POST, .handler = all_off_post_handler},
        {.uri = "/ota/url", .method = HTTP_POST, .handler = ota_url_post_handler},
        {.uri = "/ota/upload", .method = HTTP_POST, .handler = ota_upload_post_handler},
        {.uri = "/network", .method = HTTP_POST, .handler = network_post_handler},
        {.uri = "/mode", .method = HTTP_POST, .handler = mode_post_handler},
        {.uri = "/lines", .method = HTTP_POST, .handler = lines_post_handler},
        {.uri = "/generate_204", .method = HTTP_GET, .handler = root_get_handler},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = root_get_handler},
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        httpd_register_uri_handler(server, &handlers[i]);
    }
}

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    while (true) {
        struct sockaddr_in source;
        socklen_t source_len = sizeof(source);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&source, &source_len);
        if (len < 12) {
            continue;
        }
        uint8_t response[600];
        memcpy(response, buf, len);
        response[2] = 0x81;
        response[3] = 0x80;
        response[7] = 1;
        int pos = len;
        response[pos++] = 0xc0;
        response[pos++] = 0x0c;
        response[pos++] = 0x00;
        response[pos++] = 0x01;
        response[pos++] = 0x00;
        response[pos++] = 0x01;
        response[pos++] = 0x00;
        response[pos++] = 0x00;
        response[pos++] = 0x00;
        response[pos++] = 0x3c;
        response[pos++] = 0x00;
        response[pos++] = 0x04;
        response[pos++] = 192;
        response[pos++] = 168;
        response[pos++] = 4;
        response[pos++] = 1;
        sendto(sock, response, pos, 0, (struct sockaddr *)&source, source_len);
    }
}

static void start_ap(void)
{
    if (s_panel.ap_started) {
        return;
    }
    wifi_config_t ap_config = {0};
    copy_trunc((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), s_panel.ap_ssid);
    ap_config.ap.ssid_len = strlen(s_panel.ap_ssid);
    ap_config.ap.channel = 6;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(s_panel.core->config.wifi_ssid[0] ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    s_panel.ap_started = true;
    ESP_LOGW(TAG, "configuration portal AP started: %s (%s)", s_panel.ap_ssid, PORTAL_AP_IP);
    xTaskCreate(dns_task, "portal_dns", 3072, NULL, 3, NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Wi-Fi STA disconnected reason=%u, retry=%u", event != NULL ? event->reason : 0, s_panel.retry_count);
        xEventGroupClearBits(s_panel.wifi_events, WIFI_CONNECTED_BIT);
        if (s_panel.retry_count++ >= 10) {
            xEventGroupSetBits(s_panel.wifi_events, WIFI_FAIL_BIT);
            start_ap();
            s_panel.retry_count = 10;
        }
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_panel.retry_count = 0;
        xEventGroupSetBits(s_panel.wifi_events, WIFI_CONNECTED_BIT);
        if (!esp_sntp_enabled()) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
        }
        ESP_LOGI(TAG,
                 "Wi-Fi STA connected; control panel is available at http://" IPSTR,
                 IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t wifi_start(void)
{
    if (s_panel.wifi_started) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        return loop_err;
    }
    s_panel.event_loop_ready = true;
    s_panel.sta_netif = esp_netif_create_default_wifi_sta();
    s_panel.ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_country_t country = {
        .cc = "PL",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));
    if (s_panel.core->config.wifi_ssid[0] != '\0') {
        wifi_config_t sta_config = {0};
        copy_trunc((char *)sta_config.sta.ssid, sizeof(sta_config.sta.ssid), s_panel.core->config.wifi_ssid);
        copy_trunc((char *)sta_config.sta.password, sizeof(sta_config.sta.password), s_panel.core->config.wifi_password);
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    } else {
        start_ap();
    }

    s_panel.wifi_started = true;
    return esp_wifi_start();
}

bool control_panel_wifi_connected(void)
{
    if (s_panel.wifi_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_panel.wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

bool control_panel_ap_active(void)
{
    return s_panel.ap_started;
}

bool control_panel_wait_for_wifi(TickType_t timeout_ticks)
{
    if (s_panel.wifi_events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_panel.wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        timeout_ticks);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t control_panel_start(irrigation_core_t *core, QueueHandle_t controller_queue)
{
    if (core == NULL || controller_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_panel, 0, sizeof(s_panel));
    s_panel.core = core;
    s_panel.queue = controller_queue;
    s_panel.wifi_events = xEventGroupCreate();

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_panel.ap_ssid, sizeof(s_panel.ap_ssid), "Irrigation-%02X%02X%02X", mac[3], mac[4], mac[5]);

    ESP_ERROR_CHECK(wifi_start());

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 14;
    ESP_ERROR_CHECK(httpd_start(&s_panel.server, &config));
    register_handlers(s_panel.server);
    ESP_LOGI(TAG, "HTTP control panel started");
    return ESP_OK;
}
