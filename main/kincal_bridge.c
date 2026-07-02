/*
 * KinCal → calendar bridge.
 *
 * Converts a kincal_display_data_t into the legacy global state that
 * calendar.c consumes (g_weather, g_events[], g_event_count) and calls
 * the appropriate calendar.c redraw entry points.
 *
 * Also writes KinCal-specific overrides (rest_days, lunar_text, ai_face)
 * to globals defined in hello_world_main.c — calendar.c reads them via
 * externs to override its local holiday table and lunar computation.
 */

#include "kincal_bridge.h"
#include "calendar.h"
#include "caldav.h"
#include "weather.h"
#include "kincal_client.h"
#include "utf8_gb2312.h"
#include "st7306.h"
#include "app_manager.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "kincal_bridge";

// ── Globals owned by hello_world_main.c ───────────────────────────────────────
extern weather_data_t g_weather;
extern caldav_event_t g_events[];
extern int g_event_count;

// KinCal overrides for calendar.c. When g_kincal_active is true and the
// corresponding count is > 0, calendar.c prefers these over its local
// static holiday table / lunar computation.
int    g_kincal_rest_days[32]   = {0};
uint8_t g_kincal_rest_count     = 0;
bool   g_kincal_active          = false;

int    g_kincal_event_dates[32] = {0};
uint8_t g_kincal_event_count    = 0;

uint8_t g_kincal_lunar_text[32] = {0};   // GB2312 encoded "农历四月十二"
bool   g_kincal_has_lunar       = false;

// True when the most recent KinCal response included the weather object.
// calendar_app.c reads this to decide whether to fall back to SHTC3 indoor
// readings for the weather panel.
bool   g_kincal_has_weather     = false;

// ── Helpers ───────────────────────────────────────────────────────────────────

// Parse "HH:MM" → hour/min. Returns false for "全天" or unparseable strings.
// All-day sentinel: hour=-1, min=0 (calendar.c renders "全天" verbatim).
static bool parse_time(const char *s, int *hour, int *min)
{
    if (!s || !s[0]) { *hour = -1; *min = 0; return false; }
    /* "全天" GB2312 = C8 AB CC EC — but server sends UTF-8. UTF-8 "全天" = E5 85 A8 E5 A4 A9 */
    if (strstr(s, "全天") || strstr(s, "allday") || strstr(s, "All Day")) {
        *hour = -1; *min = 0;
        return true;
    }
    int h = 0, m = 0;
    if (sscanf(s, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
        *hour = h; *min = m;
        return true;
    }
    *hour = -1; *min = 0;
    return false;
}

// Parse "MM/DD" → month/day. Year inferred from current time (caller passes
// the current year so we don't need to call time() here).
static bool parse_date_mmdd(const char *s, int cur_year, int cur_month,
                            int *out_year, int *out_month, int *out_day)
{
    int mo = 0, da = 0;
    if (sscanf(s, "%d/%d", &mo, &da) == 2 && mo >= 1 && mo <= 12 && da >= 1 && da <= 31) {
        /* If month is in the past relative to current month, assume next year */
        *out_year  = (mo < cur_month) ? cur_year + 1 : cur_year;
        *out_month = mo;
        *out_day   = da;
        return true;
    }
    return false;
}

// Convert one KinCal event to caldav_event_t. Sets year/month/day from
// caller (today's date for events_today, parsed date for upcoming).
static void fill_event(const kincal_event_t *ke, caldav_event_t *out,
                       int year, int month, int day)
{
    memset(out, 0, sizeof(*out));
    out->year = year;
    out->month = month;
    out->day = day;
    parse_time(ke->time_str, &out->start_hour, &out->start_min);
    out->end_hour = -1;
    out->end_min  = 0;
    strncpy(out->summary, ke->title, sizeof(out->summary) - 1);
    utf8_to_gb2312(ke->title, out->summary_gb, sizeof(out->summary_gb));
}

// ── Public API ────────────────────────────────────────────────────────────────

void kincal_bridge_apply(const kincal_display_data_t *d)
{
    if (!d) return;

    /* Mark KinCal active so calendar.c overrides take effect */
    g_kincal_active = true;

    /* ── rest_days / event_dates overrides ── */
    g_kincal_rest_count = 0;
    for (int i = 0; i < d->rest_count && i < 32; i++) {
        g_kincal_rest_days[g_kincal_rest_count++] = d->rest_days[i];
    }
    g_kincal_event_count = 0;
    for (int i = 0; i < d->event_count && i < 32; i++) {
        g_kincal_event_dates[g_kincal_event_count++] = d->event_dates[i];
    }

    /* ── lunar text override (UTF-8 → GB2312) ── */
    g_kincal_has_lunar = false;
    if (d->has_lunar && d->lunar_date_text[0]) {
        int n = utf8_to_gb2312(d->lunar_date_text,
                               g_kincal_lunar_text,
                               sizeof(g_kincal_lunar_text) - 1);
        if (n > 0) {
            g_kincal_lunar_text[n] = 0;
            g_kincal_has_lunar = true;
        }
    }

    /* ── weather (if plan-gated present) ── */
    g_kincal_has_weather = d->has_weather;
    if (d->has_weather) {
        g_weather.temperature = (float)d->weather.temp_c;
        g_weather.humidity = (d->weather.humidity >= 0)
                              ? d->weather.humidity : 0;
        /* description comes as UTF-8 — convert to GB2312 bytes for hzk16.
         * For simplicity we point g_weather.description at a small static
         * buffer. Caller must keep the KinCal data alive until next fetch. */
        static uint8_t weather_desc_gb[32];
        int dn = utf8_to_gb2312(d->weather.description,
                                weather_desc_gb, sizeof(weather_desc_gb) - 1);
        if (dn <= 0) {
            /* fallback: 多云 */
            static const uint8_t gb_duoyun[] = {0xB6, 0xE0, 0xD4, 0xC6, 0};
            g_weather.description = gb_duoyun;
        } else {
            weather_desc_gb[dn] = 0;
            g_weather.description = weather_desc_gb;
        }
        /* weather_code unused for rendering — set a sentinel */
        g_weather.weather_code = -1;
        /* Reset city to NULL so calendar.c falls back to 北京 (outdoor mode).
         * When !has_weather, calendar_app.c will overwrite with 室内 from SHTC3. */
        g_weather.city = NULL;
    }

    /* ── events: flatten events_today + upcoming into g_events[] ──
     * calendar_set_events re-splits by year/month/day matching, so we tag
     * events_today with today's date and upcoming with parsed future dates.
     */
    time_t now;
    time(&now);
    struct tm ti;
    localtime_r(&now, &ti);
    int today_y = ti.tm_year + 1900;
    int today_m = ti.tm_mon + 1;
    int today_d = ti.tm_mday;

    int count = 0;
    for (int i = 0; i < d->today_count && count < CALDAV_MAX_EVENTS; i++) {
        fill_event(&d->events_today[i], &g_events[count++],
                   today_y, today_m, today_d);
    }
    for (int i = 0; i < d->upcoming_count && count < CALDAV_MAX_EVENTS; i++) {
        int y = today_y, mo = 0, da = 0;
        parse_date_mmdd(d->upcoming[i].date_str, today_y, today_m, &y, &mo, &da);
        /* If date parsing failed (mo=0), calendar_set_events will treat the
         * event as upcoming (no match to today's y/m/d). The render path
         * displays month/day from the event struct, so an unparseable date
         * shows up as "00/00" — log it but don't crash. */
        if (mo == 0) {
            ESP_LOGW(TAG, "upcoming[%d] unparseable date_str='%s'",
                     i, d->upcoming[i].date_str);
        }
        fill_event(&d->upcoming[i], &g_events[count++], y, mo, da);
    }
    g_event_count = count;

    ESP_LOGI(TAG, "bridge applied: events=%d rest=%d lunar=%d weather=%d",
             g_event_count, g_kincal_rest_count,
             g_kincal_has_lunar, d->has_weather);
}

void kincal_bridge_clear(void)
{
    g_kincal_active = false;
    g_kincal_rest_count = 0;
    g_kincal_event_count = 0;
    g_kincal_has_lunar = false;
    g_kincal_lunar_text[0] = 0;
    ESP_LOGI(TAG, "bridge cleared");
}
