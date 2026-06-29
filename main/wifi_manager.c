/*
 * WiFi Manager for ESP32-S3-RLCD-4.2 Calendar
 * AP config mode (first boot) or STA auto-connect + SNTP time sync.
 */

#include "wifi_manager.h"
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_mgr";

// ── NVS keys ───────────────────────────────────────────────────────────────────
#define NVS_NAMESPACE "wifi_creds"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "password"

// ── AP config ──────────────────────────────────────────────────────────────────
#define AP_PASS       "12345678"
#define AP_MAX_CONN   4
#define AP_CHANNEL    1

static char s_ap_ssid[24] = {0};

// ── STA state ──────────────────────────────────────────────────────────────────
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_count = 0;
static const int MAX_RETRY = 10;

// ── NVS helpers ────────────────────────────────────────────────────────────────
bool wifi_manager_has_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return false;
    nvs_close(handle);
    return true;
}

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_set_str(handle, NVS_KEY_SSID, ssid);
    nvs_set_str(handle, NVS_KEY_PASS, pass);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Credentials saved: SSID=%s", ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_load_credentials(char *ssid, size_t ssid_len,
                                         char *pass, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_len);
    if (err != ESP_OK) { nvs_close(handle); return err; }
    err = nvs_get_str(handle, NVS_KEY_PASS, pass, &pass_len);
    nvs_close(handle);
    return err;
}

const char *wifi_manager_get_ap_ssid(void)
{
    return s_ap_ssid;
}

// ── URL decode ─────────────────────────────────────────────────────────────────
static int url_decode(const char *src, char *dst, int dst_max)
{
    int j = 0;
    for (int i = 0; src[i] && j < dst_max - 1; i++) {
        if (src[i] == '+') {
            dst[j++] = ' ';
        } else if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = {src[i+1], src[i+2], 0};
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
    return j;
}

// ── HTTP server (AP config mode) ───────────────────────────────────────────────
static const char *CONFIG_HTML =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>RLCD Calendar</title>"
"<style>body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 20px}"
"h1{color:#333}input{width:100%;padding:8px;margin:8px 0;box-sizing:border-box}"
"button{width:100%;padding:12px;background:#007bff;color:#fff;border:none;font-size:16px}"
"</style></head><body>"
"<h1>WiFi &#37197;&#32622;</h1>"  /* WiFi 配置 */
"<form method=\"POST\" action=\"/save\">"
"<p>WiFi&#21517;&#31216;: <input name=\"ssid\" required></p>"  /* 名称 */
"<p>&#23494;&#30721;: <input name=\"pass\" type=\"password\"></p>"  /* 密码 */
"<button type=\"submit\">&#20445;&#23384;&#24182;&#36830;&#25509;</button>"  /* 保存并连接 */
"</form></body></html>";

static esp_err_t config_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, CONFIG_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Read failed");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // Parse ssid=...&pass=...
    char ssid[64] = {0}, pass[64] = {0};
    char *p = strstr(buf, "ssid=");
    if (p) {
        p += 5;
        char *end = strchr(p, '&');
        if (end) {
            char tmp[64];
            int len = (int)(end - p);
            if (len >= (int)sizeof(tmp)) len = sizeof(tmp) - 1;
            memcpy(tmp, p, len);
            tmp[len] = '\0';
            url_decode(tmp, ssid, sizeof(ssid));
        }
    }
    p = strstr(buf, "pass=");
    if (p) {
        p += 5;
        char tmp[64];
        strncpy(tmp, p, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        url_decode(tmp, pass, sizeof(pass));
    }

    ESP_LOGI(TAG, "Received SSID=%s", ssid);

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty SSID");
        return ESP_FAIL;
    }

    wifi_manager_save_credentials(ssid, pass);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,
        "<h3>OK! Rebooting...</h3>"
        "<p>Device will restart and connect to WiFi.</p>",
        HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;  // unreachable
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    httpd_uri_t get_root = { .uri = "/", .method = HTTP_GET, .handler = config_get_handler };
    httpd_uri_t post_save = { .uri = "/save", .method = HTTP_POST, .handler = config_post_handler };
    httpd_register_uri_handler(server, &get_root);
    httpd_register_uri_handler(server, &post_save);

    ESP_LOGI(TAG, "HTTP server started on port 80");
    return server;
}

// ── WiFi event handlers ────────────────────────────────────────────────────────
static void sta_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg; (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "Retry STA connect (%d/%d)", s_retry_count, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ── AP mode (config) ───────────────────────────────────────────────────────────
static void wifi_init_ap(void)
{
    // Generate AP SSID from MAC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "RLCD-%02X%02X", mac[4], mac[5]);

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = 0,  // null-terminated
            .channel = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .required = true },
        },
    };
    strlcpy((char *)wifi_config.ap.ssid, s_ap_ssid, sizeof(wifi_config.ap.ssid));
    strlcpy((char *)wifi_config.ap.password, AP_PASS, sizeof(wifi_config.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started: SSID=%s, pass=%s", s_ap_ssid, AP_PASS);

    start_webserver();
}

// ── STA mode (connect) ─────────────────────────────────────────────────────────
static esp_err_t wifi_init_sta(void)
{
    char ssid[64], pass[64];
    esp_err_t err = wifi_manager_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
    if (err != ESP_OK) return err;

    s_wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t inst_any, inst_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_handler, NULL, &inst_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_handler, NULL, &inst_got_ip));

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "STA connecting to SSID=%s...", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(30000));  // 30s timeout

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to connect to WiFi");
    return ESP_FAIL;
}

// ── Public API ─────────────────────────────────────────────────────────────────
esp_err_t wifi_manager_init(void)
{
    // Init netif and event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (wifi_manager_has_credentials()) {
        return wifi_init_sta();
    } else {
        wifi_init_ap();
        return ESP_ERR_NOT_FOUND;  // signal: AP config mode
    }
}

int wifi_manager_get_rssi(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return (int)ap_info.rssi;
    }
    return -100;
}

bool wifi_manager_get_time(struct tm *timeinfo)
{
    // Set timezone to China Standard Time
    setenv("TZ", "CST-8", 1);
    tzset();

    // Configure SNTP
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");

    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_cfg));

    ESP_LOGI(TAG, "Waiting for SNTP sync...");
    int retry = 0;
    const int max_retry = 10;
    esp_err_t ret = ESP_ERR_TIMEOUT;
    while (retry < max_retry) {
        ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000));
        if (ret == ESP_OK) break;
        ESP_LOGI(TAG, "SNTP waiting... (%d/%d)", ++retry, max_retry);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SNTP sync failed");
        esp_netif_sntp_deinit();
        return false;
    }

    time_t now;
    time(&now);
    localtime_r(&now, timeinfo);

    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", timeinfo);
    ESP_LOGI(TAG, "SNTP synced: %s", buf);

    esp_netif_sntp_deinit();
    return true;
}
