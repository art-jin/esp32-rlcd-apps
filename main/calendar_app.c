/*
 * Calendar app: encapsulates the monthly calendar view + 3-key navigation.
 * Worker task handles 1Hz status bar refresh + periodic weather/CalDAV fetches.
 * on_key handles PREV/NEXT/ENTER day navigation; BACK returns to menu.
 */

#include "calendar_app.h"
#include "app_manager.h"
#include "calendar.h"
#include "weather.h"
#include "caldav.h"
#include "wifi_manager.h"
#include "battery.h"
#include "shtc3.h"
#include "st7306.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <time.h>

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
static int s_weather_counter = 0;
static int s_caldav_counter = 0;
static int s_env_counter = 60;   // force first-loop SHTC3 read

// Local helper (kept identical to old fetch_and_set_events in app_manager.c)
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

static void calendar_worker(void *arg)
{
    int sub_tick = 0;
    ESP_LOGI(TAG, "Worker started");

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

        // Weather refresh (every 10 min)
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

        // CalDAV refresh (initial burst at counter=5, then every 10 min)
        if (++s_caldav_counter >= 600 ||
            (s_caldav_counter == 5 && g_event_count == 0)) {
            s_caldav_counter = 0;
            fetch_and_set_events(y, m, d);
        }

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
    s_weather_counter = 599;
    s_caldav_counter = 0;
    s_env_counter = 60;

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
            app_manager_switch(APP_ID_MENU);
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
