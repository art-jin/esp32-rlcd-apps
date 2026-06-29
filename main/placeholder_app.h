#ifndef PLACEHOLDER_APP_H
#define PLACEHOLDER_APP_H

#include "app_framework.h"

/**
 * Generic placeholder app for not-yet-implemented apps (CodePilot/Snake/Tetris).
 * Shows "X 开发中" + BACK returns to menu.
 * Uses app_manager_current() to know which app is active.
 */
void placeholder_on_enter(void);
void placeholder_on_exit(void);
void placeholder_on_key(key_event_t key);

#endif // PLACEHOLDER_APP_H
