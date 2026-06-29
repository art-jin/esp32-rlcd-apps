/*
 * Weather client for ESP32-S3-RLCD-4.2 Calendar
 * Fetches current conditions from Open-Meteo (free, no API key).
 */

#include "weather.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "weather";

// Beijing coordinates
#define WEATHER_LAT    39.9
#define WEATHER_LON    116.4
#define WEATHER_URL    "http://api.open-meteo.com/v1/forecast?latitude=39.9&longitude=116.4&current=temperature_2m,relative_humidity_2m,weather_code&timezone=Asia%2FShanghai"

// ── WMO weather code → GB2312 description ──────────────────────────────────────
// GB2312 encoded: 晴 多云 阴 雾 小雨 中雨 大雨 雪 阵雨 雷暴 冻雨 阵雪 毛毛雨
static const uint8_t gb_qing[]     = {0xC7,0xE7, 0};               // 晴
static const uint8_t gb_duoyun[]   = {0xB6,0xE0,0xD4,0xC6, 0};    // 多云
static const uint8_t gb_yin[]      = {0xD2,0xF5, 0};               // 阴
static const uint8_t gb_wu[]       = {0xCE,0xED, 0};               // 雾
static const uint8_t gb_xiaoyu[]   = {0xD0,0xA1,0xD3,0xEA, 0};    // 小雨
static const uint8_t gb_zhongyu[]  = {0xD6,0xD0,0xD3,0xEA, 0};    // 中雨
static const uint8_t gb_dayu[]     = {0xB4,0xF3,0xD3,0xEA, 0};    // 大雨
static const uint8_t gb_xue[]      = {0xD1,0xA9, 0};               // 雪
static const uint8_t gb_zhenyu[]   = {0xD5,0xF3,0xD3,0xEA, 0};    // 阵雨
static const uint8_t gb_leibao[]   = {0xC0,0xD7,0xB1,0xA9, 0};    // 雷暴
static const uint8_t gb_dongyu[]   = {0xB6,0xB3,0xD3,0xEA, 0};    // 冻雨
static const uint8_t gb_zhenxue[]  = {0xD5,0xF3,0xD1,0xA9, 0};    // 阵雪

static const uint8_t *wmo_to_desc(int code)
{
    if (code <= 1) return gb_qing;       // Clear / Mainly clear
    if (code == 2) return gb_duoyun;     // Partly cloudy
    if (code == 3) return gb_yin;        // Overcast
    if (code == 45 || code == 48) return gb_wu;    // Fog
    if (code >= 51 && code <= 55) return gb_xiaoyu; // Drizzle
    if (code == 56 || code == 57) return gb_dongyu; // Freezing drizzle
    if (code == 61) return gb_xiaoyu;    // Slight rain
    if (code == 63) return gb_zhongyu;   // Moderate rain
    if (code == 65) return gb_dayu;      // Heavy rain
    if (code == 66 || code == 67) return gb_dongyu; // Freezing rain
    if (code == 71) return gb_xue;       // Slight snow
    if (code == 73) return gb_xue;       // Moderate snow
    if (code == 75) return gb_xue;       // Heavy snow
    if (code == 77) return gb_zhenxue;   // Snow grains
    if (code >= 80 && code <= 82) return gb_zhenyu; // Rain showers
    if (code >= 85 && code <= 86) return gb_zhenxue; // Snow showers
    if (code >= 95) return gb_leibao;    // Thunderstorm
    return gb_duoyun;                    // Default
}

// ── HTTP response buffer ────────────────────────────────────────────────────────
static char *s_resp_buf;
static int s_resp_len;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (s_resp_len + evt->data_len < 2048) {
            memcpy(s_resp_buf + s_resp_len, evt->data, evt->data_len);
            s_resp_len += evt->data_len;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

// ── Public API ──────────────────────────────────────────────────────────────────
esp_err_t weather_fetch(weather_data_t *data)
{
    if (!data) return ESP_ERR_INVALID_ARG;

    // Default values
    data->temperature = 0;
    data->humidity = 0;
    data->weather_code = 0;
    data->description = gb_qing;

    char resp_buf[2048];
    s_resp_buf = resp_buf;
    s_resp_len = 0;

    esp_http_client_config_t config = {
        .url = WEATHER_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .event_handler = http_event_handler,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "HTTP request failed: err=%s, status=%d", esp_err_to_name(err), status);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    resp_buf[s_resp_len] = '\0';
    ESP_LOGI(TAG, "Weather response (%d bytes): %s", s_resp_len, resp_buf);

    esp_http_client_cleanup(client);

    // Parse JSON
    cJSON *root = cJSON_Parse(resp_buf);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return ESP_FAIL;
    }

    cJSON *current = cJSON_GetObjectItem(root, "current");
    if (!current) {
        ESP_LOGE(TAG, "No 'current' in JSON");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *temp = cJSON_GetObjectItem(current, "temperature_2m");
    if (temp && cJSON_IsNumber(temp)) {
        data->temperature = (float)temp->valuedouble;
    }

    cJSON *humid = cJSON_GetObjectItem(current, "relative_humidity_2m");
    if (humid && cJSON_IsNumber(humid)) {
        data->humidity = humid->valueint;
    }

    cJSON *wcode = cJSON_GetObjectItem(current, "weather_code");
    if (wcode && cJSON_IsNumber(wcode)) {
        data->weather_code = wcode->valueint;
        data->description = wmo_to_desc(data->weather_code);
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Weather: %.1f°C, %d%%, code=%d",
             data->temperature, data->humidity, data->weather_code);

    return ESP_OK;
}
