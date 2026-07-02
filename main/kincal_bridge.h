#ifndef KINCAL_BRIDGE_H
#define KINCAL_BRIDGE_H

#include "kincal_client.h"
#include <stdbool.h>

/**
 * Apply a freshly-fetched KinCal payload to the calendar.c rendering state.
 *
 * Side effects (in order):
 *   1. Sets g_kincal_active = true, populates g_kincal_rest_days / event_dates
 *   2. Converts lunar_date_text UTF-8 → GB2312 into g_kincal_lunar_text
 *   3. If has_weather, writes g_weather (temperature / humidity / description)
 *   4. Flattens events_today + upcoming into g_events[] with year/month/day
 *      tags so calendar_set_events can re-split today vs future
 *
 * Caller is responsible for invoking calendar_set_events / calendar_draw_*
 * afterwards to reflect the new state on screen.
 */
void kincal_bridge_apply(const kincal_display_data_t *d);

/**
 * Clear KinCal override state (called when the device loses pairing or
 * falls back to legacy data sources). After this, calendar.c reverts to
 * its local static holiday table + lunar computation.
 */
void kincal_bridge_clear(void);

/* True iff the most recent KinCal response included the weather object.
 * calendar_app.c reads this to fall back to SHTC3 indoor readings when
 * the plan doesn't include outdoor weather. */
extern bool g_kincal_has_weather;

#endif /* KINCAL_BRIDGE_H */
