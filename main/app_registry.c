/*
 * App registry: static registration of all 6 apps.
 * MENU/CALENDAR/XIAOZHI are real implementations (Phase A).
 * CODEPILOT/SNAKE/TETRIS use placeholder until Phase B/C/D.
 */

#include "app_framework.h"
#include "menu_app.h"
#include "calendar_app.h"
#include "xiaozhi_app.h"
#include "codepilot_app.h"
#include "snake_app.h"
#include "tetris_app.h"
#include "placeholder_app.h"

// GB2312 pre-encoded display names
static const uint8_t nm_menu[]     = {0xD6, 0xF7, 0xB2, 0xCB, 0xB5, 0xA5, 0};        // 主菜单
static const uint8_t nm_calendar[] = {0xC8, 0xD5, 0xC0, 0xFA, 0};                    // 日历
static const uint8_t nm_xiaozhi[]  = {0xD0, 0xA1, 0xD6, 0xC7, 0};                    // 小智
static const char    nm_codepilot[] = "CodePilot";
static const uint8_t nm_snake[]    = {0xCC, 0xB0, 0xB3, 0xD4, 0xC9, 0xDF, 0};        // 贪吃蛇
static const uint8_t nm_tetris[]   = {0xB6, 0xED, 0xC2, 0xDE, 0xCB, 0xB9,
                                       0xB7, 0xBD, 0xBF, 0xE9, 0};                   // 俄罗斯方块

static const app_t s_apps[APP_ID_COUNT] = {
    [APP_ID_MENU]      = { APP_ID_MENU,      (const char *)nm_menu,
                           menu_on_enter, menu_on_exit, menu_on_key, menu_on_tick_1s },
    [APP_ID_CALENDAR]  = { APP_ID_CALENDAR,  (const char *)nm_calendar,
                           calendar_on_enter, calendar_on_exit, calendar_on_key, NULL },
    [APP_ID_XIAOZHI]   = { APP_ID_XIAOZHI,   (const char *)nm_xiaozhi,
                           xiaozhi_app_on_enter, xiaozhi_app_on_exit,
                           xiaozhi_app_on_key, xiaozhi_app_on_tick_1s },
    [APP_ID_CODEPILOT] = { APP_ID_CODEPILOT, nm_codepilot,
                           codepilot_on_enter, codepilot_on_exit,
                           codepilot_on_key, codepilot_on_tick_1s },
    [APP_ID_SNAKE]     = { APP_ID_SNAKE,     (const char *)nm_snake,
                           snake_on_enter, snake_on_exit, snake_on_key, NULL },
    [APP_ID_TETRIS]    = { APP_ID_TETRIS,    (const char *)nm_tetris,
                           tetris_on_enter, tetris_on_exit, tetris_on_key, NULL },
};

const app_t *app_registry_get(app_id_t id)
{
    if (id < 0 || id >= APP_ID_COUNT) return NULL;
    return &s_apps[id];
}

const app_t *app_registry_all(void)
{
    return s_apps;
}
