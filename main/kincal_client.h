#ifndef KINCAL_CLIENT_H
#define KINCAL_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * KinCal JSON display client.
 *
 * Fetches GET /api/v1/esp32/display/{short_code} from the configured
 * KinCal host (CONFIG_KINCAL_HOST). The short code is loaded once from
 * NVS at init time — empty short code means "not provisioned", caller
 * should render the pair-prompt screen instead of fetching.
 *
 * All strings in the output struct are UTF-8 (preserving emoji and
 * special chars). The bridge layer converts to GB2312 when copying into
 * the legacy caldav_event_t / weather_data_t shapes that calendar.c
 * consumes.
 */

#define KINCAL_MAX_EVENTS         8      // hard cap, server sends max 4 + N
#define KINCAL_PLAN_TIER_LEN      8

typedef struct {
    char date_str[8];     /* "MM/DD" for upcoming, "" for events_today */
    char time_str[12];    /* "09:30" or "全天" (UTF-8)                 */
    char title[64];       /* UTF-8 event title (raw from server)       */
    char location[32];    /* UTF-8 location, may be empty              */
} kincal_event_t;

typedef struct {
    int  temp_c;
    char description[32]; /* UTF-8 weather description */
    int  humidity;        /* 0-100, -1 if missing      */
    char city[32];        /* UTF-8 city name           */
} kincal_weather_t;

typedef struct {
    /* Identity / time */
    int   server_now_epoch;     /* UTC seconds, for clock-drift detection */
    char  timezone[32];

    /* Rendering month (may differ from today when ?month= is used) */
    int   cal_year, cal_month;
    int   today_day;            /* day-of-month, -1 when viewing other month */

    /* Month grid */
    int   weeks[6][7];          /* 0 = padding cell                     */

    /* Day annotations */
    int   rest_days[32];        /* weekend + holiday - tiaoxiu          */
    uint8_t rest_count;
    int   event_dates[32];
    uint8_t event_count;

    /* Events (already split by server) */
    kincal_event_t events_today[KINCAL_MAX_EVENTS];
    uint8_t today_count;
    kincal_event_t upcoming[KINCAL_MAX_EVENTS];
    uint8_t upcoming_count;

    /* Plan-gated optional fields */
    kincal_weather_t weather;
    bool  has_weather;
    char  lunar_date_text[32];  /* UTF-8 like "农历四月十二" or empty */
    bool  has_lunar;

    /* AI face */
    char  ai_face_id[12];       /* "sleep"/"morning"/"busy"/"has_events"/"idle" */
    char  ai_face_msg[32];      /* UTF-8 message mirroring ai_face_id           */

    /* Refresh + plan */
    int   refresh_interval_seconds;  /* 60 (Pro/Biz) or 300 (Free) */
    char  plan_tier[KINCAL_PLAN_TIER_LEN];
} kincal_display_data_t;

/**
 * Idempotent init. Reads short code + host override from NVS, caches them.
 * Safe to call multiple times.
 */
esp_err_t kincal_client_init(void);

/**
 * Blocking fetch from the KinCal server. Caller must invoke from a worker
 * task (not the dispatcher). On failure, `out` is zeroed and the previous
 * fetch's data should be retained by the caller.
 */
esp_err_t kincal_client_fetch(kincal_display_data_t *out);

/**
 * No-op placeholder for symmetry with future freeable fields. Currently
 * all fields are plain buffers, so just zeroes the struct.
 */
void kincal_display_data_free(kincal_display_data_t *d);

/**
 * Clamp the server's refresh_interval_seconds to a sane retry window.
 * Returns 60 if `d` is NULL or refresh is 0, max 3600.
 */
int kincal_client_next_refresh_seconds(const kincal_display_data_t *d);

/**
 * Whether a short code is provisioned in NVS. False means caller should
 * draw the "please pair via AP" prompt instead of fetching.
 */
bool kincal_client_is_provisioned(void);

/**
 * Read-only access to the cached short code. Returns "" if unprovisioned.
 */
const char *kincal_client_get_short_code(void);

#endif /* KINCAL_CLIENT_H */
