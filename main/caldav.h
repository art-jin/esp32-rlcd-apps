#ifndef CALDAV_H
#define CALDAV_H

#include <stdint.h>
#include "esp_err.h"

/**
 * A single calendar event parsed from CalDAV iCal data.
 */
typedef struct {
    char summary[64];        /* UTF-8 event title (raw from Feishu) */
    uint8_t summary_gb[128]; /* GB2312 converted title for HZK16 rendering */
    int year, month, day;
    int start_hour, start_min;
    int end_hour, end_min;
} caldav_event_t;

#define CALDAV_MAX_EVENTS 15

/**
 * Fetch calendar events from Feishu CalDAV for the given date range.
 * Queries today + 7 days ahead.
 *
 * @param year     Current year
 * @param month    Current month
 * @param day      Current day
 * @param events   Output array of caldav_event_t (caller allocated, min CALDAV_MAX_EVENTS)
 * @param out_count Number of events written to the array
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t caldav_fetch_events(int year, int month, int day,
                              caldav_event_t *events, int *out_count);

#endif /* CALDAV_H */
