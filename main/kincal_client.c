/*
 * KinCal JSON client implementation.
 *
 * esp_http_client GET over HTTPS to /api/v1/esp32/display/{short_code}.
 * Uses the ESP-IDF cert bundle (same as caldav.c) so we trust whatever
 * public CA signed the server. cJSON for parsing.
 *
 * All large buffers allocated from heap — payload can be ~4-10 KB.
 */

#include "kincal_client.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "kincal";

#define KINCAL_SHORT_CODE_LEN   6
#define KINCAL_RESP_BUF_SIZE    (16 * 1024)   /* 16 KB heap                */
#define KINCAL_URL_MAX          128
#define KINCAL_DEFAULT_REFRESH  60
#define KINCAL_MAX_REFRESH      3600
#define KINCAL_MIN_REFRESH      60

// Cached at init from NVS / Kconfig.
static char  s_short_code[KINCAL_SHORT_CODE_LEN + 1] = {0};
static char  s_host[64] = {0};
static bool  s_use_https = true;
static bool  s_inited = false;

// ── HTTP receive buffer (per-call) ─────────────────────────────────────────────
static char *s_resp_buf = NULL;
static int   s_resp_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_resp_buf && s_resp_len + evt->data_len < KINCAL_RESP_BUF_SIZE) {
            memcpy(s_resp_buf + s_resp_len, evt->data, evt->data_len);
            s_resp_len += evt->data_len;
        }
    }
    return ESP_OK;
}

// ── Small cJSON helpers ───────────────────────────────────────────────────────
static void copy_str(const cJSON *obj, const char *key, char *dst, size_t dst_len)
{
    if (!obj) return;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v && cJSON_IsString(v) && v->valuestring) {
        strncpy(dst, v->valuestring, dst_len - 1);
        dst[dst_len - 1] = '\0';
    }
}

static int get_int(const cJSON *obj, const char *key, int def)
{
    if (!obj) return def;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (v && cJSON_IsNumber(v)) ? v->valueint : def;
}

static bool get_bool(const cJSON *obj, const char *key)
{
    if (!obj) return false;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return v && cJSON_IsBool(v) && cJSON_IsTrue(v);
}

// Parse a "HH:MM" or "全天" time string into the event. Leaves time_str
// as the raw string so the bridge can render "全天" verbatim.
static void parse_event(const cJSON *item, kincal_event_t *out)
{
    memset(out, 0, sizeof(*out));
    copy_str(item, "date",      out->date_str, sizeof(out->date_str));
    copy_str(item, "time",      out->time_str, sizeof(out->time_str));
    copy_str(item, "title",     out->title,    sizeof(out->title));
    copy_str(item, "location",  out->location, sizeof(out->location));
}

// ── JSON → struct ─────────────────────────────────────────────────────────────
static esp_err_t parse_payload(const char *json, kincal_display_data_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG, "cJSON parse failed");
        return ESP_FAIL;
    }

    memset(out, 0, sizeof(*out));

    out->server_now_epoch = get_int(root, "server_now_epoch", 0);
    copy_str(root, "timezone", out->timezone, sizeof(out->timezone));
    out->cal_year  = get_int(root, "cal_year",  0);
    out->cal_month = get_int(root, "cal_month", 0);
    out->today_day = get_int(root, "today_day", -1);

    // weeks: 6×7 grid
    const cJSON *weeks = cJSON_GetObjectItemCaseSensitive(root, "weeks");
    if (weeks && cJSON_IsArray(weeks)) {
        int nrows = cJSON_GetArraySize(weeks);
        if (nrows > 6) nrows = 6;
        for (int r = 0; r < nrows; r++) {
            const cJSON *row = cJSON_GetArrayItem(weeks, r);
            if (!row || !cJSON_IsArray(row)) continue;
            int ncols = cJSON_GetArraySize(row);
            if (ncols > 7) ncols = 7;
            for (int c = 0; c < ncols; c++) {
                const cJSON *cell = cJSON_GetArrayItem(row, c);
                if (cell && cJSON_IsNumber(cell)) {
                    out->weeks[r][c] = cell->valueint;
                }
            }
        }
    }

    // rest_days / event_dates arrays
    const cJSON *rest = cJSON_GetObjectItemCaseSensitive(root, "rest_days");
    if (rest && cJSON_IsArray(rest)) {
        int n = cJSON_GetArraySize(rest);
        if (n > 32) n = 32;
        for (int i = 0; i < n; i++) {
            const cJSON *v = cJSON_GetArrayItem(rest, i);
            if (v && cJSON_IsNumber(v)) out->rest_days[out->rest_count++] = v->valueint;
        }
    }
    const cJSON *ed = cJSON_GetObjectItemCaseSensitive(root, "event_dates");
    if (ed && cJSON_IsArray(ed)) {
        int n = cJSON_GetArraySize(ed);
        if (n > 32) n = 32;
        for (int i = 0; i < n; i++) {
            const cJSON *v = cJSON_GetArrayItem(ed, i);
            if (v && cJSON_IsNumber(v)) out->event_dates[out->event_count++] = v->valueint;
        }
    }

    // events_today
    const cJSON *et = cJSON_GetObjectItemCaseSensitive(root, "events_today");
    if (et && cJSON_IsArray(et)) {
        int n = cJSON_GetArraySize(et);
        if (n > KINCAL_MAX_EVENTS) n = KINCAL_MAX_EVENTS;
        for (int i = 0; i < n; i++) {
            parse_event(cJSON_GetArrayItem(et, i), &out->events_today[out->today_count++]);
        }
    }

    // upcoming_events
    const cJSON *up = cJSON_GetObjectItemCaseSensitive(root, "upcoming_events");
    if (up && cJSON_IsArray(up)) {
        int n = cJSON_GetArraySize(up);
        if (n > KINCAL_MAX_EVENTS) n = KINCAL_MAX_EVENTS;
        for (int i = 0; i < n; i++) {
            parse_event(cJSON_GetArrayItem(up, i), &out->upcoming[out->upcoming_count++]);
        }
    }

    // Plan-gated weather
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(root, "weather");
    if (w && cJSON_IsObject(w)) {
        out->has_weather = true;
        out->weather.temp_c = get_int(w, "temp_c", 0);
        copy_str(w, "description", out->weather.description,
                 sizeof(out->weather.description));
        out->weather.humidity = get_int(w, "humidity", -1);
        copy_str(w, "city", out->weather.city, sizeof(out->weather.city));
    }

    // Plan-gated lunar
    const cJSON *lunar_text = cJSON_GetObjectItemCaseSensitive(root, "lunar_date_text");
    if (lunar_text && cJSON_IsString(lunar_text) && lunar_text->valuestring[0]) {
        out->has_lunar = true;
        copy_str(root, "lunar_date_text", out->lunar_date_text,
                 sizeof(out->lunar_date_text));
    }

    // AI face
    copy_str(root, "ai_face_id",      out->ai_face_id, sizeof(out->ai_face_id));
    copy_str(root, "ai_face_message", out->ai_face_msg, sizeof(out->ai_face_msg));

    // Refresh + plan
    out->refresh_interval_seconds = get_int(root, "refresh_interval_seconds",
                                            KINCAL_DEFAULT_REFRESH);
    const cJSON *plan = cJSON_GetObjectItemCaseSensitive(root, "plan");
    copy_str(plan, "tier", out->plan_tier, sizeof(out->plan_tier));

    cJSON_Delete(root);
    return ESP_OK;
}

// ── Public API ────────────────────────────────────────────────────────────────
esp_err_t kincal_client_init(void)
{
    if (s_inited) return ESP_OK;

    // Read short code from NVS (saved by AP config portal)
    wifi_manager_load_kincal_code(s_short_code, sizeof(s_short_code));

    // Host: Kconfig default, dev override via NVS not yet wired
#ifdef CONFIG_KINCAL_HOST
    strncpy(s_host, CONFIG_KINCAL_HOST, sizeof(s_host) - 1);
#else
    strncpy(s_host, "kincal.cn", sizeof(s_host) - 1);
#endif
    s_host[sizeof(s_host) - 1] = '\0';

#ifdef CONFIG_KINCAL_USE_HTTPS
    s_use_https = true;
#else
    s_use_https = false;
#endif

    s_inited = true;
    ESP_LOGI(TAG, "init: host=%s https=%d short_code=%s%s",
             s_host, s_use_https, s_short_code,
             s_short_code[0] ? "" : " (NOT PROVISIONED)");
    return ESP_OK;
}

bool kincal_client_is_provisioned(void)
{
    return s_short_code[0] != '\0';
}

const char *kincal_client_get_short_code(void)
{
    return s_short_code;
}

esp_err_t kincal_client_fetch(kincal_display_data_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_inited) {
        ESP_LOGE(TAG, "fetch before init");
        return ESP_ERR_INVALID_STATE;
    }
    if (!kincal_client_is_provisioned()) {
        ESP_LOGW(TAG, "fetch: no short code provisioned");
        return ESP_ERR_NOT_FOUND;
    }

    memset(out, 0, sizeof(*out));

    char url[KINCAL_URL_MAX];
    int port = s_use_https ? 443 : 80;
    snprintf(url, sizeof(url), "%s://%s:%d/api/v1/esp32/display/%s",
             s_use_https ? "https" : "http",
             s_host, port, s_short_code);

    s_resp_buf = malloc(KINCAL_RESP_BUF_SIZE);
    if (!s_resp_buf) {
        ESP_LOGE(TAG, "alloc %d failed", KINCAL_RESP_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }
    s_resp_len = 0;

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .event_handler = http_event_handler,
        .transport_type = s_use_https ? HTTP_TRANSPORT_OVER_SSL
                                      : HTTP_TRANSPORT_OVER_TCP,
        .user_agent = "ESP32-RLCD/1.0",
#ifndef CONFIG_KINCAL_SKIP_CERT_VERIFY
        .crt_bundle_attach = esp_crt_bundle_attach,
#else
        .skip_cert_common_name_check = true,  /* dev only: self-signed local server */
#endif
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "http init failed");
        free(s_resp_buf); s_resp_buf = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "GET %s", url);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    esp_err_t result = ESP_FAIL;
    if (err == ESP_OK && status == 200 && s_resp_len > 0) {
        s_resp_buf[s_resp_len] = '\0';
        ESP_LOGI(TAG, "OK status=200 len=%d", s_resp_len);
        result = parse_payload(s_resp_buf, out);
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "parsed: events_today=%d upcoming=%d weather=%d lunar=%d refresh=%ds",
                     out->today_count, out->upcoming_count,
                     out->has_weather, out->has_lunar,
                     out->refresh_interval_seconds);
        }
    } else if (status == 404 || status == 403) {
        ESP_LOGE(TAG, "auth/ provisioning error: status=%d (short_code=%s)",
                 status, s_short_code);
        result = ESP_ERR_NOT_FOUND;
    } else {
        ESP_LOGE(TAG, "fetch failed: err=%s status=%d len=%d",
                 esp_err_to_name(err), status, s_resp_len);
        result = ESP_FAIL;
    }

    esp_http_client_cleanup(client);
    free(s_resp_buf); s_resp_buf = NULL;
    return result;
}

void kincal_display_data_free(kincal_display_data_t *d)
{
    if (d) memset(d, 0, sizeof(*d));
}

int kincal_client_next_refresh_seconds(const kincal_display_data_t *d)
{
    if (!d || d->refresh_interval_seconds <= 0) return KINCAL_DEFAULT_REFRESH;
    if (d->refresh_interval_seconds > KINCAL_MAX_REFRESH) return KINCAL_MAX_REFRESH;
    if (d->refresh_interval_seconds < KINCAL_MIN_REFRESH) return KINCAL_MIN_REFRESH;
    return d->refresh_interval_seconds;
}
