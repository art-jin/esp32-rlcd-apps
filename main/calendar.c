/*
 * Monthly calendar + weather + todos for ESP32-S3-RLCD-4.2 (400×300 landscape)
 * Left panel (120px, full height): weather (top) + todos (bottom)
 * Right panel (280px): header + weekday + calendar grid + status bar
 */

#include "calendar.h"
#include "st7306.h"
#include "hzk16.h"
#include "lunar.h"
#include "weather.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

// ── Layout constants ───────────────────────────────────────────────────────────
#define HEADER_H     26
#define WEEKDAY_H    18
#define GRID_TOP     (HEADER_H + WEEKDAY_H)  // 44
#define STATUS_H     26                              // same as header
#define STATUS_Y     (ST7306_HEIGHT - STATUS_H)      // 274
#define GRID_BOTTOM  (STATUS_Y - 2)                  // 272

#define PANEL_W      132    // Left panel width (weather + events)
#define CAL_X        PANEL_W  // Calendar area start x (132)
#define CAL_WIDTH    268    // Calendar area width

#define TAB_H        18    // Tab header height (black bg, white text)

#define NUM_COLS     7
#define NUM_ROWS     6
#define CELL_W       (CAL_WIDTH / NUM_COLS)  // 38
#define CELL_H       38                              // fills grid area (228 / 6)

// Day number size (custom 10×20 bitmap font)
#define DAY_NUM_W    10
#define DAY_NUM_H    20

// ── GB2312 pre-encoded strings ─────────────────────────────────────────────────
static const uint8_t gb_tianqi[]  = {0xCC, 0xEC, 0xC6, 0xF8, 0};       // 天气
static const uint8_t gb_beijing[] = {0xB1, 0xB1, 0xBE, 0xA9, 0};       // 北京
static const uint8_t gb_du[]      = {0xB6, 0xC8, 0};                    // 度
static const uint8_t gb_shidu[]   = {0xCA, 0xAA, 0xB6, 0xC8, 0};       // 湿度

static const uint8_t gb_jinri[]   = {0xB4, 0xFA, 0xB0, 0xEC, 0xCA, 0xC2, 0xD2, 0xCB, 0}; // 代办事宜
static const uint8_t gb_bullet[]  = {0xA1, 0xF1, 0};                    // ●
static const uint8_t gb_noevent[] = {0xBD, 0xF1, 0xC8, 0xD5, 0xC3, 0xBB, 0xD3, 0xD0, 0xB0, 0xB2, 0xC5, 0xC5, 0};  // 今日没有安排
static const uint8_t gb_key_xiaozhi[] = {'K','E','Y',':', 0xD0,0xA1, 0xD6,0xC7, 0};  // KEY:小智

// Pre-combined weekday strings: 星期日 ~ 星期六
static const uint8_t gb_weekday_full[][7] = {
    {0xD0,0xC7, 0xC6,0xDA, 0xC8,0xD5, 0},  // 星期日
    {0xD0,0xC7, 0xC6,0xDA, 0xD2,0xBB, 0},  // 星期一
    {0xD0,0xC7, 0xC6,0xDA, 0xB6,0xFE, 0},  // 星期二
    {0xD0,0xC7, 0xC6,0xDA, 0xC8,0xFD, 0},  // 星期三
    {0xD0,0xC7, 0xC6,0xDA, 0xCB,0xC4, 0},  // 星期四
    {0xD0,0xC7, 0xC6,0xDA, 0xCE,0xE5, 0},  // 星期五
    {0xD0,0xC7, 0xC6,0xDA, 0xC1,0xF9, 0},  // 星期六
};

// Dynamic event data from CalDAV
static caldav_event_t s_today_events[CALDAV_MAX_EVENTS];
static int s_today_count = 0;
static caldav_event_t s_upcoming_events[CALDAV_MAX_EVENTS];
static int s_upcoming_count = 0;
static int s_event_year, s_event_month, s_event_day;  // date of current events

// Navigation selection state
static bool s_has_selection = false;
static int s_sel_year, s_sel_month, s_sel_day;
static int s_today_year, s_today_month, s_today_day;
static bool s_nav_initialized = false;
static const weather_data_t *s_stored_weather = NULL;

// ── Helpers ────────────────────────────────────────────────────────────────────
// Truncate GB2312 text to fit max_px width in 16px mode (ASCII=8px, Chinese=16px)
static void truncate_gb16(const uint8_t *gb, int max_px, uint8_t *out, int out_size)
{
    int x = 0, si = 0, di = 0;
    while (gb[si] && di < out_size - 1) {
        if (gb[si] < 0x80) {
            if (x + 8 > max_px) break;
            out[di++] = gb[si++];
            x += 8;
        } else {
            if (x + 16 > max_px || di + 2 > out_size - 1) break;
            out[di++] = gb[si++];
            out[di++] = gb[si++];
            x += 16;
        }
    }
    out[di] = 0;
}

static int day_of_week(int year, int month, int day)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = year;
    if (month < 3) y--;
    return (y + y/4 - y/100 + y/400 + t[month-1] + day) % 7;
}

static int days_in_month(int year, int month)
{
    static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int d = dim[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)))
        d = 29;
    return d;
}

// ── Holiday data (compact range table) ─────────────────────────────────────────
typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t start;
    uint8_t count;      // consecutive holiday days from start
} holiday_range;

static const holiday_range holidays[] = {
    // 2025
    {2025,  1,  1,  1},     // 元旦
    {2025,  1, 28,  4},     // 春节 Jan 28-31
    {2025,  2,  1,  3},     // 春节 Feb 1-3
    {2025,  4,  4,  3},     // 清明
    {2025,  5,  1,  5},     // 劳动节
    {2025,  5, 31,  1},     // 端午
    {2025,  6,  1,  2},     // 端午 continued
    {2025, 10,  1,  8},     // 国庆+中秋
    // 2026 (国务院办公厅 国办发明电〔2025〕7号)
    {2026,  1,  1,  3},     // 元旦 Jan 1-3
    {2026,  2, 15,  9},     // 春节 Feb 15-23
    {2026,  4,  4,  3},     // 清明 Apr 4-6
    {2026,  5,  1,  5},     // 劳动节 May 1-5
    {2026,  6, 19,  3},     // 端午 Jun 19-21
    {2026,  9, 25,  3},     // 中秋 Sep 25-27
    {2026, 10,  1,  7},     // 国庆 Oct 1-7
    // sentinel
    {0, 0, 0, 0},
};

static bool is_holiday(int year, int month, int day)
{
    for (int i = 0; holidays[i].year != 0; i++) {
        if (holidays[i].year != year || holidays[i].month != month) continue;
        if (day >= holidays[i].start && day < holidays[i].start + holidays[i].count)
            return true;
    }
    return false;
}

// ── 调休 workday table (weekends designated as workdays) ────────────────────────
static const struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
} tiaoxiu_workdays[] = {
    // 2025
    {2025,  1, 26},    // 春节调休
    {2025,  2,  8},    // 春节调休
    {2025,  4, 27},    // 劳动节调休
    {2025, 10, 11},    // 国庆调休
    // 2026 (国务院办公厅 国办发明电〔2025〕7号)
    {2026,  1,  4},    // 元旦调休 (Sun)
    {2026,  2, 14},    // 春节调休 (Sat)
    {2026,  2, 28},    // 春节调休 (Sat)
    {2026,  5,  9},    // 劳动节调休 (Sat)
    {2026,  9, 20},    // 国庆调休 (Sun)
    {2026, 10, 10},    // 国庆调休 (Sat)
    // sentinel
    {0, 0, 0},
};

static bool is_tiaoxiu_workday(int year, int month, int day)
{
    for (int i = 0; tiaoxiu_workdays[i].year != 0; i++) {
        if (tiaoxiu_workdays[i].year == year &&
            tiaoxiu_workdays[i].month == month &&
            tiaoxiu_workdays[i].day == day)
            return true;
    }
    return false;
}

static bool is_rest_day(int year, int month, int day)
{
    // 调休 workdays are never rest days
    if (is_tiaoxiu_workday(year, month, day)) return false;

    // Weekends (Sunday=0, Saturday=6)
    int dow = day_of_week(year, month, day);
    if (dow == 0 || dow == 6) return true;

    // Government holidays
    return is_holiday(year, month, day);
}

// ── Draw header (full width, 14px font) ────────────────────────────────────────
static void draw_header(int year, int month, int day)
{
    int y = (HEADER_H - 14) / 2;
    char buf[16];

    // Left: "2026年5月8日 星期五"
    int x = CAL_X + 6;
    snprintf(buf, sizeof(buf), "%d", year);
    x = hzk16_draw_gb_text_14(x, y, (const uint8_t *)buf, ST7306_COLOR_BLACK);
    x = hzk16_draw_gb_text_14(x, y, gb_year_char(), ST7306_COLOR_BLACK);

    snprintf(buf, sizeof(buf), "%d", month);
    x = hzk16_draw_gb_text_14(x, y, (const uint8_t *)buf, ST7306_COLOR_BLACK);
    x = hzk16_draw_gb_text_14(x, y, gb_month_char(), ST7306_COLOR_BLACK);

    snprintf(buf, sizeof(buf), "%d", day);
    x = hzk16_draw_gb_text_14(x, y, (const uint8_t *)buf, ST7306_COLOR_BLACK);
    x = hzk16_draw_gb_text_14(x, y, gb_day_char(), ST7306_COLOR_BLACK);

    x += 6;
    int wd = day_of_week(year, month, day);
    if (wd >= 0 && wd <= 6) {
        // Draw weekday using native 16×16 font (y-1 to vertically center in 26px header)
        hzk16_draw_gb_text(x, y - 1, gb_weekday_full[wd], ST7306_COLOR_BLACK);
    }

    // Right: "农历四月十二"
    lunar_date_t lunar;
    if (solar_to_lunar(year, month, day, 12, &lunar) == 0) {
        uint8_t lunar_str[32];
        int pos = 0;
        const uint8_t *p;

        p = gb_lunar_prefix();
        for (int i = 0; p[i] && pos < 28; i++) lunar_str[pos++] = p[i];
        if (lunar.is_leap) {
            p = gb_leap_char();
            for (int i = 0; p[i] && pos < 28; i++) lunar_str[pos++] = p[i];
        }
        p = lunar_month_name(lunar.month);
        for (int i = 0; p[i] && pos < 28; i++) lunar_str[pos++] = p[i];
        p = lunar_day_name(lunar.day);
        for (int i = 0; p[i] && pos < 28; i++) lunar_str[pos++] = p[i];
        lunar_str[pos] = 0;

        int lw = hzk16_text_width_14(lunar_str);
        hzk16_draw_gb_text_14(CAL_X + CAL_WIDTH - lw - 6, y, lunar_str, ST7306_COLOR_BLACK);
    }

    st7306_draw_hline(CAL_X, CAL_X + CAL_WIDTH - 1, HEADER_H - 1, ST7306_COLOR_BLACK);
}

// ── Draw weekday labels (calendar area only) ───────────────────────────────────
static void draw_weekday_labels(void)
{
    int y = HEADER_H + 1;
    static const uint8_t *labels[] = {
        (const uint8_t *)"\xC8\xD5",  // 日
        (const uint8_t *)"\xD2\xBB",  // 一
        (const uint8_t *)"\xB6\xFE",  // 二
        (const uint8_t *)"\xC8\xFD",  // 三
        (const uint8_t *)"\xCB\xC4",  // 四
        (const uint8_t *)"\xCE\xE5",  // 五
        (const uint8_t *)"\xC1\xF9",  // 六
    };

    // Black background for weekday row
    st7306_draw_filled_rect(CAL_X, HEADER_H, CAL_WIDTH, WEEKDAY_H, ST7306_COLOR_BLACK);

    for (int col = 0; col < NUM_COLS; col++) {
        int cx = CAL_X + col * CELL_W;
        int tw = hzk16_text_width(labels[col]);
        int sx = cx + (CELL_W - tw) / 2;
        hzk16_draw_gb_text(sx, y, labels[col], ST7306_COLOR_WHITE);
    }

    st7306_draw_hline(CAL_X, CAL_X + CAL_WIDTH - 1, GRID_TOP - 1, ST7306_COLOR_WHITE);
}

// Check if any event (today or upcoming) falls on the given date
static bool has_events_on(int year, int month, int day)
{
    for (int i = 0; i < s_today_count; i++) {
        if (s_today_events[i].year == year &&
            s_today_events[i].month == month &&
            s_today_events[i].day == day)
            return true;
    }
    for (int i = 0; i < s_upcoming_count; i++) {
        if (s_upcoming_events[i].year == year &&
            s_upcoming_events[i].month == month &&
            s_upcoming_events[i].day == day)
            return true;
    }
    return false;
}

// Forward declarations
static void draw_calendar(int year, int month, int day);
static void draw_left_panel(const weather_data_t *weather);

// ── Selection cursor overlay ──────────────────────────────────────────────────
static void draw_cursor_overlay(int year, int month, int day, int today_day)
{
    int first_dow = day_of_week(year, month, 1);
    int cell_idx = first_dow + day - 1;
    int row = cell_idx / NUM_COLS;
    int col = cell_idx % NUM_COLS;
    if (row >= NUM_ROWS) return;

    int cx = CAL_X + col * CELL_W;
    int cy = GRID_TOP + row * CELL_H;
    int is_today = (day == today_day);
    int color = is_today ? ST7306_COLOR_WHITE : ST7306_COLOR_BLACK;

    // 2px-wide outline cursor (two nested rectangles)
    st7306_draw_rect(cx + 1, cy + 1, CELL_W - 2, CELL_H - 2, color);
    st7306_draw_rect(cx + 3, cy + 3, CELL_W - 6, CELL_H - 6, color);
}

// Full redraw for navigated view (different month or first selection)
static void calendar_draw_navigate(void)
{
    if (!s_nav_initialized) return;

    int view_year, view_month, header_day;
    if (s_has_selection) {
        view_year = s_sel_year;
        view_month = s_sel_month;
        header_day = s_sel_day;
    } else {
        view_year = s_today_year;
        view_month = s_today_month;
        header_day = s_today_day;
    }

    // Only highlight "today" if viewing the current month
    int today_day = (view_year == s_today_year && view_month == s_today_month)
                    ? s_today_day : 0;

    st7306_clear();
    draw_header(view_year, view_month, header_day);
    draw_weekday_labels();
    draw_calendar(view_year, view_month, today_day);
    draw_left_panel(s_stored_weather);

    if (s_has_selection && view_year == s_sel_year && view_month == s_sel_month) {
        draw_cursor_overlay(view_year, view_month, s_sel_day, today_day);
    }
}

static void draw_calendar(int year, int month, int day)
{
    int first_dow = day_of_week(year, month, 1);
    int dim = days_in_month(year, month);

    // Row separators (calendar area only)
    for (int row = 1; row < NUM_ROWS; row++) {
        int y = GRID_TOP + row * CELL_H;
        st7306_draw_hline(CAL_X, CAL_X + CAL_WIDTH - 1, y, ST7306_COLOR_BLACK);
    }

    // Day numbers — current month only
    for (int d = 1; d <= dim; d++) {
        int cell_idx = first_dow + d - 1;
        int row = cell_idx / NUM_COLS;
        int col = cell_idx % NUM_COLS;
        if (row >= NUM_ROWS) continue;

        int cx = CAL_X + col * CELL_W;
        int cy = GRID_TOP + row * CELL_H;
        int is_today = (d == day);
        int rest = is_rest_day(year, month, d);

        if (is_today) {
            st7306_draw_filled_rect(cx + 1, cy + 1, CELL_W - 2, CELL_H - 2, ST7306_COLOR_BLACK);
        } else if (rest) {
            // Striped background for holidays (light gray effect)
            for (int dy = cy + 2; dy < cy + CELL_H - 2; dy += 2) {
                st7306_draw_hline(cx + 2, cx + CELL_W - 3, dy, ST7306_COLOR_BLACK);
            }
        }

        char day_str[12];
        snprintf(day_str, sizeof(day_str), "%u", (unsigned)d);
        int ndigits = strlen(day_str);
        int dw = ndigits * DAY_NUM_W + (ndigits - 1) * 2;
        int dx = cx + (CELL_W - dw) / 2;
        int dy = cy + (CELL_H - DAY_NUM_H) / 2;
        int color = is_today ? ST7306_COLOR_WHITE : ST7306_COLOR_BLACK;
        for (int ci = 0; ci < ndigits; ci++) {
            st7306_draw_digit_10x20(dx + ci * (DAY_NUM_W + 2), dy,
                                    day_str[ci], color);
        }

        // Event indicator: rounded box around the day number
        if (has_events_on(year, month, d)) {
            int bx = dx - 2, by = dy - 1, bw = dw + 4, bh = DAY_NUM_H + 2;
            int bc = is_today ? ST7306_COLOR_WHITE : ST7306_COLOR_BLACK;
            // Straight edges (3px inset at corners)
            st7306_draw_hline(bx + 3, bx + bw - 4, by, bc);
            st7306_draw_hline(bx + 3, bx + bw - 4, by + bh - 1, bc);
            st7306_draw_vline(bx, by + 3, by + bh - 4, bc);
            st7306_draw_vline(bx + bw - 1, by + 3, by + bh - 4, bc);
            // Corner arcs (r=3)
            st7306_set_pixel(bx + 2, by + 1, bc);
            st7306_set_pixel(bx + 1, by + 2, bc);
            st7306_set_pixel(bx + bw - 3, by + 1, bc);
            st7306_set_pixel(bx + bw - 2, by + 2, bc);
            st7306_set_pixel(bx + 2, by + bh - 2, bc);
            st7306_set_pixel(bx + 1, by + bh - 3, bc);
            st7306_set_pixel(bx + bw - 3, by + bh - 2, bc);
            st7306_set_pixel(bx + bw - 2, by + bh - 3, bc);
        }
    }
}


void calendar_redraw_left_panel(const weather_data_t *weather)
{
    st7306_draw_filled_rect(0, 0, PANEL_W, ST7306_HEIGHT, ST7306_COLOR_WHITE);
    draw_left_panel(weather);
}

// ── Draw left panel: weather (top) + todos (bottom), full height ───────────
static void draw_left_panel(const weather_data_t *weather)
{
    int px = 4;
    int py = 0;

    // ═══ Weather tab ═══
    st7306_draw_filled_rect(0, py, PANEL_W, TAB_H, ST7306_COLOR_BLACK);
    int tw = hzk16_text_width_14(gb_tianqi);
    hzk16_draw_gb_text_14((PANEL_W - tw) / 2, py + 2, gb_tianqi, ST7306_COLOR_WHITE);
    py += TAB_H;

    int wc_top = py;
    int cy = py + 3;

    hzk16_draw_gb_text(px, cy, gb_beijing, ST7306_COLOR_BLACK);
    cy += 18;

    char temp_str[8];
    snprintf(temp_str, sizeof(temp_str), "%d", (int)weather->temperature);
    int tx = px;
    st7306_draw_text(tx, cy, temp_str, ST7306_COLOR_BLACK);
    tx += st7306_text_width(temp_str);
    tx = hzk16_draw_gb_text(tx, cy, gb_du, ST7306_COLOR_BLACK);
    if (weather->description) {
        hzk16_draw_gb_text(tx, cy, weather->description, ST7306_COLOR_BLACK);
    }
    cy += 18;

    tx = px;
    tx = hzk16_draw_gb_text(tx, cy, gb_shidu, ST7306_COLOR_BLACK);
    char hum_str[8];
    snprintf(hum_str, sizeof(hum_str), " %d%%", weather->humidity);
    st7306_draw_text(tx, cy, hum_str, ST7306_COLOR_BLACK);
    cy += 16;

    // ═══ Events tab ═══
    py = wc_top + (cy - wc_top + 3) + 3;
    st7306_draw_filled_rect(0, py, PANEL_W, TAB_H, ST7306_COLOR_BLACK);
    tw = hzk16_text_width_14(gb_jinri);
    hzk16_draw_gb_text_14((PANEL_W - tw) / 2, py + 2, gb_jinri, ST7306_COLOR_WHITE);
    py += TAB_H;

    cy = py + 3;
    int max_y = ST7306_HEIGHT - 4;

    if (s_today_count > 0) {
        for (int i = 0; i < s_today_count && cy + 36 <= max_y; i++) {
            // Line 1: ● HH:MM
            int tx2 = px;
            tx2 = hzk16_draw_gb_text(tx2, cy, gb_bullet, ST7306_COLOR_BLACK);
            tx2 += 4;
            char time_str[16];
            snprintf(time_str, sizeof(time_str), "%02d:%02d",
                     s_today_events[i].start_hour, s_today_events[i].start_min);
            hzk16_draw_gb_text(tx2, cy, (const uint8_t *)time_str, ST7306_COLOR_BLACK);
            cy += 18;

            // Line 2: indented title (16×16)
            uint8_t truncated[64];
            truncate_gb16(s_today_events[i].summary_gb, PANEL_W - px - 8, truncated, sizeof(truncated));
            hzk16_draw_gb_text(px + 16, cy, truncated, ST7306_COLOR_BLACK);
            cy += 20;
        }
    } else {
        hzk16_draw_gb_text(px, cy, gb_noevent, ST7306_COLOR_BLACK);
        cy += 18;
    }

    // Dashed separator between today and upcoming
    if (s_upcoming_count > 0 && cy + 20 <= max_y) {
        cy += 2;
        for (int dx = px; dx < PANEL_W - px; dx += 4) {
            st7306_set_pixel(dx, cy, ST7306_COLOR_BLACK);
            st7306_set_pixel(dx + 1, cy, ST7306_COLOR_BLACK);
        }
        cy += 4;
    }

    if (s_upcoming_count > 0) {
        for (int i = 0; i < s_upcoming_count && cy + 36 <= max_y; i++) {
            // Line 1: MM/DD
            char date_str[16];
            snprintf(date_str, sizeof(date_str), "%02d/%02d",
                     s_upcoming_events[i].month, s_upcoming_events[i].day);
            hzk16_draw_gb_text(px, cy, (const uint8_t *)date_str, ST7306_COLOR_BLACK);
            cy += 18;

            // Line 2: indented title (16×16)
            uint8_t trunc[64];
            truncate_gb16(s_upcoming_events[i].summary_gb, PANEL_W - px - 8, trunc, sizeof(trunc));
            hzk16_draw_gb_text(px + 16, cy, trunc, ST7306_COLOR_BLACK);
            cy += 20;
        }
    }

    // KEY hint at bottom of left panel
    {
        int hint_w = hzk16_text_width_12(gb_key_xiaozhi);
        int hint_x = (PANEL_W - hint_w) / 2;
        int hint_y = ST7306_HEIGHT - 14;
        st7306_draw_hline(4, PANEL_W - 4, hint_y - 3, ST7306_COLOR_BLACK);
        hzk16_draw_gb_text_12(hint_x, hint_y, gb_key_xiaozhi, ST7306_COLOR_BLACK);
    }

    // Vertical separator
    st7306_draw_vline(PANEL_W, 0, ST7306_HEIGHT - 2, ST7306_COLOR_BLACK);
}

// ── Draw status bar: clock + WiFi signal + battery ────────────────────────────
void calendar_draw_wifi_bars(int x, int y, int rssi)
{
    // 4 bars of increasing height (3, 6, 9, 12 px), each 4px wide, 2px gap
    int num_bars;
    if (rssi >= -55)      num_bars = 4;
    else if (rssi >= -65) num_bars = 3;
    else if (rssi >= -75) num_bars = 2;
    else                  num_bars = 1;

    int bar_w = 4;
    int gap = 2;
    int max_h = 12;

    for (int i = 0; i < 4; i++) {
        int bh = (i + 1) * 3;  // 3, 6, 9, 12
        int bx = x + i * (bar_w + gap);
        int by = y + max_h - bh;
        int color = (i < num_bars) ? ST7306_COLOR_BLACK : ST7306_COLOR_WHITE;
        st7306_draw_filled_rect(bx, by, bar_w, bh, color);
        // Outline for unfilled bars
        if (i >= num_bars) {
            st7306_draw_rect(bx, by, bar_w, bh, ST7306_COLOR_BLACK);
        }
    }
}

void calendar_draw_battery_icon(int x, int y, int pct)
{
    // Battery body: 24×10 px, 1px border
    int bw = 24;
    int bh = 10;
    int tip_w = 3;
    int tip_h = 4;

    // Body outline
    st7306_draw_rect(x, y, bw, bh, ST7306_COLOR_BLACK);
    // Tip (positive terminal)
    st7306_draw_filled_rect(x + bw, y + (bh - tip_h) / 2, tip_w, tip_h, ST7306_COLOR_BLACK);

    // Fill level
    int inner_w = bw - 2;
    int fill_w = (inner_w * pct) / 100;
    if (fill_w > 0) {
        st7306_draw_filled_rect(x + 1, y + 1, fill_w, bh - 2, ST7306_COLOR_BLACK);
    }
}

void calendar_draw_status_bar(struct tm *timeinfo, int rssi, int battery_pct)
{
    // Clear status bar area — RIGHT panel (calendar side) only
    st7306_draw_filled_rect(CAL_X, STATUS_Y, CAL_WIDTH, ST7306_HEIGHT - STATUS_Y, ST7306_COLOR_WHITE);

    // Top separator line
    st7306_draw_hline(CAL_X, CAL_X + CAL_WIDTH - 1, STATUS_Y, ST7306_COLOR_BLACK);

    int y = STATUS_Y + (STATUS_H - 16) / 2;  // vertically center in 26px

    // ── Clock (left) ──
    char clock_str[12];
    snprintf(clock_str, sizeof(clock_str), "%02d:%02d:%02d",
             timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    st7306_draw_text(CAL_X + 10, y, clock_str, ST7306_COLOR_BLACK);

    // ── WiFi signal bars (center) ──
    int wifi_x = CAL_X + 110;
    calendar_draw_wifi_bars(wifi_x, y + 2, rssi);

    // ── Battery icon + percentage (right) ──
    char pct_str[8];
    snprintf(pct_str, sizeof(pct_str), "%d%%", battery_pct);
    int pct_w = st7306_text_width(pct_str);
    int batt_x = CAL_X + CAL_WIDTH - pct_w - 30 - 6;
    calendar_draw_battery_icon(batt_x, y, battery_pct);
    st7306_draw_text(batt_x + 30, y, pct_str, ST7306_COLOR_BLACK);
}

// ── Main entries ───────────────────────────────────────────────────────────────
static void calendar_draw_internal(int year, int month, int day, const weather_data_t *weather)
{
    st7306_clear();

    draw_header(year, month, day);
    draw_weekday_labels();
    draw_calendar(year, month, day);
    draw_left_panel(weather);

    // Don't call st7306_update_display() here — caller will update status bar first
}

void calendar_draw(int year, int month, int day)
{
    // Default mock weather for backward compatibility
    static const uint8_t gb_duoyun[] = {0xB6, 0xE0, 0xD4, 0xC6, 0};  // 多云
    weather_data_t mock = {.temperature = 25, .humidity = 55, .weather_code = 2, .description = gb_duoyun};
    calendar_draw_internal(year, month, day, &mock);
    st7306_update_display();
}

void calendar_draw_with_weather(int year, int month, int day, const weather_data_t *weather)
{
    s_today_year = year;
    s_today_month = month;
    s_today_day = day;
    s_stored_weather = weather;
    s_has_selection = false;
    s_nav_initialized = true;
    calendar_draw_internal(year, month, day, weather);
    st7306_update_display();
}

void calendar_update_weather(const weather_data_t *weather)
{
    // Clear and redraw left panel — full height
    st7306_draw_filled_rect(0, 0, PANEL_W, ST7306_HEIGHT, ST7306_COLOR_WHITE);
    draw_left_panel(weather);
}

void calendar_set_events(const caldav_event_t *events, int count,
                         int today_year, int today_month, int today_day,
                         const weather_data_t *weather)
{
    // Separate today's events from upcoming events
    s_today_count = 0;
    s_upcoming_count = 0;
    s_event_year = today_year;
    s_event_month = today_month;
    s_event_day = today_day;

    for (int i = 0; i < count && i < CALDAV_MAX_EVENTS; i++) {
        if (events[i].year == today_year &&
            events[i].month == today_month &&
            events[i].day == today_day) {
            if (s_today_count < CALDAV_MAX_EVENTS) {
                s_today_events[s_today_count++] = events[i];
            }
        } else {
            if (s_upcoming_count < CALDAV_MAX_EVENTS) {
                s_upcoming_events[s_upcoming_count++] = events[i];
            }
        }
    }

    // Redraw calendar grid to show dashed rects for event days
    draw_calendar(today_year, today_month, today_day);

    // Redraw left panel
    st7306_draw_filled_rect(0, 0, PANEL_W, ST7306_HEIGHT, ST7306_COLOR_WHITE);
    draw_left_panel(weather);
}

// ── Config screen (AP mode prompt) ────────────────────────────────────────────
void calendar_draw_config(const char *ap_ssid)
{
    // Pre-encoded GB2312 strings
    static const uint8_t gb_qinglj[] = {0xC7,0xEB,0xC1,0xAC,0xBD,0xD3,0};  // 请连接
    static const uint8_t gb_dksllfw[] = {0xB4,0xF2,0xBF,0xAA,0xE4,0xAF,0xC0,0xC0,0xC6,0xF7,0xB7,0xC3,0xCE,0xCA,0};  // 打开浏览器访问

    st7306_clear();

    int y = 60;
    int x;

    // "请连接" + "WiFi"
    x = 60;
    x = hzk16_draw_gb_text(x, y, gb_qinglj, ST7306_COLOR_BLACK);
    st7306_draw_text(x, y, "WiFi", ST7306_COLOR_BLACK);

    y += 28;
    // AP SSID (ASCII)
    x = (ST7306_WIDTH - st7306_text_width(ap_ssid)) / 2;
    st7306_draw_text(x, y, ap_ssid, ST7306_COLOR_BLACK);

    y += 28;
    // Password label
    st7306_draw_text((ST7306_WIDTH - st7306_text_width("Password: 12345678")) / 2, y,
                     "Password: 12345678", ST7306_COLOR_BLACK);

    y += 40;
    // "打开浏览器访问"
    x = (ST7306_WIDTH - hzk16_text_width(gb_dksllfw)) / 2;
    hzk16_draw_gb_text(x, y, gb_dksllfw, ST7306_COLOR_BLACK);

    y += 28;
    x = (ST7306_WIDTH - st7306_text_width("http://192.168.4.1")) / 2;
    st7306_draw_text(x, y, "http://192.168.4.1", ST7306_COLOR_BLACK);

    st7306_update_display();
}

// ── Single cell redraw (for partial updates) ──────────────────────────────────
static void redraw_single_cell(int year, int month, int day, int today_day, bool draw_cursor)
{
    int first_dow = day_of_week(year, month, 1);
    int cell_idx = first_dow + day - 1;
    int row = cell_idx / NUM_COLS;
    int col = cell_idx % NUM_COLS;
    if (row >= NUM_ROWS) return;

    int cx = CAL_X + col * CELL_W;
    int cy = GRID_TOP + row * CELL_H;
    int is_today = (day == today_day);
    int rest = is_rest_day(year, month, day);

    // Clear cell interior (preserve row separator lines at edges)
    st7306_draw_filled_rect(cx + 1, cy + 1, CELL_W - 2, CELL_H - 2, ST7306_COLOR_WHITE);

    // Background
    if (is_today) {
        st7306_draw_filled_rect(cx + 1, cy + 1, CELL_W - 2, CELL_H - 2, ST7306_COLOR_BLACK);
    } else if (rest) {
        for (int dy = cy + 2; dy < cy + CELL_H - 2; dy += 2) {
            st7306_draw_hline(cx + 2, cx + CELL_W - 3, dy, ST7306_COLOR_BLACK);
        }
    }

    // Day number
    char day_str[12];
    snprintf(day_str, sizeof(day_str), "%u", (unsigned)day);
    int ndigits = strlen(day_str);
    int dw = ndigits * DAY_NUM_W + (ndigits - 1) * 2;
    int dx = cx + (CELL_W - dw) / 2;
    int dy = cy + (CELL_H - DAY_NUM_H) / 2;
    int color = is_today ? ST7306_COLOR_WHITE : ST7306_COLOR_BLACK;
    for (int ci = 0; ci < ndigits; ci++) {
        st7306_draw_digit_10x20(dx + ci * (DAY_NUM_W + 2), dy, day_str[ci], color);
    }

    // Event indicator
    if (has_events_on(year, month, day)) {
        int bx = dx - 2, by = dy - 1, bw = dw + 4, bh = DAY_NUM_H + 2;
        int bc = is_today ? ST7306_COLOR_WHITE : ST7306_COLOR_BLACK;
        st7306_draw_hline(bx + 3, bx + bw - 4, by, bc);
        st7306_draw_hline(bx + 3, bx + bw - 4, by + bh - 1, bc);
        st7306_draw_vline(bx, by + 3, by + bh - 4, bc);
        st7306_draw_vline(bx + bw - 1, by + 3, by + bh - 4, bc);
        st7306_set_pixel(bx + 2, by + 1, bc);
        st7306_set_pixel(bx + 1, by + 2, bc);
        st7306_set_pixel(bx + bw - 3, by + 1, bc);
        st7306_set_pixel(bx + bw - 2, by + 2, bc);
        st7306_set_pixel(bx + 2, by + bh - 2, bc);
        st7306_set_pixel(bx + 1, by + bh - 3, bc);
        st7306_set_pixel(bx + bw - 3, by + bh - 2, bc);
        st7306_set_pixel(bx + bw - 2, by + bh - 3, bc);
    }

    // Selection cursor
    if (draw_cursor) {
        int cursor_color = is_today ? ST7306_COLOR_WHITE : ST7306_COLOR_BLACK;
        st7306_draw_rect(cx + 1, cy + 1, CELL_W - 2, CELL_H - 2, cursor_color);
        st7306_draw_rect(cx + 3, cy + 3, CELL_W - 6, CELL_H - 6, cursor_color);
    }
}

// ── Keyboard navigation API ────────────────────────────────────────────────────

void calendar_select_day(int delta)
{
    if (!s_nav_initialized) return;

    if (!s_has_selection) {
        s_sel_year = s_today_year;
        s_sel_month = s_today_month;
        s_sel_day = s_today_day;
    }

    int old_year = s_sel_year, old_month = s_sel_month, old_day = s_sel_day;

    // Advance date by delta using mktime for normalization
    struct tm t = {0};
    t.tm_year = s_sel_year - 1900;
    t.tm_mon = s_sel_month - 1;
    t.tm_mday = s_sel_day + delta;
    t.tm_hour = 12;  // noon to avoid DST edge cases
    mktime(&t);
    s_sel_year = t.tm_year + 1900;
    s_sel_month = t.tm_mon + 1;
    s_sel_day = t.tm_mday;
    s_has_selection = true;

    ESP_LOGI("calendar", "Navigate: %d-%02d-%02d", s_sel_year, s_sel_month, s_sel_day);

    int today_day = (s_sel_year == s_today_year && s_sel_month == s_today_month)
                    ? s_today_day : 0;

    if (s_sel_month != old_month || s_sel_year != old_year) {
        // Cross-month: full redraw
        calendar_draw_navigate();
    } else {
        // Same month: partial redraw — only update 2 cells + header
        redraw_single_cell(old_year, old_month, old_day, today_day, false);
        redraw_single_cell(s_sel_year, s_sel_month, s_sel_day, today_day, true);
        // Update header to show selected date
        st7306_draw_filled_rect(CAL_X, 0, CAL_WIDTH, HEADER_H, ST7306_COLOR_WHITE);
        draw_header(s_sel_year, s_sel_month, s_sel_day);
    }
}

void calendar_confirm_selection(void)
{
    if (!s_has_selection) return;
    ESP_LOGI("calendar", "Confirm: %d-%02d-%02d", s_sel_year, s_sel_month, s_sel_day);
    calendar_draw_navigate();
}

void calendar_clear_selection(void)
{
    s_has_selection = false;
}

// ── Status message screen ──────────────────────────────────────────────────────
void calendar_draw_status(const char *line1, const char *line2)
{
    st7306_clear();
    int y = (ST7306_HEIGHT - 32) / 2;
    if (line1) {
        int x = (ST7306_WIDTH - st7306_text_width(line1)) / 2;
        st7306_draw_text(x, y, line1, ST7306_COLOR_BLACK);
    }
    if (line2) {
        int x = (ST7306_WIDTH - st7306_text_width(line2)) / 2;
        st7306_draw_text(x, y + 20, line2, ST7306_COLOR_BLACK);
    }
    st7306_update_display();
}

// "室内" GB2312
static const uint8_t gb_shinei[] = {0xCA, 0xD2, 0xC4, 0xDA, 0};

void calendar_draw_env_data(float temp_c, float humidity_pct)
{
    // Place at y=248-264 in left panel (just above status bar)
    int y = 248;
    st7306_draw_filled_rect(0, y, 132, 16, ST7306_COLOR_WHITE);

    char buf[32];
    int x = 2;
    x = hzk16_draw_gb_text(x, y, gb_shinei, ST7306_COLOR_BLACK);   // 室内
    snprintf(buf, sizeof(buf), "%.1f", temp_c);
    st7306_draw_text(x, y, buf, ST7306_COLOR_BLACK);
    x += strlen(buf) * 8;
    st7306_draw_text(x, y, "C", ST7306_COLOR_BLACK);
    x += 8;

    snprintf(buf, sizeof(buf), " %d%%", (int)humidity_pct);
    st7306_draw_text(x, y, buf, ST7306_COLOR_BLACK);
}
