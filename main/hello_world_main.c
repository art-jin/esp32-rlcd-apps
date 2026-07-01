/*
 * ESP32-S3-RLCD-4.2 entry point.
 *
 * Default data source: KinCal JSON endpoint (`/api/v1/esp32/display/{code}`).
 * The short code is loaded from NVS (provisioned via AP captive portal).
 * If the code is missing, the calendar worker falls back to local static
 * holiday/lunar data and retries KinCal every 60 s.
 *
 * Escape hatch: `CONFIG_KINCAL_LEGACY_DIRECT_APIS=y` re-enables the old
 * direct Feishu CalDAV + Open-Meteo paths.
 */

#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "st7306.h"
#include "hzk16.h"
#include "calendar.h"
#include "wifi_manager.h"
#include "battery.h"
#include "xiaozhi_bridge.h"
#include "shtc3.h"
#include "app_manager.h"

#if CONFIG_KINCAL_LEGACY_DIRECT_APIS
#include "weather.h"
#include "caldav.h"
#else
#include "kincal_client.h"
#endif

// Forward declarations for XiaoZhi display callbacks (xiaozhi_display.c)
extern void xiaozhi_on_text(const char *text);
extern void xiaozhi_on_state(xiaozhi_state_t state);

// Shared data (accessible by calendar_app worker)
weather_data_t g_weather = {
    .temperature = 25,
    .humidity = 55,
    .weather_code = 2,
    .description = (const uint8_t *)"\xB6\xE2\xD4\xC6\x00",  // 多云 (default)
};

caldav_event_t g_events[CALDAV_MAX_EVENTS];
int g_event_count = 0;

void app_main(void)
{
    printf("Initializing...\n");

    ESP_ERROR_CHECK(st7306_init());
    ESP_ERROR_CHECK(hzk16_init());

    // Init NVS (required for WiFi credentials + KinCal short code)
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    // Initialize WiFi (AP config mode or STA connect)
    esp_err_t wifi_ret = wifi_manager_init();

    if (wifi_ret == ESP_ERR_NOT_FOUND) {
        // AP config mode — no saved credentials
        printf("AP config mode. Waiting for WiFi setup...\n");
        calendar_draw_config(wifi_manager_get_ap_ssid());
        while (1) { vTaskDelay(pdMS_TO_TICKS(60000)); }
    }

    if (wifi_ret != ESP_OK) {
        printf("WiFi connection failed, using fallback date.\n");
        calendar_draw_status("WiFi connect failed", "Using fallback date");
        vTaskDelay(pdMS_TO_TICKS(3000));
        battery_init();
        calendar_draw(2026, 5, 8);
        app_manager_init();
        return;
    }

    // WiFi connected — sync time via SNTP
    printf("WiFi connected. Syncing time...\n");
    calendar_draw_status("WiFi connected", "Syncing time...");

    struct tm timeinfo;
    int year = 2026, month = 5, day = 8;  // fallback

    if (wifi_manager_get_time(&timeinfo)) {
        year  = timeinfo.tm_year + 1900;
        month = timeinfo.tm_mon + 1;
        day   = timeinfo.tm_mday;
        printf("SNTP time: %d-%02d-%02d\n", year, month, day);
    } else {
        printf("SNTP sync failed, using fallback date.\n");
    }

    // Initialize battery ADC
    battery_init();

#if CONFIG_KINCAL_LEGACY_DIRECT_APIS
    // Legacy: fetch weather up front (calendar_app worker continues every 10 min)
    printf("Fetching weather (legacy)...\n");
    if (weather_fetch(&g_weather) == ESP_OK) {
        printf("Weather: %.1f C, %d%% humidity\n", g_weather.temperature, g_weather.humidity);
    } else {
        printf("Weather fetch failed, using defaults.\n");
    }
#else
    // KinCal: cache short code from NVS. If not provisioned, calendar_app
    // worker will retry every 60 s and fall back to local lunar/holiday data.
    kincal_client_init();
    if (!kincal_client_is_provisioned()) {
        printf("KinCal short code not set — running in fallback mode.\n");
        calendar_draw_status("KinCal unpaired", "Re-run AP portal");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
#endif

    // Initialize XiaoZhi hardware upfront so I2C bus is available for SHTC3.
    // xiaozhi_init() is idempotent; later xiaozhi_app.on_enter will be a no-op.
    xiaozhi_init();

    // Initialize SHTC3 sensor (reuses XiaoZhi I2C bus, address 0x70).
    // Failure is non-fatal — env data just won't show.
    shtc3_init();

    // Draw initial calendar with weather
    calendar_draw_with_weather(year, month, day, &g_weather);

    // Register XiaoZhi callbacks (before app_manager_init which may init XiaoZhi)
    xiaozhi_register_text_cb(xiaozhi_on_text);
    xiaozhi_register_state_cb(xiaozhi_on_state);

    // Start app manager (creates update_task for calendar, handles KEY button)
    app_manager_init();

    printf("Calendar rendered. App manager running.\n");
}

