/*
 * App registry: static registration of all apps.
 * MENU + CALENDAR are always registered. XIAOZHI / CODEPILOT / SNAKE / TETRIS
 * are conditionally compiled via Kconfig (`CONFIG_KINCAL_APP_*`).
 *
 * The app_id_t enum stays fixed for binary compatibility — disabled apps
 * keep their enum slot but their `s_apps[id]` entry is left zero-initialized
 * (NULL function pointers). Callers must NULL-check `on_enter`/`on_key` or
 * use `app_registry_get()` which returns NULL for disabled entries.
 */

#include "app_framework.h"
#include "menu_app.h"
#include "calendar_app.h"
#include "xiaozhi_app.h"
#include "codepilot_app.h"
#include "snake_app.h"
#include "tetris_app.h"
#include "tower_defense_app.h"
#include "placeholder_app.h"

// GB2312 pre-encoded display names (always-on apps)
static const uint8_t nm_menu[]     = {0xD6, 0xF7, 0xB2, 0xCB, 0xB5, 0xA5, 0};        // 主菜单
static const uint8_t nm_calendar[] = {0xC8, 0xD5, 0xC0, 0xFA, 0};                    // 日历
#if CONFIG_KINCAL_APP_XIAOZHI
static const uint8_t nm_xiaozhi[]  = {0xD0, 0xA1, 0xD6, 0xC7, 0};                    // 小智
#endif
#if CONFIG_KINCAL_APP_CODEPILOT
static const char    nm_codepilot[] = "Code Pilot";
#endif
#if CONFIG_KINCAL_APP_SNAKE
static const uint8_t nm_snake[]    = {0xCC, 0xB0, 0xB3, 0xD4, 0xC9, 0xDF, 0};        // 贪吃蛇
#endif
#if CONFIG_KINCAL_APP_TETRIS
static const uint8_t nm_tetris[]   = {0xB6, 0xED, 0xC2, 0xDE, 0xCB, 0xB9,
                                       0xB7, 0xBD, 0xBF, 0xE9, 0};                   // 俄罗斯方块
#endif
#if CONFIG_KINCAL_APP_TOWER
static const uint8_t nm_tower[]    = {0xC5, 0xDA, 0xCB, 0xFE, 0xB7, 0xC0, 0xCA, 0xD8, 0}; // 炮塔防守
#endif

static const app_t s_apps[APP_ID_COUNT] = {
    [APP_ID_MENU] = {
        APP_ID_MENU, (const char *)nm_menu,
        menu_on_enter, menu_on_exit, menu_on_key, menu_on_tick_1s,
    },
    [APP_ID_CALENDAR] = {
        APP_ID_CALENDAR, (const char *)nm_calendar,
        calendar_on_enter, calendar_on_exit, calendar_on_key, NULL,
    },
#if CONFIG_KINCAL_APP_XIAOZHI
    [APP_ID_XIAOZHI] = {
        APP_ID_XIAOZHI, (const char *)nm_xiaozhi,
        xiaozhi_app_on_enter, xiaozhi_app_on_exit,
        xiaozhi_app_on_key, xiaozhi_app_on_tick_1s,
    },
#endif
#if CONFIG_KINCAL_APP_CODEPILOT
    [APP_ID_CODEPILOT] = {
        APP_ID_CODEPILOT, nm_codepilot,
        codepilot_on_enter, codepilot_on_exit,
        codepilot_on_key, codepilot_on_tick_1s,
    },
#endif
#if CONFIG_KINCAL_APP_SNAKE
    [APP_ID_SNAKE] = {
        APP_ID_SNAKE, (const char *)nm_snake,
        snake_on_enter, snake_on_exit, snake_on_key, NULL,
    },
#endif
#if CONFIG_KINCAL_APP_TETRIS
    [APP_ID_TETRIS] = {
        APP_ID_TETRIS, (const char *)nm_tetris,
        tetris_on_enter, tetris_on_exit, tetris_on_key, NULL,
    },
#endif
#if CONFIG_KINCAL_APP_TOWER
    [APP_ID_TOWER] = {
        APP_ID_TOWER, (const char *)nm_tower,
        tower_on_enter, tower_on_exit, tower_on_key, NULL,
    },
#endif
};

const app_t *app_registry_get(app_id_t id)
{
    if (id < 0 || id >= APP_ID_COUNT) return NULL;
    /* Disabled apps have NULL on_enter — surface as NULL to caller. */
    if (s_apps[id].on_enter == NULL) return NULL;
    return &s_apps[id];
}

const app_t *app_registry_all(void)
{
    return s_apps;
}

int app_registry_count_enabled(void)
{
    int n = 0;
    for (int i = 0; i < (int)APP_ID_COUNT; i++) {
        if (i == APP_ID_MENU) continue;
        if (s_apps[i].on_enter != NULL) n++;
    }
    return n;
}
