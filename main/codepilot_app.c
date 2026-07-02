/*
 * codepilot_app.c — CodePilot dashboard (Phase B)
 *
 * Receives Claude/Kimi status from PC bridge via WebSocket (port 7897),
 * parses NDJSON session.update messages, and renders to ST7306.
 *
 * 3 views (PREV/NEXT cycle):
 *   - SPLIT      : Claude + Kimi side by side
 *   - DETAIL     : Claude quota/CPU large display
 *   - NOTIFY     : latest notification full-screen
 *
 * Long-press GPIO18 in B.4 will trigger XiaoZhi STT (muted speaker).
 *
 * Bridge.js wire format (NDJSON):
 *   {"type":"session.update","provider":"Claude","status":"Connected",
 *    "current_task":"...","active":true,
 *    "quota_used":N,"quota_total":N,"timestamp":N}
 */

#include "codepilot_app.h"
#include "app_manager.h"
#include "st7306.h"
#include "hzk16.h"
#include "xiaozhi_app_display.h"  // reuse status bar
#include "xiaozhi_bridge.h"       // STT pipeline + speaker mute
#include "wifi_manager.h"
#include "battery.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <time.h>
#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "state_manager.h"
#include "ws_server.h"
#include "serial_input.h"        // USB Serial/JTAG RX (bridge_usb.js path)

static const char *TAG = "CodePilot";

// Layout
#define CP_TOP_BAR_H       28
#define CP_BOTTOM_BAR_H    24
#define CP_CONTENT_TOP     (CP_TOP_BAR_H + 4)
#define CP_CONTENT_BOTTOM  (ST7306_HEIGHT - CP_BOTTOM_BAR_H - 4)

// GB2312 pre-encoded
static const uint8_t gb_dengdai[]  = {0xB5, 0xC8, 0xB4, 0xFD, 0xCA, 0xFD, 0xBE, 0xDD, 0};  // 等待数据
static const uint8_t gb_weilian[]  = {0xCE, 0xB4, 0xC1, 0xAC, 0xBD, 0xD3, 0};              // 未连接
static const uint8_t gb_yilian[]   = {0xD2, 0xD1, 0xC1, 0xAC, 0xBD, 0xD3, 0};              // 已连接
static const uint8_t gb_fan[]      = {0xB7, 0xB5, 0xBB, 0xD8, 0};
static const uint8_t gb_caidan[]   = {0xB2, 0xCB, 0xB5, 0xA5, 0};

// View state — single full-screen agent view. PREV/NEXT cycle the agent
// (browser-tab style), not the view. Future INTERACT view for permission
// requests will be added here.
typedef enum {
    CP_VIEW_AGENT = 0,
    CP_VIEW_COUNT,
} cp_view_t;

// Lifecycle state
static volatile bool   s_stop_flag = false;
static SemaphoreHandle_t s_exit_sem = NULL;
static TaskHandle_t    s_worker = NULL;     // CodePilot WS drain task
static TaskHandle_t    s_xiaozhi_task = NULL;  // XiaoZhi STT pipeline task
static SemaphoreHandle_t s_xz_exit_sem = NULL;
static cp_view_t       s_current_view = CP_VIEW_AGENT;
static uint8_t         s_current_agent_idx = 0;  // tab cursor, 0..agent_count-1
static volatile bool   s_dirty = true;     // redraw required
static volatile bool   s_listening = false;  // true while KEY_LONG held

// STT input area (bottom 32px of content): latest user speech / AI text
#define CP_STT_BUF_LEN  200
static char s_stt_buf[CP_STT_BUF_LEN] = {0};
static volatile bool s_stt_dirty = false;

// Forward decls
static void draw_top_bar(void);
static void draw_bottom_bar(void);
static void draw_agent_view(void);
static void render_current_view(void);

// ── Worker: drain bridge queues → state_manager → mark dirty ────────────────
// Two transports, one consumer. USB serial (bridge_usb.js) is primary; the WS
// server is kept as a debug/fallback path. Poll serial first with a real
// timeout, then non-blocking WS in case both clients happen to be active.
static void cp_worker(void *arg)
{
    ESP_LOGI(TAG, "Worker started");
    char buf[WS_MAX_MESSAGE_LEN];

    while (!s_stop_flag) {
        buf[0] = '\0';
        if (serial_input_recv_line(buf, sizeof(buf), pdMS_TO_TICKS(100))) {
            // fall through to process
        } else if (ws_server_recv_line(buf, sizeof(buf), 0)) {
            // fall through to process
        } else {
            continue;  // neither had data; loop back, re-check stop_flag
        }

        bool ok = state_manager_update_from_bridge(buf);
        if (ok) {
            s_dirty = true;
            ESP_LOGI(TAG, "Bridge: %.60s", buf);
        } else {
            ESP_LOGW(TAG, "Bad msg: %.60s", buf);
        }
    }

    ESP_LOGI(TAG, "Worker exiting");
    xSemaphoreGive(s_exit_sem);
    s_worker = NULL;
    vTaskDelete(NULL);
}

// ── XiaoZhi STT pipeline task wrapper ────────────────────────────────────────
// Runs xiaozhi_run() which exits cleanly when XZ_EVT_KILL is set.
static void cp_xiaozhi_task(void *arg)
{
    ESP_LOGI(TAG, "XiaoZhi STT task started");
    xiaozhi_run();
    ESP_LOGI(TAG, "XiaoZhi STT task exiting");
    xSemaphoreGive(s_xz_exit_sem);
    s_xiaozhi_task = NULL;
    vTaskDelete(NULL);
}

// Public: receive STT text from xiaozhi_display.c
void codepilot_receive_stt(const char *text)
{
    if (!text) return;
    strncpy(s_stt_buf, text, CP_STT_BUF_LEN - 1);
    s_stt_buf[CP_STT_BUF_LEN - 1] = '\0';
    s_stt_dirty = true;
    s_dirty = true;
    ESP_LOGI(TAG, "STT: %.80s", s_stt_buf);
}

// ── Public API ───────────────────────────────────────────────────────────────
void codepilot_on_enter(void)
{
    ESP_LOGI(TAG, "Entering CodePilot");

    s_stop_flag = false;
    s_current_view = CP_VIEW_AGENT;
    s_current_agent_idx = 0;
    s_dirty = true;
    s_listening = false;
    s_stt_buf[0] = '\0';
    s_stt_dirty = false;
    if (!s_exit_sem)    s_exit_sem    = xSemaphoreCreateBinary();
    if (!s_xz_exit_sem) s_xz_exit_sem = xSemaphoreCreateBinary();
    xSemaphoreTake(s_exit_sem, 0);
    xSemaphoreTake(s_xz_exit_sem, 0);

    state_manager_init();
    ws_server_start();
    serial_input_start();   // USB Serial/JTAG RX for bridge_usb.js

    // Initial screen
    app_manager_display_lock();
    st7306_clear();
    draw_top_bar();
    {
        // Centered "等待数据" + "未连接"
        int y = ST7306_HEIGHT / 2 - 16;
        int w = hzk16_text_width(gb_dengdai);
        hzk16_draw_gb_text((ST7306_WIDTH - w) / 2, y, gb_dengdai, ST7306_COLOR_BLACK);
        y += 24;
        w = hzk16_text_width(gb_weilian);
        hzk16_draw_gb_text((ST7306_WIDTH - w) / 2, y, gb_weilian, ST7306_COLOR_BLACK);
    }
    draw_bottom_bar();
    st7306_update_display();
    app_manager_display_unlock();

    if (xTaskCreate(cp_worker, "cp_work", 8192, NULL, 1, &s_worker) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create worker task");
        s_worker = NULL;
    }

    // Start XiaoZhi STT pipeline task (sits in XZ_IDLE until long-press)
    // xiaozhi_init() was already called at boot.
    xiaozhi_prepare_reconnect();
    if (xTaskCreate(cp_xiaozhi_task, "cp_xz", 32768, NULL, 1, &s_xiaozhi_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create xiaozhi task");
        s_xiaozhi_task = NULL;
    }
}

void codepilot_on_exit(void)
{
    ESP_LOGI(TAG, "Exiting CodePilot");
    s_stop_flag = true;

    // Make sure speaker is unmuted even if KEY_LONG_END was missed
    if (s_listening) {
        xiaozhi_stop_listening();
        xiaozhi_set_speaker_mute(false);
        s_listening = false;
    }

    // Kill XiaoZhi task
    if (s_xiaozhi_task) {
        xiaozhi_force_disconnect();
        if (xSemaphoreTake(s_xz_exit_sem, pdMS_TO_TICKS(2000)) != pdPASS) {
            ESP_LOGE(TAG, "XiaoZhi task did not exit in 2s, force killing");
            vTaskDelete(s_xiaozhi_task);
            s_xiaozhi_task = NULL;
        }
    }

    // Kill CodePilot worker
    if (s_worker) {
        if (xSemaphoreTake(s_exit_sem, pdMS_TO_TICKS(2000)) != pdPASS) {
            ESP_LOGE(TAG, "Worker did not exit in 2s, force killing");
            vTaskDelete(s_worker);
            s_worker = NULL;
        }
    }
    ws_server_stop();
    serial_input_stop();
}

void codepilot_on_key(key_event_t key)
{
    switch (key) {
        case KEY_BACK:
            app_manager_switch(APP_ID_MENU);
            return;
        case KEY_PREV:
        case KEY_NEXT: {
            // Cycle the agent tab cursor (browser-tab style), not the view.
            state_manager_lock();
            uint8_t cnt = state_manager_get_state()->agent_count;
            state_manager_unlock();
            if (cnt == 0) return;
            int dir = (key == KEY_NEXT) ? 1 : -1;
            uint8_t old = s_current_agent_idx;
            s_current_agent_idx = (uint8_t)((old + dir + cnt) % cnt);
            if (s_current_agent_idx != old) {
                ESP_LOGI(TAG, "Agent tab: %u -> %u", old, s_current_agent_idx);
                s_dirty = true;
            }
            return;
        }
        case KEY_LONG_START:
            // Begin voice input: mute speaker, trigger XiaoZhi STT
            if (!s_listening) {
                ESP_LOGI(TAG, "Voice input start (long-press)");
                xiaozhi_set_speaker_mute(true);
                xiaozhi_start_listening();
                s_listening = true;
            }
            return;
        case KEY_LONG_END:
            // End voice input: stop listening, unmute speaker
            if (s_listening) {
                ESP_LOGI(TAG, "Voice input end (release)");
                xiaozhi_stop_listening();
                xiaozhi_set_speaker_mute(false);
                s_listening = false;
            }
            return;
        default:
            return;
    }
}

void codepilot_on_tick_1s(void)
{
    if (!s_dirty) {
        // Even if no new data, still refresh top bar (clock) every second
        app_manager_display_lock();
        draw_top_bar();
        st7306_update_display();
        app_manager_display_unlock();
        return;
    }

    render_current_view();
    s_dirty = false;
}

// ── Render helpers ───────────────────────────────────────────────────────────
static void draw_top_bar(void)
{
    st7306_draw_filled_rect(0, 0, ST7306_WIDTH, CP_TOP_BAR_H, ST7306_COLOR_WHITE);
    st7306_draw_hline(0, ST7306_WIDTH - 1, CP_TOP_BAR_H, ST7306_COLOR_BLACK);

    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    xiaozhi_app_draw_status_bar(&ti, wifi_manager_get_rssi(), battery_get_level());

    // Connection status on right of top bar (overwrites battery area slightly)
    const uint8_t *conn = ws_server_is_connected() ? gb_yilian : gb_weilian;
    int conn_w = hzk16_text_width(conn);
    // (status bar layout is fixed — we leave it alone to avoid clutter; conn shown elsewhere)
    (void)conn; (void)conn_w;
}

static void draw_bottom_bar(void)
{
    int y = ST7306_HEIGHT - CP_BOTTOM_BAR_H;
    st7306_draw_filled_rect(0, y, ST7306_WIDTH, CP_BOTTOM_BAR_H, ST7306_COLOR_WHITE);
    st7306_draw_hline(0, ST7306_WIDTH - 1, y, ST7306_COLOR_BLACK);

    // LEFT: "BACK: 返回菜单"   RIGHT: view name
    int x = 4;
    st7306_draw_text(x, y + 4, "BACK:", ST7306_COLOR_BLACK);
    x += 6 * 8;
    x = hzk16_draw_gb_text(x, y + 4, gb_fan, ST7306_COLOR_BLACK);
    hzk16_draw_gb_text(x, y + 4, gb_caidan, ST7306_COLOR_BLACK);

    // View indicator
    const char *vn = "AGENT";
    int vn_w = st7306_text_width(vn);
    st7306_draw_text(ST7306_WIDTH - vn_w - 4, y + 4, vn, ST7306_COLOR_BLACK);
}

static void draw_stt_area(void)
{
    // Reserve bottom 32px of content area for STT input
    int y = CP_CONTENT_BOTTOM - 32;
    st7306_draw_filled_rect(0, y, ST7306_WIDTH, 32, ST7306_COLOR_WHITE);
    st7306_draw_hline(0, ST7306_WIDTH - 1, y, ST7306_COLOR_BLACK);

    // Label: "🎤" indicator + listening state
    if (s_listening) {
        st7306_draw_text(4, y + 2, "MIC", ST7306_COLOR_BLACK);
        // Blinking effect: draw filled box when listening
        st7306_draw_filled_rect(ST7306_WIDTH - 16, y + 4, 8, 8, ST7306_COLOR_BLACK);
    }

    // Render STT text (truncated to fit width)
    if (s_stt_buf[0]) {
        // Convert UTF-8 → GB2312 for display
        extern int utf8_to_gb2312(const char *utf8, uint8_t *gb, int gb_max);
        uint8_t gb[CP_STT_BUF_LEN * 2];
        int gb_len = utf8_to_gb2312(s_stt_buf, gb, sizeof(gb) - 1);
        if (gb_len > 0) {
            gb[gb_len] = 0;
            // Truncate to fit width (~384px usable)
            int max_px = ST7306_WIDTH - 40;
            uint8_t line[CP_STT_BUF_LEN * 2];
            int x = 0, si = 0, di = 0;
            while (gb[si] && di < (int)sizeof(line) - 2) {
                if (gb[si] < 0x80) {
                    if (x + 8 > max_px) break;
                    line[di++] = gb[si++];
                    x += 8;
                } else {
                    if (x + 16 > max_px) break;
                    line[di++] = gb[si++];
                    line[di++] = gb[si++];
                    x += 16;
                }
            }
            line[di] = 0;
            hzk16_draw_gb_text(36, y + 8, line, ST7306_COLOR_BLACK);
        } else {
            // Fallback: ASCII render
            st7306_draw_text(36, y + 8, s_stt_buf, ST7306_COLOR_BLACK);
        }
    }
}

static void render_current_view(void)
{
    app_manager_display_lock();
    draw_top_bar();

    // Clear content area
    st7306_draw_filled_rect(0, CP_CONTENT_TOP, ST7306_WIDTH,
                            CP_CONTENT_BOTTOM - CP_CONTENT_TOP, ST7306_COLOR_WHITE);

    // Reserve bottom 32px for STT input
    int content_bottom = CP_CONTENT_BOTTOM - 34;
    (void)content_bottom;

    draw_agent_view();

    draw_stt_area();
    draw_bottom_bar();
    st7306_update_display();
    app_manager_display_unlock();
}

// Single full-screen agent view. PREV/NEXT cycle the agent tab cursor
// (s_current_agent_idx), not the view. Layout:
//   y=36   tab row   "name (idx+1/count)"
//   y=54   hline
//   y=60   status line  with ■/□ active indicator
//   y=80   current task (truncated)
//   y=96   hline
//   y=104  quota progress bar  (h=20, the headline number)
//   y=130  quota %  +  $cost
//   y=148  hline
//   y=156  $rate/h      |     session time   (left/right)
//   y=176  +added / -removed lines
//   y=196  project name (truncated)
// Content area y=32..240 (STT band claims 240..272).
static void draw_agent_view(void)
{
    state_manager_lock();
    const global_state_t *st = state_manager_get_state();

    if (st->agent_count == 0) {
        // No data yet — show "未连接" / "等待数据" stacked, centered
        int w1 = hzk16_text_width(gb_weilian);
        hzk16_draw_gb_text((ST7306_WIDTH - w1) / 2, CP_CONTENT_TOP + 30,
                          gb_weilian, ST7306_COLOR_BLACK);
        int w2 = hzk16_text_width(gb_dengdai);
        hzk16_draw_gb_text((ST7306_WIDTH - w2) / 2, CP_CONTENT_TOP + 60,
                          gb_dengdai, ST7306_COLOR_BLACK);
        state_manager_unlock();
        return;
    }

    // Clamp tab cursor in case agent_count shrank (agent disconnected)
    if (s_current_agent_idx >= st->agent_count) s_current_agent_idx = 0;
    const agent_state_t *a = &st->agents[s_current_agent_idx];

    // === Section 1: identity ===
    char tab[40];
    snprintf(tab, sizeof(tab), "%s  (%d/%d)",
             protocol_agent_type_to_string(a->type),
             s_current_agent_idx + 1, st->agent_count);
    st7306_draw_text(8, 36, tab, ST7306_COLOR_BLACK);
    st7306_draw_hline(0, ST7306_WIDTH - 1, 54, ST7306_COLOR_BLACK);

    // === Section 2: status + task ===
    if (a->active) {
        st7306_draw_filled_rect(8, 62, 12, 12, ST7306_COLOR_BLACK);
    } else {
        st7306_draw_rect(8, 62, 12, 12, ST7306_COLOR_BLACK);
    }
    char stat[48];
    snprintf(stat, sizeof(stat), "%s  %s",
             a->active ? "Active" : "Idle",
             a->status[0] ? a->status : "-");
    st7306_draw_text(24, 60, stat, ST7306_COLOR_BLACK);

    if (a->current_task[0]) {
        char task[40];
        snprintf(task, sizeof(task), "%.34s", a->current_task);
        st7306_draw_text(8, 80, task, ST7306_COLOR_BLACK);
    }
    st7306_draw_hline(0, ST7306_WIDTH - 1, 96, ST7306_COLOR_BLACK);

    // === Section 3: quota (the headline) ===
    int bar_x = 20, bar_y = 104, bar_w = ST7306_WIDTH - 40, bar_h = 20;
    st7306_draw_rect(bar_x, bar_y, bar_w, bar_h, ST7306_COLOR_BLACK);
    if (a->quota_total > 0) {
        int fill = (int)((bar_w - 2) * (uint64_t)a->quota_used / a->quota_total);
        if (fill > 0) {
            st7306_draw_filled_rect(bar_x + 1, bar_y + 1, fill, bar_h - 2, ST7306_COLOR_BLACK);
        }
    }
    {
        char left[16], right[32];
        if (a->quota_total > 0) {
            snprintf(left, sizeof(left), "%lu%%", (unsigned long)a->quota_used);
        } else {
            snprintf(left, sizeof(left), "--%%");
        }
        st7306_draw_text(bar_x, bar_y + bar_h + 4, left, ST7306_COLOR_BLACK);

        if (a->cost_cents > 0) {
            snprintf(right, sizeof(right), "$%lu.%02lu",
                     (unsigned long)a->cost_cents / 100,
                     (unsigned long)a->cost_cents % 100);
            int rw = st7306_text_width(right);
            st7306_draw_text(bar_x + bar_w - rw, bar_y + bar_h + 4, right, ST7306_COLOR_BLACK);
        }
    }
    st7306_draw_hline(0, ST7306_WIDTH - 1, 148, ST7306_COLOR_BLACK);

    // === Section 4: rate / session / lines / project ===
    {
        char rate[24];
        if (a->rate_cents_per_hour > 0) {
            snprintf(rate, sizeof(rate), "$%lu.%02lu/h",
                     (unsigned long)a->rate_cents_per_hour / 100,
                     (unsigned long)a->rate_cents_per_hour % 100);
            st7306_draw_text(8, 156, rate, ST7306_COLOR_BLACK);
        }
    }
    {
        char sess[24];
        if (a->session_minutes > 0) {
            unsigned h = a->session_minutes / 60;
            unsigned m = a->session_minutes % 60;
            if (h > 0) snprintf(sess, sizeof(sess), "%uh %umin", h, m);
            else       snprintf(sess, sizeof(sess), "%umin", m);
            int sw = st7306_text_width(sess);
            st7306_draw_text(ST7306_WIDTH - sw - 8, 156, sess, ST7306_COLOR_BLACK);
        }
    }
    if (a->lines_added > 0 || a->lines_removed > 0) {
        char ln[40];
        snprintf(ln, sizeof(ln), "+%lu / -%lu lines",
                 (unsigned long)a->lines_added,
                 (unsigned long)a->lines_removed);
        st7306_draw_text(8, 176, ln, ST7306_COLOR_BLACK);
    }
    if (a->project[0]) {
        char pr[28];
        snprintf(pr, sizeof(pr), "%.24s", a->project);
        st7306_draw_text(8, 196, pr, ST7306_COLOR_BLACK);
    }

    state_manager_unlock();
}
