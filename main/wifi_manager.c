/*
 * WiFi Manager for ESP32-S3-RLCD-4.2 Calendar
 * AP config mode (first boot) or STA auto-connect + SNTP time sync.
 */

#include "wifi_manager.h"
#include <string.h>
#include <ctype.h>
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
#define NVS_KEY_SSID       "ssid"
#define NVS_KEY_PASS       "password"
#define NVS_KEY_KINCAL     "kincal_code"   // 6-char short code
#define NVS_KEY_HAS_BACK   "has_back_key"  // u8 bool

// Valid characters for KinCal short codes (no ambiguous chars: 0/O, 1/I).
#define KINCAL_CODE_ALPHABET "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
#define KINCAL_CODE_LEN      6

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
"small{color:#666}"
"</style></head><body>"
"<h1>WiFi &#37197;&#32622;</h1>"  /* WiFi 配置 */
"<form method=\"POST\" action=\"/save\">"
"<p>WiFi&#21517;&#31216;: <input name=\"ssid\" required></p>"  /* 名称 */
"<p>&#23494;&#30721;: <input name=\"pass\" type=\"password\"></p>"  /* 密码 */
"<p>KinCal&#30701;&#30721;: <input name=\"kcode\" maxlength=\"6\""
" pattern=\"[ABCDEFGHJKLMNPQRSTUVWXYZ23456789]{6}\""
" placeholder=\"ABC234\" style=\"text-transform:uppercase\"></p>"
"<small>&#30041;&#31354;&#36339;&#36807; KinCal&#37197;&#23545;&#65288;&#20165;WiFi&#27169;&#24335;&#65289;</small><br>"
"<label><input type=\"checkbox\" name=\"backkey\" value=\"1\">"
"&#25105;&#26377;4&#38190;&#30828;&#20214;&#65288;GPIO43 BACK&#25353;&#38190;&#65289;</label>"
"<button type=\"submit\">&#20445;&#23384;&#24182;&#36830;&#25509;</button>"  /* 保存并连接 */
"</form></body></html>";

static esp_err_t config_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, CONFIG_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Extract one form field from a urlencoded body. Finds "name=..." up to next
// '&' or end-of-string, URL-decodes into `out`. Returns field length or 0 if
// not found / empty.
static int extract_field(const char *body, const char *name,
                         char *out, size_t out_len)
{
    size_t nlen = strlen(name);
    const char *p = body;
    while ((p = strstr(p, name)) != NULL) {
        // Ensure match is at field boundary (start of body or preceded by '&')
        if (p != body && p[-1] != '&') {
            p += nlen;
            continue;
        }
        p += nlen;
        if (*p != '=') continue;
        p++;  // skip '='
        const char *end = strchr(p, '&');
        size_t raw_len = end ? (size_t)(end - p) : strlen(p);
        char tmp[128];
        if (raw_len >= sizeof(tmp)) raw_len = sizeof(tmp) - 1;
        memcpy(tmp, p, raw_len);
        tmp[raw_len] = '\0';
        return url_decode(tmp, out, (int)out_len);
    }
    out[0] = '\0';
    return 0;
}

// Validate a KinCal short code: exactly 6 chars from the unambiguous alphabet.
static bool kincal_code_valid(const char *code)
{
    if (strlen(code) != KINCAL_CODE_LEN) return false;
    for (int i = 0; i < KINCAL_CODE_LEN; i++) {
        if (strchr(KINCAL_CODE_ALPHABET, code[i]) == NULL) return false;
    }
    return true;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    // 4 fields × ~80 chars max each — 512 is plenty.
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Read failed");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ssid[64] = {0}, pass[64] = {0};
    char kcode[16] = {0}, backkey[8] = {0};
    extract_field(buf, "ssid",   ssid,   sizeof(ssid));
    extract_field(buf, "pass",   pass,   sizeof(pass));
    extract_field(buf, "kcode",  kcode,  sizeof(kcode));
    extract_field(buf, "backkey", backkey, sizeof(backkey));

    // Uppercase kcode (browser may submit lowercase despite pattern attr).
    for (char *q = kcode; *q; q++) *q = toupper((unsigned char)*q);

    ESP_LOGI(TAG, "Received SSID=%s kcode=%s backkey=%s", ssid, kcode, backkey);

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty SSID");
        return ESP_FAIL;
    }
    // kcode is optional — empty means "skip KinCal pairing, WiFi only".
    if (kcode[0] != '\0' && !kincal_code_valid(kcode)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
            "Invalid KinCal code (need 6 chars A-Z 2-9, no O/I/0/1)");
        return ESP_FAIL;
    }

    // Persist all four fields in one NVS commit.
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY_SSID, ssid);
        nvs_set_str(h, NVS_KEY_PASS, pass);
        nvs_set_str(h, NVS_KEY_KINCAL, kcode);  // empty string is fine
        uint8_t has_back = (backkey[0] == '1') ? 1 : 0;
        nvs_set_u8(h, NVS_KEY_HAS_BACK, has_back);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Saved: SSID=%s kcode=%s has_back=%d", ssid, kcode, has_back);
    } else {
        ESP_LOGE(TAG, "NVS open failed for save");
    }

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

// ── KinCal code + 4-key NVS getters ───────────────────────────────────────────
esp_err_t wifi_manager_load_kincal_code(char *out, size_t len)
{
    if (len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t required = len;
    err = nvs_get_str(h, NVS_KEY_KINCAL, out, &required);
    nvs_close(h);
    return err;
}

bool wifi_manager_load_has_back_key(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t val = 0;
    nvs_get_u8(h, NVS_KEY_HAS_BACK, &val);
    nvs_close(h);
    return val != 0;
}
