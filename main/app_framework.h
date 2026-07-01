#ifndef APP_FRAMEWORK_H
#define APP_FRAMEWORK_H

#include <stdbool.h>
#include "keyboard.h"

/**
 * Application identifiers (order = menu display order)
 */
typedef enum {
    APP_ID_MENU = 0,
    APP_ID_CALENDAR,
    APP_ID_XIAOZHI,
    APP_ID_CODEPILOT,
    APP_ID_SNAKE,
    APP_ID_TETRIS,
    APP_ID_COUNT,
} app_id_t;

/**
 * App lifecycle interface.
 * - on_enter: alloc resources, draw first screen, spawn worker task
 * - on_exit: cooperative — set stop_flag, wait for worker self-delete (≤2s)
 * - on_key: route key events (called from dispatcher task, single-threaded)
 * - on_tick_1s: optional 1Hz heartbeat (NULL = no tick)
 */
typedef struct {
    app_id_t id;
    const char *name;            // GB2312 pre-encoded byte string for menu display
    void (*on_enter)(void);
    void (*on_exit)(void);
    void (*on_key)(key_event_t key);
    void (*on_tick_1s)(void);    // may be NULL
} app_t;

const app_t *app_registry_get(app_id_t id);
const app_t *app_registry_all(void);   // APP_ID_COUNT entries

/**
 * Number of enabled apps (excluding APP_ID_MENU itself). Used to decide
 * whether BACK should return to the menu or do something app-local.
 * Returns 0 in KinCal factory firmware where Calendar is the only visible app.
 */
int app_registry_count_enabled(void);

#endif // APP_FRAMEWORK_H
