#ifndef APP_MANAGER_H
#define APP_MANAGER_H

#include "app_framework.h"

/**
 * Initialize app manager: install GPIO ISR service, init keyboard,
 * spawn dispatcher task. Starts in APP_ID_MENU mode.
 */
void app_manager_init(void);

/**
 * Switch to a different app. Cooperative: calls current app's on_exit
 * (which waits for its worker task to self-delete), then target's on_enter.
 * No-op if target == current.
 */
void app_manager_switch(app_id_t target);

/**
 * Thread-safe: which app is currently active?
 */
app_id_t app_manager_current(void);

/**
 * Display sequence lock: wrap multi-call draw sequences to prevent
 * tearing when worker tasks and dispatcher both render.
 * Uses recursive mutex — same task can lock multiple times.
 */
void app_manager_display_lock(void);
void app_manager_display_unlock(void);

#endif // APP_MANAGER_H
