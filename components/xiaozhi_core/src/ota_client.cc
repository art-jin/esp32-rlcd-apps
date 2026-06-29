#include "ota_client.h"
#include "config.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_mac.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_partition.h>
#include <nvs_flash.h>
#include <esp_crt_bundle.h>
#include <cJSON.h>
#include <cstring>
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "XiaoZhi OTA";

OtaClient::OtaClient() {}

OtaClient::~OtaClient() {}

std::string OtaClient::GetMacAddress() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(buf);
}

esp_err_t OtaClient::BuildPostBody(std::string &json) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_FAIL;

    cJSON_AddNumberToObject(root, "version", XZ_PROTOCOL_VERSION);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    cJSON_AddNumberToObject(root, "flash_size", (double)flash_size);
    cJSON_AddNumberToObject(root, "minimum_free_heap_size", (double)esp_get_free_heap_size());

    std::string mac = GetMacAddress();
    cJSON_AddStringToObject(root, "mac_address", mac.c_str());

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON_AddStringToObject(root, "chip_model_name", "esp32s3");
    cJSON *chip_info = cJSON_CreateObject();
    cJSON_AddNumberToObject(chip_info, "model", chip.model);
    cJSON_AddNumberToObject(chip_info, "cores", chip.cores);
    cJSON_AddNumberToObject(chip_info, "revision", chip.revision);
    cJSON_AddItemToObject(root, "chip_info", chip_info);

    cJSON *app = cJSON_CreateObject();
    cJSON_AddStringToObject(app, "name", XZ_BOARD_NAME);
    cJSON_AddStringToObject(app, "version", "2.1.0");
    cJSON_AddItemToObject(root, "application", app);

    char *json_str = cJSON_PrintUnformatted(root);
    json = json_str;
    cJSON_Delete(root);
    free(json_str);
    return ESP_OK;
}

esp_err_t OtaClient::ParseResponse(const char *json_str, int len) {
    cJSON *root = cJSON_ParseWithLength(json_str, len);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse OTA response JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Extract websocket config
    cJSON *ws = cJSON_GetObjectItem(root, "websocket");
    if (ws) {
        nvs_handle_t h;
        esp_err_t ret = nvs_open(XZ_NVS_NAMESPACE, NVS_READWRITE, &h);
        if (ret == ESP_OK) {
            cJSON *item = ws->child;
            while (item) {
                if (cJSON_IsString(item)) {
                    ESP_LOGI(TAG, "  NVS key='%s' value='%s'", item->string, item->valuestring);
                    nvs_set_str(h, item->string, item->valuestring);
                } else if (cJSON_IsNumber(item)) {
                    ESP_LOGI(TAG, "  NVS key='%s' value=%d", item->string, (int)item->valuedouble);
                    nvs_set_i32(h, item->string, (int32_t)item->valuedouble);
                }
                item = item->next;
            }
            nvs_commit(h);
            nvs_close(h);
            ESP_LOGI(TAG, "WebSocket credentials stored in NVS");
        }
    }

    cJSON_Delete(root);
    return ws ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t OtaClient::StoreCredentials() {
    // Already stored in ParseResponse
    return ESP_OK;
}

esp_err_t OtaClient::LoadCredentials(std::string &url, std::string &token, int &version) {
    nvs_handle_t h;
    esp_err_t ret = nvs_open(XZ_NVS_NAMESPACE, NVS_READONLY, &h);
    if (ret != ESP_OK) return ESP_ERR_NOT_FOUND;

    char buf[256];
    size_t len = sizeof(buf);

    ret = nvs_get_str(h, XZ_NVS_KEY_URL, buf, &len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS key '%s' not found (err=%s)", XZ_NVS_KEY_URL, esp_err_to_name(ret));
        nvs_close(h); return ESP_ERR_NOT_FOUND;
    }
    url = buf;

    len = sizeof(buf);
    ret = nvs_get_str(h, XZ_NVS_KEY_TOKEN, buf, &len);
    if (ret != ESP_OK) { nvs_close(h); return ESP_ERR_NOT_FOUND; }
    token = buf;

    int32_t v = 1;
    nvs_get_i32(h, "version", &v);
    version = (int)v;

    nvs_close(h);
    ESP_LOGI(TAG, "Loaded credentials: url=%s..., version=%d", url.substr(0, 30).c_str(), version);
    return ESP_OK;
}

static const int MAX_RESPONSE_LEN = 4096;

typedef struct {
    char *buf;
    int len;
    int cap;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    http_response_t *resp = (http_response_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (resp->len + evt->data_len >= resp->cap) {
            int new_cap = resp->cap * 2;
            if (new_cap < resp->len + evt->data_len + 1)
                new_cap = resp->len + evt->data_len + 1;
            char *new_buf = (char *)realloc(resp->buf, new_cap);
            if (!new_buf) return ESP_FAIL;
            resp->buf = new_buf;
            resp->cap = new_cap;
        }
        memcpy(resp->buf + resp->len, evt->data, evt->data_len);
        resp->len += evt->data_len;
        resp->buf[resp->len] = '\0';
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t OtaClient::Activate() {
    std::string body;
    esp_err_t ret = BuildPostBody(body);
    if (ret != ESP_OK) return ret;

    std::string mac = GetMacAddress();

    http_response_t resp = {};
    resp.cap = MAX_RESPONSE_LEN;
    resp.buf = (char *)malloc(resp.cap);
    if (!resp.buf) return ESP_ERR_NO_MEM;
    resp.buf[0] = '\0';

    // Retry logic: 3 attempts with backoff
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            int delay_ms = 5000 * (1 << (attempt - 1)); // 5s, 10s
            ESP_LOGI(TAG, "Retry %d in %dms", attempt + 1, delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }

        esp_http_client_config_t cfg = {};
        cfg.url = XZ_OTA_URL;
        cfg.method = HTTP_METHOD_POST;
        cfg.event_handler = http_event_handler;
        cfg.user_data = &resp;
        cfg.timeout_ms = 15000;
        cfg.buffer_size = 2048;
        cfg.skip_cert_common_name_check = true;
        cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) continue;

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Device-Id", mac.c_str());
        esp_http_client_set_header(client, "User-Agent", XZ_USER_AGENT);
        esp_http_client_set_post_field(client, body.c_str(), body.length());

        resp.len = 0;
        resp.buf[0] = '\0';

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);

        if (err == ESP_OK && status == 200) {
            ESP_LOGI(TAG, "OTA activation success (%d bytes)", resp.len);
            ret = ParseResponse(resp.buf, resp.len);
            esp_http_client_cleanup(client);
            free(resp.buf);
            return ret;
        }

        ESP_LOGW(TAG, "OTA attempt %d failed: err=%s status=%d",
                 attempt + 1, esp_err_to_name(err), status);
        esp_http_client_cleanup(client);
    }

    free(resp.buf);
    ESP_LOGE(TAG, "OTA activation failed after 3 attempts");
    return ESP_FAIL;
}
