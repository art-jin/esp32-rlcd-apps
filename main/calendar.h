#ifndef CALENDAR_H
#define CALENDAR_H

#include <stdbool.h>
#include "weather.h"
#include "caldav.h"
#include <time.h>

/**
 * Draw the monthly calendar view for the given date (mock weather).
 */
void calendar_draw(int year, int month, int day);

/**
 * Draw the monthly calendar with real weather data.
 * Does NOT call st7306_update_display() — caller should draw status bar first.
 */
void calendar_draw_with_weather(int year, int month, int day, const weather_data_t *weather);

/**
 * Update only the right panel (weather + todos) without full redraw.
 * Modifies frame buffer only — caller must call st7306_update_display().
 */
void calendar_update_weather(const weather_data_t *weather);

/**
 * Set calendar events (from CalDAV) and redraw the right panel.
 * Separates today's events from upcoming events.
 */
void calendar_set_events(const caldav_event_t *events, int count,
                         int today_year, int today_month, int today_day,
                         const weather_data_t *weather);

/**
 * Draw the bottom status bar: clock, WiFi signal bars, battery icon.
 * Modifies frame buffer only — caller must call st7306_update_display().
 */
void calendar_draw_status_bar(struct tm *timeinfo, int rssi, int battery_pct);

/**
 * Draw the WiFi config prompt screen (AP mode).
 */
void calendar_draw_config(const char *ap_ssid);

/**
 * Draw a status message screen (centered text).
 */
void calendar_draw_status(const char *line1, const char *line2);

/**
 * Redraw the entire left panel (weather + events).
 */
void calendar_redraw_left_panel(const weather_data_t *weather);

/**
 * Draw a "室内 X°C 湿度 Y%" line at the bottom of the left panel.
 * Modifies frame buffer only — caller must call st7306_update_display().
 */
void calendar_draw_env_data(float temp_c, float humidity_pct);

/**
 * Draw WiFi signal bars (4 bars of increasing height).
 * Public so XiaoZhi app display can reuse it.
 */
void calendar_draw_wifi_bars(int x, int y, int rssi);

/**
 * Draw battery icon + fill level.
 * Public so XiaoZhi app display can reuse it.
 */
void calendar_draw_battery_icon(int x, int y, int pct);

/**
 * Move the day selection cursor by delta days (+1 or -1).
 * Redraws the calendar grid with the cursor.
 */
void calendar_select_day(int delta);

/**
 * Confirm the current selection (log + redraw).
 */
void calendar_confirm_selection(void);

/**
 * Clear the selection state (used when switching apps).
 */
void calendar_clear_selection(void);

/**
 * Whether a non-today day is currently selected (cursor moved off today).
 */
bool calendar_has_selection(void);

#endif // CALENDAR_H
