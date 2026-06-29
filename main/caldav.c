/*
 * CalDAV client for Feishu calendar.
 * Two-step REPORT: calendar-query (get hrefs) + calendar-multiget (get iCal data).
 * HTTPS with certificate bundle, Basic Auth.
 */

#include "caldav.h"
#include "utf8_gb2312.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "caldav";

// ── CalDAV configuration ────────────────────────────────────────────────────────
#define CALDAV_HOST     "https://caldav.feishu.cn"
#define CALDAV_CAL_PATH "/u_ohpt7825/5FFD87EB-B799-4002-5FFD-87EBB7994002/"
#define CALDAV_AUTH     "Basic dV9vaHB0NzgyNTpOYmg5c0R3a0pZ"

// ── HTTP response buffer ────────────────────────────────────────────────────────
static char *s_resp_buf;
static int   s_resp_len;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (s_resp_len + evt->data_len < 8192) {
            memcpy(s_resp_buf + s_resp_len, evt->data, evt->data_len);
            s_resp_len += evt->data_len;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/**
 * Send a CalDAV REPORT request with the given XML body.
 * Returns the HTTP status code, response stored in resp_buf.
 */
static int caldav_report(const char *xml_body, int xml_len)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", CALDAV_HOST, CALDAV_CAL_PATH);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_REPORT,
        .timeout_ms = 15000,
        .event_handler = http_event_handler,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return -1;
    }

    esp_http_client_set_header(client, "Authorization", CALDAV_AUTH);
    esp_http_client_set_header(client, "Content-Type", "application/xml; charset=\"utf-8\"");
    esp_http_client_set_header(client, "Depth", "1");

    // POST the XML body
    esp_http_client_set_post_field(client, xml_body, xml_len);

    s_resp_len = 0;
    esp_err_t err = esp_http_client_perform(client);
    int status = -1;

    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
        s_resp_buf[s_resp_len] = '\0';
        ESP_LOGI(TAG, "REPORT status=%d, len=%d", status, s_resp_len);
    } else {
        ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return status;
}

/**
 * Find next occurrence of <tag>...</tag> in text starting at *pos.
 * Writes the content between tags into out (max out_size).
 * Advances *pos past the closing tag.
 * Returns 1 if found, 0 if not.
 */
static int xml_find_tag(const char *text, int len, int *pos,
                        const char *tag, char *out, int out_size)
{
    char open_tag[64], close_tag[64];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    // Find opening tag
    const char *p = strstr(text + *pos, open_tag);
    if (!p) return 0;

    const char *content_start = p + strlen(open_tag);
    const char *content_end = strstr(content_start, close_tag);
    if (!content_end) return 0;

    int content_len = content_end - content_start;
    if (content_len >= out_size) content_len = out_size - 1;
    memcpy(out, content_start, content_len);
    out[content_len] = '\0';

    *pos = (content_end - text) + strlen(close_tag);
    return 1;
}

/**
 * Decode HTML entities in-place: &#xD; &#xA; &#x0A; &#13; etc.
 * Also decode &amp; &lt; &gt; &quot; &apos;
 */
static void decode_html_entities(char *text)
{
    char *src = text, *dst = text;
    while (*src) {
        if (src[0] == '&' && src[1] == '#') {
            // Numeric entity &#NN; or &#xHH;
            char *end;
            long val;
            if (src[2] == 'x' || src[2] == 'X') {
                val = strtol(src + 3, &end, 16);
            } else {
                val = strtol(src + 2, &end, 10);
            }
            if (end && *end == ';') {
                // CR (13) and LF (10) — convert to space or newline
                if (val == 13 || val == 10) {
                    *dst++ = '\n';
                } else {
                    *dst++ = (char)val;
                }
                src = end + 1;
                continue;
            }
        } else if (strncmp(src, "&amp;", 5) == 0) {
            *dst++ = '&'; src += 5; continue;
        } else if (strncmp(src, "&lt;", 4) == 0) {
            *dst++ = '<'; src += 4; continue;
        } else if (strncmp(src, "&gt;", 4) == 0) {
            *dst++ = '>'; src += 4; continue;
        } else if (strncmp(src, "&quot;", 6) == 0) {
            *dst++ = '"'; src += 6; continue;
        } else if (strncmp(src, "&apos;", 6) == 0) {
            *dst++ = '\''; src += 6; continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

/**
 * Parse an iCal VEVENT block into a caldav_event_t.
 */
static int parse_vevent(const char *ical, caldav_event_t *evt)
{
    memset(evt, 0, sizeof(*evt));

    // Find SUMMARY
    const char *p = strstr(ical, "SUMMARY:");
    if (p) {
        p += 8;
        const char *eol = strchr(p, '\n');
        if (!eol) eol = strchr(p, '\r');
        if (!eol) eol = p + strlen(p);
        int len = eol - p;
        // Trim trailing \r
        while (len > 0 && (p[len-1] == '\r' || p[len-1] == '\n')) len--;
        if (len >= (int)sizeof(evt->summary)) len = sizeof(evt->summary) - 1;
        memcpy(evt->summary, p, len);
        evt->summary[len] = '\0';
    }

    // Find DTSTART;TZID=Asia/Shanghai:YYYYMMDDTHHMMSS
    p = strstr(ical, "DTSTART");
    if (p) {
        const char *colon = strchr(p, ':');
        if (colon) {
            // Parse YYYYMMDDTHHMMSS
            int y, m, d, hh, mm;
            if (sscanf(colon + 1, "%4d%2d%2dT%2d%2d", &y, &m, &d, &hh, &mm) >= 5) {
                evt->year = y;
                evt->month = m;
                evt->day = d;
                evt->start_hour = hh;
                evt->start_min = mm;
            }
        }
    }

    // Find DTEND
    p = strstr(ical, "DTEND");
    if (p) {
        const char *colon = strchr(p, ':');
        if (colon) {
            int hh, mm;
            if (sscanf(colon + 1, "%*4d%*2d%*2dT%2d%2d", &hh, &mm) >= 2) {
                evt->end_hour = hh;
                evt->end_min = mm;
            }
        }
    }

    // Convert summary from UTF-8 to GB2312
    utf8_to_gb2312(evt->summary, evt->summary_gb, sizeof(evt->summary_gb));

    return (evt->summary[0] != 0 && evt->year != 0) ? 1 : 0;
}

/**
 * Compare two events by date and start time (for qsort).
 */
static int cmp_events(const void *a, const void *b)
{
    const caldav_event_t *ea = (const caldav_event_t *)a;
    const caldav_event_t *eb = (const caldav_event_t *)b;
    if (ea->year != eb->year) return ea->year - eb->year;
    if (ea->month != eb->month) return ea->month - eb->month;
    if (ea->day != eb->day) return ea->day - eb->day;
    if (ea->start_hour != eb->start_hour) return ea->start_hour - eb->start_hour;
    return ea->start_min - eb->start_min;
}

esp_err_t caldav_fetch_events(int year, int month, int day,
                              caldav_event_t *events, int *out_count)
{
    if (!events || !out_count) return ESP_ERR_INVALID_ARG;

    *out_count = 0;

    // Allocate all large buffers on heap to avoid stack overflow
    s_resp_buf = malloc(8192);
    char (*hrefs)[128] = malloc(20 * 128);        // 2560 bytes
    char *multiget_xml = malloc(4096);
    char *cal_data = malloc(2048);
    char *query_xml = malloc(512);

    if (!s_resp_buf || !hrefs || !multiget_xml || !cal_data || !query_xml) {
        ESP_LOGE(TAG, "Failed to alloc CalDAV buffers");
        free(s_resp_buf); free(hrefs); free(multiget_xml);
        free(cal_data); free(query_xml);
        return ESP_ERR_NO_MEM;
    }

    // Calculate date range: today to end of next month
    char start_date[32], end_date[32];
    snprintf(start_date, sizeof(start_date), "%04d%02d%02dT000000Z", year, month, day);

    int end_month = month + 1;
    int end_year = year;
    if (end_month > 12) { end_month = 1; end_year++; }
    snprintf(end_date, sizeof(end_date), "%04d%02d01T000000Z", end_year, end_month);

    // ── Step 1: calendar-query to get event hrefs ──
    int query_len = snprintf(query_xml, 512,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<c:calendar-query xmlns:D=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
        "<D:prop><c:calendar-data/></D:prop>"
        "<c:filter><c:comp-filter name=\"VCALENDAR\">"
        "<c:comp-filter name=\"VEVENT\">"
        "<c:time-range start=\"%s\" end=\"%s\"/>"
        "</c:comp-filter></c:comp-filter></c:filter>"
        "</c:calendar-query>",
        start_date, end_date);

    ESP_LOGI(TAG, "Step 1: calendar-query %s ~ %s", start_date, end_date);
    int status = caldav_report(query_xml, query_len);
    if (status != 207) {
        ESP_LOGE(TAG, "calendar-query failed, status=%d", status);
        goto cleanup_fail;
    }

    // Parse hrefs from step 1 response
    int num_hrefs = 0;
    int pos = 0;
    while (num_hrefs < 20) {
        if (!xml_find_tag(s_resp_buf, s_resp_len, &pos, "D:href",
                          hrefs[num_hrefs], 128)) {
            int pos2 = pos;
            if (!xml_find_tag(s_resp_buf, s_resp_len, &pos2, "href",
                              hrefs[num_hrefs], 128)) {
                break;
            }
            pos = pos2;
        }
        if (strstr(hrefs[num_hrefs], ".ics")) {
            num_hrefs++;
        }
    }

    ESP_LOGI(TAG, "Found %d event hrefs", num_hrefs);
    if (num_hrefs == 0) {
        goto cleanup_ok;
    }

    // ── Step 2: calendar-multiget to fetch full iCal data ──
    int mlen = snprintf(multiget_xml, 4096,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<c:calendar-multiget xmlns:D=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
        "<D:prop><c:calendar-data/></D:prop>");

    for (int i = 0; i < num_hrefs; i++) {
        mlen += snprintf(multiget_xml + mlen, 4096 - mlen,
                         "<D:href>%s</D:href>", hrefs[i]);
    }
    mlen += snprintf(multiget_xml + mlen, 4096 - mlen,
                     "</c:calendar-multiget>");

    ESP_LOGI(TAG, "Step 2: calendar-multiget (%d hrefs)", num_hrefs);
    status = caldav_report(multiget_xml, mlen);
    if (status != 207) {
        ESP_LOGE(TAG, "calendar-multiget failed, status=%d", status);
        goto cleanup_fail;
    }

    // Parse calendar-data from step 2 response
    int count = 0;
    pos = 0;
    while (count < CALDAV_MAX_EVENTS) {
        int found = xml_find_tag(s_resp_buf, s_resp_len, &pos,
                                 "C:calendar-data", cal_data, 2048);
        if (!found) {
            found = xml_find_tag(s_resp_buf, s_resp_len, &pos,
                                 "calendar-data", cal_data, 2048);
        }
        if (!found) break;

        decode_html_entities(cal_data);

        const char *vevent = strstr(cal_data, "BEGIN:VEVENT");
        if (vevent) {
            if (parse_vevent(vevent, &events[count])) {
                ESP_LOGI(TAG, "Event: %04d-%02d-%02d %02d:%02d %s",
                         events[count].year, events[count].month, events[count].day,
                         events[count].start_hour, events[count].start_min,
                         events[count].summary);
                count++;
            }
        }
    }

    qsort(events, count, sizeof(caldav_event_t), cmp_events);
    *out_count = count;
    ESP_LOGI(TAG, "Total events parsed: %d", count);

cleanup_ok:
    free(s_resp_buf); s_resp_buf = NULL;
    free(hrefs); free(multiget_xml); free(cal_data); free(query_xml);
    return ESP_OK;

cleanup_fail:
    free(s_resp_buf); s_resp_buf = NULL;
    free(hrefs); free(multiget_xml); free(cal_data); free(query_xml);
    return ESP_FAIL;
}
