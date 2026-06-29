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

// View state
typedef enum {
    CP_VIEW_SPLIT = 0,
    CP_VIEW_DETAIL,
    CP_VIEW_NOTIFY,
    CP_VIEW_COUNT,
} cp_view_t;

static const char *view_name(cp_view_t v) {
    switch (v) {
        case CP_VIEW_SPLIT:  return "SPLIT";
        case CP_VIEW_DETAIL: return "DETAIL";
        case CP_VIEW_NOTIFY: return "NOTIFY";
        default:             return "?";
    }
}

// Lifecycle state
static volatile bool   s_stop_flag = false;
static SemaphoreHandle_t s_exit_sem = NULL;
static TaskHandle_t    s_worker = NULL;     // CodePilot WS drain task
static TaskHandle_t    s_xiaozhi_task = NULL;  // XiaoZhi STT pipeline task
static SemaphoreHandle_t s_xz_exit_sem = NULL;
static cp_view_t       s_current_view = CP_VIEW_SPLIT;
static volatile bool   s_dirty = true;     // redraw required
static volatile bool   s_listening = false;  // true while KEY_LONG held

// STT input area (bottom 32px of content): latest user speech / AI text
#define CP_STT_BUF_LEN  200
static char s_stt_buf[CP_STT_BUF_LEN] = {0};
static volatile bool s_stt_dirty = false;

// Forward decls
static void draw_top_bar(void);
static void draw_bottom_bar(void);
static void draw_split_view(void);
static void draw_detail_view(void);
static void draw_notify_view(void);
static void render_current_view(void);

// ── Worker: drain WS recv queue → state_manager → mark dirty ────────────────
static void cp_worker(void *arg)
{
    ESP_LOGI(TAG, "Worker started");
    char buf[WS_MAX_MESSAGE_LEN];

    while (!s_stop_flag) {
        // Block up to 200ms waiting for a message so we can re-check stop_flag
        if (ws_server_recv_line(buf, sizeof(buf), pdMS_TO_TICKS(200))) {
            bool ok = state_manager_update_from_bridge(buf);
            if (ok) {
                s_dirty = true;
                ESP_LOGI(TAG, "Updated state from bridge: %.60s", buf);
            } else {
                ESP_LOGW(TAG, "Failed to process: %.60s", buf);
            }
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
    s_current_view = CP_VIEW_SPLIT;
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
}

void codepilot_on_key(key_event_t key)
{
    cp_view_t old = s_current_view;
    switch (key) {
        case KEY_BACK:
            app_manager_switch(APP_ID_MENU);
            return;
        case KEY_PREV:
            s_current_view = (cp_view_t)((s_current_view - 1 + CP_VIEW_COUNT) % CP_VIEW_COUNT);
            break;
        case KEY_NEXT:
            s_current_view = (cp_view_t)((s_current_view + 1) % CP_VIEW_COUNT);
            break;
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
    if (s_current_view != old) {
        ESP_LOGI(TAG, "View: %s -> %s", view_name(old), view_name(s_current_view));
        s_dirty = true;
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
    const char *vn = view_name(s_current_view);
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

    switch (s_current_view) {
        case CP_VIEW_SPLIT:  draw_split_view();  break;
        case CP_VIEW_DETAIL: draw_detail_view(); break;
        case CP_VIEW_NOTIFY: draw_notify_view(); break;
        default: break;
    }
    (void)content_bottom;

    draw_stt_area();
    draw_bottom_bar();
    st7306_update_display();
    app_manager_display_unlock();
}

// ASCII render of an agent's basic info to a horizontal panel
static void draw_agent_panel(int x, int y, int w, int h,
                              const agent_state_t *agent, bool focus)
{
    // Border
    st7306_draw_rect(x, y, w, h, ST7306_COLOR_BLACK);
    if (focus) {
        // Thicker border for focused panel: draw inner rect
        st7306_draw_rect(x + 2, y + 2, w - 4, h - 4, ST7306_COLOR_BLACK);
    }

    if (!agent) {
        // Empty slot
        return;
    }

    // Agent name (small, top-left)
    const char *name = protocol_agent_type_to_string(agent->type);
    st7306_draw_text(x + 6, y + 4, name, ST7306_COLOR_BLACK);

    // Active indicator (filled box vs hollow)
    if (agent->active) {
        st7306_draw_filled_rect(x + w - 16, y + 4, 10, 10, ST7306_COLOR_BLACK);
    } else {
        st7306_draw_rect(x + w - 16, y + 4, 10, 10, ST7306_COLOR_BLACK);
    }

    // Status string (next row)
    char status_line[40];
    snprintf(status_line, sizeof(status_line), "%s", agent->status[0] ? agent->status : "-");
    st7306_draw_text(x + 6, y + 20, status_line, ST7306_COLOR_BLACK);

    // Quota progress bar
    int bar_y = y + h - 24;
    int bar_x = x + 6;
    int bar_w = w - 12;
    st7306_draw_rect(bar_x, bar_y, bar_w, 8, ST7306_COLOR_BLACK);
    if (agent->quota_total > 0) {
        int fill = (int)((bar_w - 2) * (uint64_t)agent->quota_used / agent->quota_total);
        if (fill > 0) {
            st7306_draw_filled_rect(bar_x + 1, bar_y + 1, fill, 6, ST7306_COLOR_BLACK);
        }
    }

    // Quota text below
    char q[32];
    snprintf(q, sizeof(q), "%lu/%lu", (unsigned long)agent->quota_used,
             (unsigned long)agent->quota_total);
    st7306_draw_text(bar_x, bar_y + 10, q, ST7306_COLOR_BLACK);
}

static void draw_split_view(void)
{
    state_manager_lock();
    const global_state_t *st = state_manager_get_state();

    // Show first two agents (Claude + Kimi typically)
    const agent_state_t *a0 = st->agent_count >= 1 ? &st->agents[0] : NULL;
    const agent_state_t *a1 = st->agent_count >= 2 ? &st->agents[1] : NULL;

    int half_w = (ST7306_WIDTH - 8) / 2;
    int panel_h = CP_CONTENT_BOTTOM - CP_CONTENT_TOP - 4;
    draw_agent_panel(2,                  CP_CONTENT_TOP, half_w, panel_h, a0, true);
    draw_agent_panel(4 + half_w + 2,     CP_CONTENT_TOP, half_w, panel_h, a1, false);

    // Connection state at bottom of content
    const uint8_t *conn = ws_server_is_connected() ? gb_yilian : gb_weilian;
    int cw = hzk16_text_width(conn);
    hzk16_draw_gb_text((ST7306_WIDTH - cw) / 2, CP_CONTENT_BOTTOM - 18, conn, ST7306_COLOR_BLACK);

    state_manager_unlock();
}

static void draw_detail_view(void)
{
    state_manager_lock();
    const global_state_t *st = state_manager_get_state();

    if (st->agent_count == 0) {
        // No agent — show "未连接"
        const uint8_t *msg = gb_weilian;
        int w = hzk16_text_width(msg);
        hzk16_draw_gb_text((ST7306_WIDTH - w) / 2, CP_CONTENT_TOP + 20, msg, ST7306_COLOR_BLACK);
        state_manager_unlock();
        return;
    }

    const agent_state_t *a = &st->agents[0];  // Claude typically

    // Big name
    const char *name = protocol_agent_type_to_string(a->type);
    int nw = st7306_text_width(name);
    st7306_draw_text((ST7306_WIDTH - nw) / 2, CP_CONTENT_TOP, name, ST7306_COLOR_BLACK);

    // Active state — large box
    int box_y = CP_CONTENT_TOP + 20;
    if (a->active) {
        st7306_draw_filled_rect(ST7306_WIDTH/2 - 30, box_y, 60, 30, ST7306_COLOR_BLACK);
        st7306_draw_text(ST7306_WIDTH/2 - 14, box_y + 8, "ACT", ST7306_COLOR_WHITE);
    } else {
        st7306_draw_rect(ST7306_WIDTH/2 - 30, box_y, 60, 30, ST7306_COLOR_BLACK);
        st7306_draw_text(ST7306_WIDTH/2 - 12, box_y + 8, "IDLE", ST7306_COLOR_BLACK);
    }

    // Quota progress bar (large)
    int bar_y = box_y + 50;
    int bar_x = 20;
    int bar_w = ST7306_WIDTH - 40;
    st7306_draw_rect(bar_x, bar_y, bar_w, 16, ST7306_COLOR_BLACK);
    if (a->quota_total > 0) {
        int fill = (int)((bar_w - 2) * (uint64_t)a->quota_used / a->quota_total);
        if (fill > 0) {
            st7306_draw_filled_rect(bar_x + 1, bar_y + 1, fill, 14, ST7306_COLOR_BLACK);
        }
    }
    char q[40];
    snprintf(q, sizeof(q), "%lu / %lu", (unsigned long)a->quota_used,
             (unsigned long)a->quota_total);
    st7306_draw_text(bar_x, bar_y + 22, q, ST7306_COLOR_BLACK);

    // Status line
    char s[40];
    snprintf(s, sizeof(s), "Status: %s", a->status[0] ? a->status : "-");
    st7306_draw_text(20, bar_y + 50, s, ST7306_COLOR_BLACK);

    state_manager_unlock();
}

static void draw_notify_view(void)
{
    state_manager_lock();
    const notification_t *n = state_manager_get_highest_priority_notification();

    if (!n) {
        const uint8_t no_msg[] = {0xCE, 0xDE, 0xCD, 0xA8, 0xD6, 0xAA, 0};  // 无通知
        int w = hzk16_text_width(no_msg);
        hzk16_draw_gb_text((ST7306_WIDTH - w) / 2, ST7306_HEIGHT / 2 - 8,
                          no_msg, ST7306_COLOR_BLACK);
    } else {
        char id_line[80];
        snprintf(id_line, sizeof(id_line), "ID: %.60s", n->id);
        st7306_draw_text(8, CP_CONTENT_TOP, id_line, ST7306_COLOR_BLACK);

        char sev_line[40];
        snprintf(sev_line, sizeof(sev_line), "Severity: %s",
                 protocol_severity_to_string(n->severity));
        st7306_draw_text(8, CP_CONTENT_TOP + 20, sev_line, ST7306_COLOR_BLACK);

        // Message body (ASCII, may be truncated)
        char msg[80];
        snprintf(msg, sizeof(msg), "%.70s", n->message);
        st7306_draw_text(8, CP_CONTENT_TOP + 50, msg, ST7306_COLOR_BLACK);
    }

    state_manager_unlock();
}
