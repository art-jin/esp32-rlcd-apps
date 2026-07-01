/*
 * Menu app: vertical list of all apps (skips APP_ID_MENU itself).
 * PREV/NEXT moves cursor, ENTER switches to selected app.
 * Cursor wraps around. Default selection: APP_ID_CALENDAR.
 */

#include "menu_app.h"
#include "app_manager.h"
#include "app_framework.h"
#include "st7306.h"
#include "hzk16.h"
#include "xiaozhi_app_display.h"  // reuse status bar
#include "wifi_manager.h"
#include "battery.h"
#include "esp_log.h"
#include <time.h>

static const char *TAG = "Menu";

// Layout
#define MENU_TOP_Y        60
#define MENU_ITEM_H       32
#define MENU_BOTTOM_Y     260
#define MENU_BOX_X        40
#define MENU_BOX_W        (ST7306_WIDTH - MENU_BOX_X * 2)

// Apps displayed in menu (skip APP_ID_MENU itself).
// Conditional entries are omitted when their Kconfig flag is off — the menu
// naturally shrinks to just Calendar in the KinCal-only factory firmware.
static const app_id_t s_menu_items[] = {
    APP_ID_CALENDAR,
#if CONFIG_KINCAL_APP_XIAOZHI
    APP_ID_XIAOZHI,
#endif
#if CONFIG_KINCAL_APP_CODEPILOT
    APP_ID_CODEPILOT,
#endif
#if CONFIG_KINCAL_APP_SNAKE
    APP_ID_SNAKE,
#endif
#if CONFIG_KINCAL_APP_TETRIS
    APP_ID_TETRIS,
#endif
};
#define MENU_ITEM_COUNT (sizeof(s_menu_items) / sizeof(s_menu_items[0]))

static int s_cursor = 0;  // index into s_menu_items

// Bottom hint strings (GB2312)
static const uint8_t gb_xuanze[] = {0xD1, 0xA1, 0xD4, 0xF1, 0};  // 选择
static const uint8_t gb_jinru[]  = {0xBD, 0xF8, 0xC8, 0xEB, 0};  // 进入

static void draw_status_bar(void)
{
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    xiaozhi_app_draw_status_bar(&ti, wifi_manager_get_rssi(), battery_get_level());
}

static void draw_item(int idx, bool selected)
{
    app_id_t id = s_menu_items[idx];
    const app_t *app = app_registry_get(id);
    if (!app) return;

    int y = MENU_TOP_Y + idx * MENU_ITEM_H;

    app_manager_display_lock();
    if (selected) {
        // Reverse video: black bg, white text
        st7306_draw_filled_rect(MENU_BOX_X, y, MENU_BOX_W, MENU_ITEM_H, ST7306_COLOR_BLACK);
        // ">" indicator on left
        st7306_draw_text(MENU_BOX_X + 4, y + 8, ">", ST7306_COLOR_WHITE);
        // App name (centered in box)
        int name_w = hzk16_text_width((const uint8_t *)app->name);
        int name_x = MENU_BOX_X + (MENU_BOX_W - name_w) / 2;
        hzk16_draw_gb_text(name_x, y + 8, (const uint8_t *)app->name, ST7306_COLOR_WHITE);
    } else {
        // White bg, black text
        st7306_draw_filled_rect(MENU_BOX_X, y, MENU_BOX_W, MENU_ITEM_H, ST7306_COLOR_WHITE);
        st7306_draw_rect(MENU_BOX_X, y, MENU_BOX_W, MENU_ITEM_H, ST7306_COLOR_BLACK);
        int name_w = hzk16_text_width((const uint8_t *)app->name);
        int name_x = MENU_BOX_X + (MENU_BOX_W - name_w) / 2;
        hzk16_draw_gb_text(name_x, y + 8, (const uint8_t *)app->name, ST7306_COLOR_BLACK);
    }
    st7306_update_display();
    app_manager_display_unlock();
}

static void draw_bottom_hint(void)
{
    int y = ST7306_HEIGHT - 24;
    app_manager_display_lock();
    st7306_draw_filled_rect(0, y - 2, ST7306_WIDTH, 24, ST7306_COLOR_WHITE);
    st7306_draw_hline(0, ST7306_WIDTH - 1, y - 2, ST7306_COLOR_BLACK);

    // "<- ->" on left, "OK" hint on right
    st7306_draw_text(8, y, "PREV/NEXT:", ST7306_COLOR_BLACK);
    int x = 8 + 10 * 8;
    hzk16_draw_gb_text(x, y, gb_xuanze, ST7306_COLOR_BLACK);

    int jin_w = 8 * 3 + 16 + hzk16_text_width(gb_jinru);
    int jx = ST7306_WIDTH - jin_w - 8;
    st7306_draw_text(jx, y, "OK:", ST7306_COLOR_BLACK);
    jx += 3 * 8;
    hzk16_draw_gb_text(jx, y, gb_jinru, ST7306_COLOR_BLACK);
    st7306_update_display();
    app_manager_display_unlock();
}

void menu_on_enter(void)
{
    ESP_LOGI(TAG, "Entering menu, cursor=%d (%s)", s_cursor,
             app_registry_get(s_menu_items[s_cursor])->name);

    app_manager_display_lock();
    st7306_clear();
    st7306_draw_filled_rect(0, 0, ST7306_WIDTH, ST7306_HEIGHT, ST7306_COLOR_WHITE);

    // Status bar
    draw_status_bar();

    // Separator above first menu item
    st7306_draw_hline(0, ST7306_WIDTH - 1, MENU_TOP_Y - 4, ST7306_COLOR_BLACK);

    // All items
    for (int i = 0; i < (int)MENU_ITEM_COUNT; i++) {
        int y = MENU_TOP_Y + i * MENU_ITEM_H;
        bool selected = (i == s_cursor);
        if (selected) {
            st7306_draw_filled_rect(MENU_BOX_X, y, MENU_BOX_W, MENU_ITEM_H, ST7306_COLOR_BLACK);
            st7306_draw_text(MENU_BOX_X + 4, y + 8, ">", ST7306_COLOR_WHITE);
            const app_t *app = app_registry_get(s_menu_items[i]);
            int name_w = hzk16_text_width((const uint8_t *)app->name);
            int name_x = MENU_BOX_X + (MENU_BOX_W - name_w) / 2;
            hzk16_draw_gb_text(name_x, y + 8, (const uint8_t *)app->name, ST7306_COLOR_WHITE);
        } else {
            st7306_draw_rect(MENU_BOX_X, y, MENU_BOX_W, MENU_ITEM_H, ST7306_COLOR_BLACK);
            const app_t *app = app_registry_get(s_menu_items[i]);
            int name_w = hzk16_text_width((const uint8_t *)app->name);
            int name_x = MENU_BOX_X + (MENU_BOX_W - name_w) / 2;
            hzk16_draw_gb_text(name_x, y + 8, (const uint8_t *)app->name, ST7306_COLOR_BLACK);
        }
    }

    st7306_update_display();
    app_manager_display_unlock();

    draw_bottom_hint();
}

void menu_on_exit(void)
{
    ESP_LOGI(TAG, "Exiting menu");
}

void menu_on_key(key_event_t key)
{
    int old_cursor = s_cursor;
    switch (key) {
        case KEY_PREV:
            s_cursor = (s_cursor - 1 + (int)MENU_ITEM_COUNT) % MENU_ITEM_COUNT;
            break;
        case KEY_NEXT:
            s_cursor = (s_cursor + 1) % MENU_ITEM_COUNT;
            break;
        case KEY_ENTER:
            ESP_LOGI(TAG, "ENTER: switching to app %d", s_menu_items[s_cursor]);
            app_manager_switch(s_menu_items[s_cursor]);
            return;
        default:
            return;
    }
    if (s_cursor != old_cursor) {
        ESP_LOGI(TAG, "Cursor moved: %d -> %d", old_cursor, s_cursor);
        draw_item(old_cursor, false);
        draw_item(s_cursor, true);
    }
}

void menu_on_tick_1s(void)
{
    app_manager_display_lock();
    draw_status_bar();
    st7306_update_display();
    app_manager_display_unlock();
}
