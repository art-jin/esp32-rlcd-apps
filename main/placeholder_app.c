/*
 * Placeholder app: shows "X 开发中" + BACK returns to menu.
 * Used for CodePilot/Snake/Tetris until real implementations land in Phase B/C/D.
 */

#include "placeholder_app.h"
#include "app_manager.h"
#include "app_framework.h"
#include "st7306.h"
#include "hzk16.h"
#include "xiaozhi_app_display.h"  // reuse status bar
#include "wifi_manager.h"
#include "battery.h"
#include "esp_log.h"
#include <time.h>

static const char *TAG = "Placeholder";

// GB2312 pre-encoded strings
static const uint8_t gb_kaifa[] = {0xBF, 0xAA, 0xB7, 0xA2, 0xD6, 0xD0, 0};  // 开发中
static const uint8_t gb_qing[]  = {0xC7, 0xEB, 0xB5, 0xC8, 0xB4, 0xFD, 0};  // 请等待
static const uint8_t gb_fan[]   = {0xB7, 0xB5, 0xBB, 0xD8, 0};              // 返回
static const uint8_t gb_caidan[] = {0xB2, 0xCB, 0xB5, 0xA5, 0};             // 菜单

void placeholder_on_enter(void)
{
    app_id_t id = app_manager_current();
    const app_t *app = app_registry_get(id);
    const char *name = app ? app->name : "?";

    app_manager_display_lock();
    st7306_clear();

    // Top status bar (reuse XiaoZhi's status bar renderer)
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    xiaozhi_app_draw_status_bar(&ti, wifi_manager_get_rssi(), battery_get_level());

    // Centered "X" (app name) + "开发中" + "请等待"
    int y = 100;
    int name_w = hzk16_text_width((const uint8_t *)name);
    hzk16_draw_gb_text((ST7306_WIDTH - name_w) / 2, y, (const uint8_t *)name, ST7306_COLOR_BLACK);
    y += 30;

    int kf_w = hzk16_text_width(gb_kaifa);
    hzk16_draw_gb_text((ST7306_WIDTH - kf_w) / 2, y, gb_kaifa, ST7306_COLOR_BLACK);
    y += 24;

    int qing_w = hzk16_text_width(gb_qing);
    hzk16_draw_gb_text((ST7306_WIDTH - qing_w) / 2, y, gb_qing, ST7306_COLOR_BLACK);

    // Bottom hint: "返回菜单"
    int hint_y = ST7306_HEIGHT - 24;
    st7306_draw_hline(0, ST7306_WIDTH - 1, hint_y - 2, ST7306_COLOR_BLACK);
    int hint_w = hzk16_text_width(gb_fan) + 16 + hzk16_text_width(gb_caidan);
    int hx = (ST7306_WIDTH - hint_w) / 2;
    st7306_draw_text(hx, hint_y, "BACK:", ST7306_COLOR_BLACK);
    hx += 6 * 8;
    hzk16_draw_gb_text(hx, hint_y, gb_fan, ST7306_COLOR_BLACK);
    hx += 16 + 16;
    hzk16_draw_gb_text(hx, hint_y, gb_caidan, ST7306_COLOR_BLACK);

    st7306_update_display();
    app_manager_display_unlock();

    ESP_LOGI(TAG, "Entered placeholder for app %d (%s)", id, name);
}

void placeholder_on_exit(void)
{
    ESP_LOGI(TAG, "Exiting placeholder");
}

void placeholder_on_key(key_event_t key)
{
    if (key == KEY_BACK) {
        app_manager_switch(APP_ID_MENU);
    }
}
