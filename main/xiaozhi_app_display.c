/*
 * XiaoZhi standalone app display (400×300 monochrome)
 * Layout follows XiaoZhi V2.1.0 simple mode:
 *   - Top: status bar (WiFi + clock + battery)
 *   - Center: "小智" + state label
 *   - Middle: chat text area (14px, multi-line)
 *   - Bottom: button hints
 */

#include "xiaozhi_app_display.h"
#include "st7306.h"
#include "hzk16.h"
#include "calendar.h"       // for calendar_draw_wifi_bars, calendar_draw_battery_icon
#include "utf8_gb2312.h"
#include "wifi_manager.h"
#include "battery.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

// ── Layout constants ─────────────────────────────────────────────────────────
#define XZ_TOP_BAR_H      28
#define XZ_CENTER_TOP     60
#define XZ_CHAT_TOP       210
#define XZ_BOTTOM_Y       268
#define XZ_MARGIN          8

// ── GB2312 pre-encoded strings ───────────────────────────────────────────────
static const uint8_t gb_xiaozhi[]    = {0xD0, 0xA1, 0xD6, 0xC7, 0};              // 小智
static const uint8_t gb_daiji[]     = {0xB4, 0xFD, 0xBB, 0xFA, 0};              // 待机
static const uint8_t gb_lingting[]  = {0xF1, 0xF6, 0xCC, 0xFD, 0xD6, 0xD0, 0};  // 聆听中
static const uint8_t gb_huifu[]     = {0xBB, 0xD8, 0xB8, 0xB4, 0xD6, 0xD0, 0};  // 回复中
static const uint8_t gb_lianjie[]   = {0xC1, 0xAC, 0xBD, 0xD3, 0xD6, 0xD0, 0};  // 连接中
static const uint8_t gb_cuowu[]     = {0xB4, 0xED, 0xCE, 0xF3, 0};              // 错误
static const uint8_t gb_anboot[]    = {0xB0, 0xB4, 0};                           // 按
static const uint8_t gb_duihua[]    = {0xB6, 0xD4, 0xBB, 0xB0, 0};              // 对话
static const uint8_t gb_qiehuan[]   = {0xC7, 0xD0, 0xBB, 0xBB, 0};              // 切换
static const uint8_t gb_rili[]      = {0xC8, 0xD5, 0xC0, 0xFA, 0};              // 日历
static const uint8_t gb_kaishi[]    = {0xBF, 0xAA, 0xCA, 0xBC, 0};              // 开始

// Persistent text buffer
static char s_chat_text[256] = {0};

// ── Draw top status bar ──────────────────────────────────────────────────────
void xiaozhi_app_draw_status_bar(struct tm *ti, int rssi, int battery_pct)
{
    // Clear top bar area
    st7306_draw_filled_rect(0, 0, ST7306_WIDTH, XZ_TOP_BAR_H, ST7306_COLOR_WHITE);
    st7306_draw_hline(0, ST7306_WIDTH - 1, XZ_TOP_BAR_H, ST7306_COLOR_BLACK);

    int y = (XZ_TOP_BAR_H - 16) / 2;

    // WiFi signal bars (left)
    calendar_draw_wifi_bars(XZ_MARGIN, y + 2, rssi);

    // Clock (center)
    char clock_str[12];
    snprintf(clock_str, sizeof(clock_str), "%02d:%02d:%02d",
             ti->tm_hour, ti->tm_min, ti->tm_sec);
    int clock_w = st7306_text_width(clock_str);
    int clock_x = (ST7306_WIDTH - clock_w) / 2;
    st7306_draw_text(clock_x, y, clock_str, ST7306_COLOR_BLACK);

    // Battery (right)
    char pct_str[8];
    snprintf(pct_str, sizeof(pct_str), "%d%%", battery_pct);
    int pct_w = st7306_text_width(pct_str);
    int batt_x = ST7306_WIDTH - pct_w - 30 - XZ_MARGIN - 6;
    calendar_draw_battery_icon(batt_x, y, battery_pct);
    st7306_draw_text(batt_x + 30, y, pct_str, ST7306_COLOR_BLACK);
}

// ── Draw center area (小智 + state) ──────────────────────────────────────────
static void draw_center_area(xiaozhi_state_t state)
{
    int center_y = XZ_CENTER_TOP;

    // Clear center area
    st7306_draw_filled_rect(0, XZ_TOP_BAR_H + 1, ST7306_WIDTH,
                            XZ_CHAT_TOP - XZ_TOP_BAR_H - 2, ST7306_COLOR_WHITE);

    // "小智" centered, 16px
    int xz_w = hzk16_text_width(gb_xiaozhi);
    int xz_x = (ST7306_WIDTH - xz_w) / 2;
    hzk16_draw_gb_text(xz_x, center_y, gb_xiaozhi, ST7306_COLOR_BLACK);
    center_y += 24;

    // State label centered, 14px
    const uint8_t *state_str = gb_daiji;
    switch (state) {
    case XZ_IDLE:       state_str = gb_daiji; break;
    case XZ_LISTENING:  state_str = gb_lingting; break;
    case XZ_SPEAKING:   state_str = gb_huifu; break;
    case XZ_CONNECTING: state_str = gb_lianjie; break;
    case XZ_ERROR:      state_str = gb_cuowu; break;
    }

    int state_w = hzk16_text_width(state_str);
    int state_x = (ST7306_WIDTH - state_w) / 2;
    int tx = hzk16_draw_gb_text(state_x, center_y, state_str, ST7306_COLOR_BLACK);

    // "..." for active states
    if (state == XZ_LISTENING || state == XZ_SPEAKING || state == XZ_CONNECTING) {
        st7306_draw_text(tx, center_y, "...", ST7306_COLOR_BLACK);
    }
    center_y += 22;

    // IDLE hint: "按BOOT开始对话"
    if (state == XZ_IDLE) {
        int hx = hzk16_draw_gb_text(state_x, center_y, gb_anboot, ST7306_COLOR_BLACK);
        st7306_draw_text(hx, center_y, "BOOT", ST7306_COLOR_BLACK);
        hx += 8 * 4;  // width of "BOOT" in 8x16 ASCII
        hx = hzk16_draw_gb_text(hx, center_y, gb_kaishi, ST7306_COLOR_BLACK);  // 开始
        hzk16_draw_gb_text(hx, center_y, gb_duihua, ST7306_COLOR_BLACK);  // 对话
    }
}

// ── Draw chat text area ──────────────────────────────────────────────────────
static void draw_chat_area(void)
{
    // Clear chat area
    st7306_draw_filled_rect(0, XZ_CHAT_TOP - 2, ST7306_WIDTH,
                            XZ_BOTTOM_Y - XZ_CHAT_TOP + 2, ST7306_COLOR_WHITE);
    st7306_draw_hline(0, ST7306_WIDTH - 1, XZ_CHAT_TOP - 2, ST7306_COLOR_BLACK);

    if (s_chat_text[0] == '\0') return;

    // Convert UTF-8 → GB2312
    uint8_t gb_buf[512];
    int gb_len = utf8_to_gb2312(s_chat_text, gb_buf, sizeof(gb_buf));
    if (gb_len <= 0) return;
    gb_buf[gb_len] = 0;

    // Draw line by line, 16px font, up to 3 lines
    const uint8_t *p = gb_buf;
    int text_y = XZ_CHAT_TOP + 2;
    int text_x = XZ_MARGIN;
    int max_px = ST7306_WIDTH - XZ_MARGIN * 2;
    int max_y = XZ_BOTTOM_Y - 4;

    for (int line = 0; line < 3 && *p && text_y + 16 <= max_y; line++) {
        uint8_t line_buf[80];
        int x = 0, si = 0, di = 0;
        while (p[si] && di < (int)sizeof(line_buf) - 2) {
            if (p[si] < 0x80) {
                if (x + 8 > max_px) break;
                line_buf[di++] = p[si++];
                x += 8;
            } else {
                if (x + 16 > max_px || di + 2 > (int)sizeof(line_buf) - 1) break;
                line_buf[di++] = p[si++];
                line_buf[di++] = p[si++];
                x += 16;
            }
        }
        line_buf[di] = 0;
        p += si;

        if (di > 0) {
            hzk16_draw_gb_text(text_x, text_y, line_buf, ST7306_COLOR_BLACK);
            text_y += 18;
        } else {
            break;
        }
    }
}

// ── Draw bottom hint bar ─────────────────────────────────────────────────────
static void draw_bottom_bar(void)
{
    st7306_draw_filled_rect(0, XZ_BOTTOM_Y, ST7306_WIDTH,
                            ST7306_HEIGHT - XZ_BOTTOM_Y, ST7306_COLOR_WHITE);
    st7306_draw_hline(0, ST7306_WIDTH - 1, XZ_BOTTOM_Y, ST7306_COLOR_BLACK);

    int y = XZ_BOTTOM_Y + (ST7306_HEIGHT - XZ_BOTTOM_Y - 16) / 2;

    // Left: "KEY:" + "切换日历"
    int x = XZ_MARGIN;
    st7306_draw_text(x, y, "KEY:", ST7306_COLOR_BLACK);
    x += 4 * 8;
    x = hzk16_draw_gb_text(x, y, gb_qiehuan, ST7306_COLOR_BLACK);  // 切换
    hzk16_draw_gb_text(x, y, gb_rili, ST7306_COLOR_BLACK);     // 日历

    // Right: "BOOT:" + "开始对话"
    static const uint8_t gb_maohao[] = {0xA3, 0xBA, 0};  // ：(fullwidth colon)
    // Right-align: BOOT(8*4) + 5 Chinese chars (16*5)
    int rx = ST7306_WIDTH - XZ_MARGIN - 8 * 4 - 16 * 5;
    st7306_draw_text(rx, y, "BOOT", ST7306_COLOR_BLACK);
    rx += 8 * 4;
    rx = hzk16_draw_gb_text(rx, y, gb_maohao, ST7306_COLOR_BLACK);
    rx = hzk16_draw_gb_text(rx, y, gb_kaishi, ST7306_COLOR_BLACK);  // 开始
    hzk16_draw_gb_text(rx, y, gb_duihua, ST7306_COLOR_BLACK);       // 对话
}

// ── Public API ───────────────────────────────────────────────────────────────

void xiaozhi_app_draw_full(xiaozhi_state_t state, const char *text)
{
    st7306_clear();

    // Update text buffer
    if (text) {
        strncpy(s_chat_text, text, sizeof(s_chat_text) - 1);
        s_chat_text[sizeof(s_chat_text) - 1] = '\0';
    } else {
        s_chat_text[0] = '\0';
    }

    // Draw all sections
    time_t now;
    time(&now);
    struct tm ti;
    localtime_r(&now, &ti);
    xiaozhi_app_draw_status_bar(&ti, wifi_manager_get_rssi(), battery_get_level());
    draw_center_area(state);
    draw_chat_area();
    draw_bottom_bar();
}

void xiaozhi_app_update_state(xiaozhi_state_t state)
{
    draw_center_area(state);
}

void xiaozhi_app_update_text(const char *text)
{
    if (text) {
        strncpy(s_chat_text, text, sizeof(s_chat_text) - 1);
        s_chat_text[sizeof(s_chat_text) - 1] = '\0';
    } else {
        s_chat_text[0] = '\0';
    }
    draw_chat_area();
}
