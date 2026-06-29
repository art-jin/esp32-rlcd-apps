/*
 * XiaoZhi app: encapsulates the XiaoZhi voice assistant UI + lifecycle.
 * Worker task runs xiaozhi_run() (which now exits cleanly on XZ_EVT_KILL).
 * on_key: BACK returns to menu, ENTER triggers conversation (BOOT also works).
 * on_tick_1s: refreshes status bar clock (fixes pre-existing bug).
 */

#include "xiaozhi_app.h"
#include "app_manager.h"
#include "xiaozhi_bridge.h"
#include "xiaozhi_app_display.h"
#include "wifi_manager.h"
#include "battery.h"
#include "st7306.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <time.h>

static const char *TAG = "XiaoZhiApp";

static volatile bool s_stop_flag = false;
static SemaphoreHandle_t s_exit_sem = NULL;
static TaskHandle_t s_worker = NULL;

static void xiaozhi_worker(void *arg)
{
    ESP_LOGI(TAG, "Worker started, calling xiaozhi_run()");
    // xiaozhi_run now returns when XZ_EVT_KILL is set
    xiaozhi_run();
    ESP_LOGI(TAG, "xiaozhi_run returned, worker exiting");
    xSemaphoreGive(s_exit_sem);
    s_worker = NULL;
    vTaskDelete(NULL);
}

void xiaozhi_app_on_enter(void)
{
    ESP_LOGI(TAG, "Entering XiaoZhi");

    s_stop_flag = false;
    if (!s_exit_sem) s_exit_sem = xSemaphoreCreateBinary();
    xSemaphoreTake(s_exit_sem, 0);  // ensure empty

    // xiaozhi_init() is idempotent (returns ESP_OK if already done).
    // First call: full audio init at boot. Subsequent: prepare_reconnect.
    if (xiaozhi_init() != ESP_OK) {
        ESP_LOGE(TAG, "xiaozhi_init failed on entry");
    }
    xiaozhi_prepare_reconnect();

    // Initial UI
    app_manager_display_lock();
    st7306_clear();
    xiaozhi_app_draw_full(XZ_IDLE, NULL);
    st7306_update_display();
    app_manager_display_unlock();

    // Spawn worker task (32KB stack for Opus + WS + TLS)
    if (xTaskCreate(xiaozhi_worker, "xz_app", 32768, NULL, 1, &s_worker) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create xiaozhi worker task");
        s_worker = NULL;
    }
}

void xiaozhi_app_on_exit(void)
{
    ESP_LOGI(TAG, "Exiting XiaoZhi");
    s_stop_flag = true;

    if (s_worker) {
        // Force xiaozhi_run to exit + close WS/pipeline
        xiaozhi_force_disconnect();

        // Wait for worker to actually finish (≤2s)
        if (xSemaphoreTake(s_exit_sem, pdMS_TO_TICKS(2000)) != pdPASS) {
            ESP_LOGE(TAG, "Worker did not exit in 2s, force killing");
            vTaskDelete(s_worker);
            s_worker = NULL;
        }
    }

    // Always disable audio to silence mic/speaker between sessions
    xiaozhi_disable_audio();
}

void xiaozhi_app_on_key(key_event_t key)
{
    switch (key) {
        case KEY_BACK:
            app_manager_switch(APP_ID_MENU);
            return;
        case KEY_ENTER:
            // ENTER also triggers conversation (in addition to BOOT)
            xiaozhi_start_listening();
            return;
        default:
            return;
    }
}

void xiaozhi_app_on_tick_1s(void)
{
    // Refresh status bar clock + battery + WiFi (fixes pre-existing bug
    // where XiaoZhi clock didn't update because only xiaozhi_app_draw_full
    // drew the status bar once at enter)
    app_manager_display_lock();
    time_t now;
    time(&now);
    struct tm ti;
    localtime_r(&now, &ti);
    xiaozhi_app_draw_status_bar(&ti, wifi_manager_get_rssi(), battery_get_level());
    st7306_update_display();
    app_manager_display_unlock();
}
