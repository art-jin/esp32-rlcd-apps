#ifndef XIAOZHI_APP_DISPLAY_H
#define XIAOZHI_APP_DISPLAY_H

#include "xiaozhi_bridge.h"
#include <time.h>

/**
 * Draw the full XiaoZhi app screen (call when entering XiaoZhi mode).
 */
void xiaozhi_app_draw_full(xiaozhi_state_t state, const char *text);

/**
 * Update state indicator only (center area + status bar).
 * Modifies frame buffer — caller should call st7306_update_display().
 */
void xiaozhi_app_update_state(xiaozhi_state_t state);

/**
 * Update text output area only (chat region).
 * Modifies frame buffer — caller should call st7306_update_display().
 */
void xiaozhi_app_update_text(const char *text);

/**
 * Draw XiaoZhi status bar (top area: WiFi, clock, battery).
 */
void xiaozhi_app_draw_status_bar(struct tm *ti, int rssi, int battery_pct);

#endif // XIAOZHI_APP_DISPLAY_H
