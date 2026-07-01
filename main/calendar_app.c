/*
 * Calendar app: monthly view + 3/4-key navigation.
 *
 * Worker task does 1Hz status-bar refresh + periodic data fetch + 60s SHTC3.
 *
 * Data source:
 *   - Default: KinCal JSON endpoint via kincal_client + kincal_bridge.
 *     Refresh interval comes from the server response (60s Pro/Biz, 300s Free).
 *   - Escape hatch (CONFIG_KINCAL_LEGACY_DIRECT_APIS=y): direct CalDAV
 *     (Feishu) + Open-Meteo calls. Kept compilable for offline / debug.
 */

#include "calendar_app.h"
#include "app_manager.h"
#include "app_framework.h"
#include "calendar.h"
#include "wifi_manager.h"
#include "battery.h"
#include "shtc3.h"
#include "st7306.h"
#include "keyboard.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <time.h>

#if CONFIG_KINCAL_LEGACY_DIRECT_APIS
#include "weather.h"
#include "caldav.h"
#else
#include "kincal_client.h"
#include "kincal_bridge.h"
#endif

static const char *TAG = "CalendarApp";

// Shared global state (defined in hello_world_main.c)
extern weather_data_t g_weather;
extern caldav_event_t g_events[];
extern int g_event_count;

// Lifecycle state
static volatile bool s_stop_flag = false;
static SemaphoreHandle_t s_exit_sem = NULL;
static TaskHandle_t s_worker = NULL;

// Periodic counters (persist across exits, reset on enter)
static int s_last_day = -1;
static int s_env_counter = 60;        // force first-loop SHTC3 read

#if CONFIG_KINCAL_LEGACY_DIRECT_APIS
static int s_weather_counter = 0;
static int s_caldav_counter = 0;

// Legacy: direct CalDAV fetch from Feishu
static void fetch_and_set_events(int year, int month, int day)
{
    caldav_event_t events[CALDAV_MAX_EVENTS];
    int count = 0;
    ESP_LOGI(TAG, "Fetching CalDAV events...");
    if (caldav_fetch_events(year, month, day, events, &count) == ESP_OK) {
        g_event_count = count > CALDAV_MAX_EVENTS ? CALDAV_MAX_EVENTS : count;
        for (int i = 0; i < g_event_count; i++) {
            g_events[i] = events[i];
        }
        calendar_set_events(g_events, g_event_count, year, month, day, &g_weather);
        ESP_LOGI(TAG, "CalDAV: %d events set", count);
    } else {
        ESP_LOGW(TAG, "CalDAV fetch failed, keeping previous events");
    }
}
#else
// KinCal: single fetch covers weather + events + lunar + rest_days.
static int s_kincal_counter = 0;
static int s_kincal_refresh_seconds = 60;  // updated after first successful fetch
static kincal_display_data_t s_kincal_data; // static so the 3 KB doesn't sit on stack

static void fetch_and_apply_kincal(int year, int month, int day)
{
    ESP_LOGI(TAG, "Fetching KinCal...");
    esp_err_t err = kincal_client_fetch(&s_kincal_data);
    if (err == ESP_OK) {
        s_kincal_refresh_seconds = kincal_client_next_refresh_seconds(&s_kincal_data);
        kincal_bridge_apply(&s_kincal_data);
        ESP_LOGI(TAG, "KinCal applied, refresh=%ds", s_kincal_refresh_seconds);
    } else {
        ESP_LOGW(TAG, "KinCal fetch failed: %s — keeping previous state",
                 esp_err_to_name(err));
    }
}
#endif

static void calendar_worker(void *arg)
{
    int sub_tick = 0;
    ESP_LOGI(TAG, "Worker started");

    /* Watchdog note: sdkconfig.defaults disables
     * CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/1 because XiaoZhi audio
     * pipeline + KinCal TLS handshake + ST7306 SPI redraw share CPU 1 and
     * can starve IDLE below the 10s threshold. cal_work itself is never
     * auto-subscribed to the WDT, so no esp_task_wdt_delete() needed here. */

    while (!s_stop_flag) {
        vTaskDelay(pdMS_TO_TICKS(100));  // 100ms granularity for responsive exit
        if (s_stop_flag) break;
        if (++sub_tick < 10) continue;   // 10 × 100ms = 1Hz
        sub_tick = 0;

        time_t now;
        time(&now);
        struct tm ti;
        localtime_r(&now, &ti);
        int y = ti.tm_year + 1900;
        int m = ti.tm_mon + 1;
        int d = ti.tm_mday;

        app_manager_display_lock();

        if (d != s_last_day) {
            // Day changed (or first iteration): full redraw
            s_last_day = d;
            calendar_draw_with_weather(y, m, d, &g_weather);
            if (g_event_count > 0) {
                calendar_set_events(g_events, g_event_count, y, m, d, &g_weather);
            }
        } else {
            calendar_redraw_left_panel(&g_weather);
        }
        calendar_draw_status_bar(&ti, wifi_manager_get_rssi(), battery_get_level());
        st7306_update_display();

        app_manager_display_unlock();

#if CONFIG_KINCAL_LEGACY_DIRECT_APIS
        // Legacy: weather refresh every 10 min
        if (++s_weather_counter >= 600) {
            s_weather_counter = 0;
            ESP_LOGI(TAG, "Fetching weather...");
            if (weather_fetch(&g_weather) == ESP_OK) {
                app_manager_display_lock();
                calendar_update_weather(&g_weather);
                st7306_update_display();
                app_manager_display_unlock();
            }
        }

        // Legacy: CalDAV refresh (initial burst at counter=5, then every 10 min)
        if (++s_caldav_counter >= 600 ||
            (s_caldav_counter == 5 && g_event_count == 0)) {
            s_caldav_counter = 0;
            fetch_and_set_events(y, m, d);
        }
#else
        // KinCal: unified fetch on dynamic interval.
        // Initial burst at counter==3 (let WiFi/SNTP settle), then dynamic.
        if (++s_kincal_counter >= s_kincal_refresh_seconds ||
            (s_kincal_counter == 3 && g_event_count == 0)) {
            s_kincal_counter = 0;
            fetch_and_apply_kincal(y, m, d);
            // Reflect the new state on screen immediately.
            app_manager_display_lock();
            calendar_draw_with_weather(y, m, d, &g_weather);
            if (g_event_count > 0) {
                calendar_set_events(g_events, g_event_count, y, m, d, &g_weather);
            }
            calendar_draw_status_bar(&ti, wifi_manager_get_rssi(),
                                     battery_get_level());
            st7306_update_display();
            app_manager_display_unlock();
        }
#endif

        // SHTC3 indoor temp/humidity every 60s
        if (++s_env_counter >= 60) {
            s_env_counter = 0;
            if (shtc3_measure()) {
                app_manager_display_lock();
                calendar_draw_env_data(shtc3_last_temperature(),
                                       shtc3_last_humidity());
                calendar_draw_status_bar(&ti, wifi_manager_get_rssi(),
                                         battery_get_level());
                st7306_update_display();
                app_manager_display_unlock();
            }
        }
    }

    ESP_LOGI(TAG, "Worker exiting");
    calendar_clear_selection();
    xSemaphoreGive(s_exit_sem);
    s_worker = NULL;
    vTaskDelete(NULL);
}

void calendar_on_enter(void)
{
    ESP_LOGI(TAG, "Entering calendar");

    s_stop_flag = false;
    if (!s_exit_sem) s_exit_sem = xSemaphoreCreateBinary();
    xSemaphoreTake(s_exit_sem, 0);  // ensure empty

    // Reset periodic counters (forces immediate refresh on entry)
    s_last_day = -1;
    s_env_counter = 60;
#if CONFIG_KINCAL_LEGACY_DIRECT_APIS
    s_weather_counter = 599;
    s_caldav_counter = 0;
#else
    s_kincal_counter = 0;
    // First fetch will happen at counter==3 (≈3s after entry)
#endif

    // Initial full draw
    time_t now;
    time(&now);
    struct tm ti;
    localtime_r(&now, &ti);
    int y = ti.tm_year + 1900;
    int m = ti.tm_mon + 1;
    int d = ti.tm_mday;

    app_manager_display_lock();
    st7306_clear();
    calendar_draw_with_weather(y, m, d, &g_weather);
    if (g_event_count > 0) {
        calendar_set_events(g_events, g_event_count, y, m, d, &g_weather);
    }
    calendar_draw_status_bar(&ti, wifi_manager_get_rssi(), battery_get_level());
    st7306_update_display();
    app_manager_display_unlock();

    // Spawn worker
    if (xTaskCreate(calendar_worker, "cal_work", 8192, NULL, 1, &s_worker) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create calendar worker task");
        s_worker = NULL;
    }
}

void calendar_on_exit(void)
{
    ESP_LOGI(TAG, "Exiting calendar");
    s_stop_flag = true;
    if (s_worker) {
        if (xSemaphoreTake(s_exit_sem, pdMS_TO_TICKS(2000)) != pdPASS) {
            ESP_LOGE(TAG, "Worker did not exit in 2s, force killing");
            vTaskDelete(s_worker);
            s_worker = NULL;
        }
    }
}

void calendar_on_key(key_event_t key)
{
    switch (key) {
        case KEY_PREV:
            calendar_select_day(-1);
            break;
        case KEY_NEXT:
            calendar_select_day(1);
            break;
        case KEY_ENTER:
            calendar_confirm_selection();
            break;
        case KEY_BACK:
            // BACK (4-key hw) or USER-short-press (single-key hw, aliased to
            // BACK by dispatcher):
            // - If a day is selected, clear it and snap cursor back to today.
            // - Otherwise, only return to menu if other apps exist — on the
            //   KinCal-only factory build Calendar is the only app, so BACK
            //   becomes a no-op rather than stranding the user in an empty menu.
            if (calendar_has_selection()) {
                calendar_clear_selection();
                time_t now;
                time(&now);
                struct tm ti;
                localtime_r(&now, &ti);
                app_manager_display_lock();
                calendar_draw_with_weather(ti.tm_year + 1900, ti.tm_mon + 1,
                                           ti.tm_mday, &g_weather);
                if (g_event_count > 0) {
                    calendar_set_events(g_events, g_event_count,
                                        ti.tm_year + 1900, ti.tm_mon + 1,
                                        ti.tm_mday, &g_weather);
                }
                calendar_draw_status_bar(&ti, wifi_manager_get_rssi(),
                                         battery_get_level());
                st7306_update_display();
                app_manager_display_unlock();
                return;
            }
            if (app_registry_count_enabled() > 1) {
                app_manager_switch(APP_ID_MENU);
            } else {
                ESP_LOGI(TAG, "BACK no-op (single-app firmware, no selection)");
            }
            return;
        case KEY_LONG_START:
            // Single-key hardware: long-press is the only way back to MENU
            // when there are multiple apps (USER-short is consumed by the
            // dispatcher's USER→BACK alias for clear-selection). 4-key
            // hardware keeps long-press for CodePilot STT trigger.
            if (!keyboard_has_back_key() && app_registry_count_enabled() > 1) {
                app_manager_switch(APP_ID_MENU);
            }
            return;
        default:
            return;
    }

    // Refresh status bar after day navigation
    app_manager_display_lock();
    time_t now;
    time(&now);
    struct tm ti;
    localtime_r(&now, &ti);
    calendar_draw_status_bar(&ti, wifi_manager_get_rssi(), battery_get_level());
    st7306_update_display();
    app_manager_display_unlock();
}
