/*
 * App Manager — Phase A4 dispatcher + lifecycle.
 *
 * Single dispatcher task routes key events and 1Hz ticks to the current
 * app's hooks. Switching is cooperative: on_exit sets stop_flag, app's
 * worker self-deletes via vTaskDelete(NULL), on_exit waits on semaphore
 * (2s timeout, force-kill fallback).
 *
 * GPIO18 (USER) and GPIO43 (BACK) are owned by keyboard.c (Phase A2).
 */

#include "app_manager.h"
#include "st7306.h"
#include "keyboard.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"

static const char *TAG = "AppManager";

#define APP_MGR_STACK     8192
#define APP_MGR_PRIO      2
#define DISPATCHER_TICK_MS 1000

static const app_t *s_current = NULL;
static TaskHandle_t s_dispatcher = NULL;
static SemaphoreHandle_t s_display_mutex = NULL;  // recursive

// Forward declarations
static void dispatcher_task(void *arg);

static void enter_app(app_id_t id)
{
    const app_t *app = app_registry_get(id);
    if (!app) {
        ESP_LOGE(TAG, "enter_app: invalid id %d", id);
        return;
    }
    ESP_LOGI(TAG, "Entering app: %s", app->name);
    s_current = app;
    if (app->on_enter) app->on_enter();
}

static void exit_app(void)
{
    if (!s_current) return;
    ESP_LOGI(TAG, "Exiting app: %s", s_current->name);
    if (s_current->on_exit) s_current->on_exit();
    s_current = NULL;
}

static void dispatcher_task(void *arg)
{
    ESP_LOGI(TAG, "Dispatcher task started");

    // Single-App factory firmware (Calendar-only): skip MENU entirely so
    // single-key hardware (only GPIO18 available) isn't stranded on a
    // screen that requires KEY_ENTER to leave. Multi-App builds still
    // boot into MENU as before.
    app_id_t initial = (app_registry_count_enabled() == 1)
                       ? APP_ID_CALENDAR
                       : APP_ID_MENU;
    enter_app(initial);

    while (1) {
        key_event_t key = keyboard_wait(pdMS_TO_TICKS(DISPATCHER_TICK_MS));

        if (key != KEY_NONE) {
            // GPIO18 (USER) event routing on single-key hardware (no BACK key):
            //   - MENU app: receives raw KEY_USER (cycle) + KEY_DOUBLE_CLICK (enter)
            //   - Other apps: KEY_USER is aliased to KEY_BACK so existing apps
            //     that only handle KEY_BACK (snake/tetris/xiaozhi/codepilot)
            //     keep working without modification. KEY_DOUBLE_CLICK and
            //     KEY_LONG_* are passed through (apps may opt-in to handle).
            // 4-key hardware: every event delivered as-is.
            if (key == KEY_USER && !keyboard_has_back_key() &&
                s_current && s_current->id != APP_ID_MENU) {
                key = KEY_BACK;
            }
            if (s_current && s_current->on_key) {
                s_current->on_key(key);
            }
        } else {
            // Tick: only when key wait timed out (1s elapsed)
            if (s_current && s_current->on_tick_1s) {
                s_current->on_tick_1s();
            }
        }
    }
}

void app_manager_init(void)
{
    // Install ISR service (keyboard.c depends on this; idempotent)
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "GPIO ISR service install failed: %s", esp_err_to_name(ret));
    }

    // Recursive mutex so the same task can lock multiple times
    s_display_mutex = xSemaphoreCreateRecursiveMutex();

    // Initialize keyboard (GPIO1/3/17/43 + GPIO18 long-press)
    keyboard_init();

    // Spawn dispatcher task (single-threaded app.on_key/on_tick_1s callers)
    xTaskCreate(dispatcher_task, "dispatcher", APP_MGR_STACK, NULL, APP_MGR_PRIO, &s_dispatcher);

    ESP_LOGI(TAG, "App manager initialized (dispatcher task, MENU mode)");
}

void app_manager_switch(app_id_t target)
{
    if (!s_current) {
        ESP_LOGE(TAG, "switch: no current app");
        return;
    }
    if (target == s_current->id) {
        ESP_LOGI(TAG, "switch: already in %d, no-op", target);
        return;
    }
    if (target < 0 || target >= APP_ID_COUNT) {
        ESP_LOGE(TAG, "switch: invalid target %d", target);
        return;
    }
    ESP_LOGI(TAG, "switch: %s -> %s", s_current->name, app_registry_get(target)->name);
    exit_app();
    enter_app(target);
}

app_id_t app_manager_current(void)
{
    return s_current ? s_current->id : APP_ID_MENU;
}

void app_manager_display_lock(void)
{
    if (s_display_mutex) {
        xSemaphoreTakeRecursive(s_display_mutex, portMAX_DELAY);
    }
}

void app_manager_display_unlock(void)
{
    if (s_display_mutex) {
        xSemaphoreGiveRecursive(s_display_mutex);
    }
}
