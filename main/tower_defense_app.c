/*
 * tower_defense_app.c — Turret defense game (Phase E)
 *
 * Player turret sits at screen center. PREV/NEXT rotate the barrel in
 * 22.5° steps (16 directions), ENTER fires a single bullet (with
 * cooldown). Enemies spawn from screen edges and walk toward the
 * turret; if they reach it, player loses a life. 3 lives, 0 = game over.
 *
 * Endless waves: each wave adds enemies and accelerates spawn rate.
 *
 * Lifecycle mirrors snake_app.c: cooperative worker task with
 * stop_flag + binary semaphore, 2-second shutdown timeout.
 */

#include "tower_defense_app.h"
#include "app_manager.h"
#include "st7306.h"
#include "hzk16.h"
#include "xiaozhi_app_display.h"
#include "wifi_manager.h"
#include "battery.h"
#include "keyboard.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "TD";

// Layout
#define TD_TOP_BAR_H        28
#define TD_BOTTOM_BAR_H     32
#define TD_PLAYFIELD_TOP    TD_TOP_BAR_H
#define TD_PLAYFIELD_BOTTOM (ST7306_HEIGHT - TD_BOTTOM_BAR_H)
#define TD_CENTER_X         (ST7306_WIDTH / 2)
#define TD_CENTER_Y         ((TD_PLAYFIELD_TOP + TD_PLAYFIELD_BOTTOM) / 2)

// Game tuning
#define TD_MAX_ENEMIES         12
#define TD_MAX_BULLETS          6
#define TD_LIVES_START          3
#define TD_BREACH_RADIUS_SQ  (16 * 16)   // enemy reaches turret, dist² < this
#define TD_HIT_RADIUS_SQ     (10 * 10)   // bullet hits enemy, dist² < this
#define TD_BULLET_LIFE         60         // ticks (3s @ 20Hz)
#define TD_BULLET_SPEED         4         // px/tick (dir16 magnitude is ~4)
#define TD_FIRE_COOLDOWN        8         // ticks between shots (~400ms)
#define TD_WAVE_PREP_TICKS     60         // 3s between waves

// 16-step direction table (magnitude ~4). 0=up, 4=right, 8=down, 12=left
static const int8_t dir16_x[16] = {
    0,  1,  2,  3,  4,  3,  2,  1,
    0, -1, -2, -3, -4, -3, -2, -1
};
static const int8_t dir16_y[16] = {
   -4, -3, -2, -1,  0,  1,  2,  3,
    4,  3,  2,  1,  0, -1, -2, -3
};

// Enemy types
enum {
    TD_BASIC = 0,
    TD_FAST,
    TD_TANKY,
};

typedef struct {
    int16_t x, y;
    int8_t  vx, vy;
    uint8_t hp;
    uint8_t type;
    bool    active;
} td_enemy_t;

typedef struct {
    int16_t x, y;
    int8_t  dx, dy;
    uint8_t life;
    bool    active;
} td_bullet_t;

// GB2312 pre-encoded labels
static const uint8_t gb_fen_shu[]   = {0xB7, 0xD6, 0xCA, 0xFD, 0};                          // 分数
static const uint8_t gb_di[]        = {0xB5, 0xDA, 0};                                       // 第
static const uint8_t gb_bo[]        = {0xB2, 0xA8, 0};                                       // 波
static const uint8_t gb_you_xi_jieshu[] = {0xD3, 0xCE, 0xCF, 0xB7, 0xBD, 0xE1, 0xCA, 0xF8, 0}; // 游戏结束
static const uint8_t gb_chong_kai[] = {0xD6, 0xD8, 0xBF, 0xAA, 0};                          // 重开
static const uint8_t gb_fan_hui[]   = {0xB7, 0xB5, 0xBB, 0xD8, 0};                          // 返回
static const uint8_t gb_cai_dan[]   = {0xB2, 0xCB, 0xB5, 0xA5, 0};                          // 菜单
static const uint8_t gb_miao_zhun[] = {0xC3, 0xE9, 0xD7, 0xBC, 0};                          // 瞄准
static const uint8_t gb_kai_huo[]   = {0xBF, 0xAA, 0xBB, 0xF0, 0};                          // 开火
static const uint8_t gb_tui_chu[]   = {0xCD, 0xCB, 0xB3, 0xF6, 0};                          // 退出

// State
static td_enemy_t  s_enemies[TD_MAX_ENEMIES];
static td_bullet_t s_bullets[TD_MAX_BULLETS];

static int8_t  s_turret_angle = 0;
static int     s_score = 0;
static int     s_lives = TD_LIVES_START;
static int     s_wave = 1;
static int     s_wave_total = 0;
static int     s_wave_spawned = 0;
static int     s_ticks_to_spawn = 0;
static int     s_ticks_to_next_wave = 0;
static int     s_fire_cooldown = 0;
static bool    s_wave_intermission = false;
static bool    s_game_over = false;

// Lifecycle
static volatile bool s_stop_flag = false;
static SemaphoreHandle_t s_exit_sem = NULL;
static TaskHandle_t s_worker = NULL;

// ── Helpers ─────────────────────────────────────────────────────────────────
static int find_free_enemy_slot(void) {
    for (int i = 0; i < TD_MAX_ENEMIES; i++) if (!s_enemies[i].active) return i;
    return -1;
}

static int find_free_bullet_slot(void) {
    for (int i = 0; i < TD_MAX_BULLETS; i++) if (!s_bullets[i].active) return i;
    return -1;
}

static int count_active_enemies(void) {
    int n = 0;
    for (int i = 0; i < TD_MAX_ENEMIES; i++) if (s_enemies[i].active) n++;
    return n;
}

// ── Spawn ───────────────────────────────────────────────────────────────────
static void spawn_enemy(void) {
    int slot = find_free_enemy_slot();
    if (slot < 0) return;

    int side = rand() % 4;
    int x, y;
    const int margin = 12;
    switch (side) {
        case 0:  // top
            x = 20 + rand() % (ST7306_WIDTH - 40);
            y = TD_PLAYFIELD_TOP + margin;
            break;
        case 1:  // right
            x = ST7306_WIDTH - margin;
            y = TD_PLAYFIELD_TOP + 20 + rand() % (TD_PLAYFIELD_BOTTOM - TD_PLAYFIELD_TOP - 40);
            break;
        case 2:  // bottom
            x = 20 + rand() % (ST7306_WIDTH - 40);
            y = TD_PLAYFIELD_BOTTOM - margin;
            break;
        default:  // left
            x = margin;
            y = TD_PLAYFIELD_TOP + 20 + rand() % (TD_PLAYFIELD_BOTTOM - TD_PLAYFIELD_TOP - 40);
            break;
    }

    // Pick type based on wave
    int type = TD_BASIC;
    int r = rand() % 100;
    if (s_wave >= 7 && r < 20) type = TD_TANKY;
    else if (s_wave >= 4 && r < 40) type = TD_FAST;

    int speed = (type == TD_FAST) ? 2 : 1;
    int hp    = (type == TD_TANKY) ? 3 : 1;

    // Velocity = direction-to-center × speed
    int dx = TD_CENTER_X - x;
    int dy = TD_CENTER_Y - y;
    float dist = sqrtf((float)(dx*dx + dy*dy));
    if (dist < 1.0f) dist = 1.0f;
    int8_t vx = (int8_t)((float)dx * speed / dist);
    int8_t vy = (int8_t)((float)dy * speed / dist);
    // Guard against pure-cardinal spawns zeroing out under int8 rounding
    if (vx == 0 && vy == 0) {
        vx = (dx > 0) ? speed : (dx < 0) ? -speed : 0;
        vy = (dy > 0) ? speed : (dy < 0) ? -speed : 0;
    }

    td_enemy_t *e = &s_enemies[slot];
    e->x = (int16_t)x; e->y = (int16_t)y;
    e->vx = vx;        e->vy = vy;
    e->hp = hp;
    e->type = (uint8_t)type;
    e->active = true;
}

// ── Step ────────────────────────────────────────────────────────────────────
static void step_game(void) {
    if (s_fire_cooldown > 0) s_fire_cooldown--;

    // Wave management
    if (s_wave_intermission) {
        if (s_ticks_to_next_wave > 0) {
            s_ticks_to_next_wave--;
            return;  // freeze spawns/movement during prep
        }
        s_wave_intermission = false;
        s_wave++;
        s_wave_total = 4 + s_wave * 2;
        s_wave_spawned = 0;
        s_ticks_to_spawn = 0;
    } else if (s_wave_total == 0) {
        // First-wave init
        s_wave_total = 4 + s_wave * 2;
        s_ticks_to_spawn = 30;  // 1.5s grace
    }

    if (s_wave_intermission) return;  // safety

    // Spawn enemies
    if (s_wave_spawned < s_wave_total) {
        if (s_ticks_to_spawn > 0) {
            s_ticks_to_spawn--;
        } else {
            spawn_enemy();
            s_wave_spawned++;
            int interval = 40 - s_wave * 4;
            if (interval < 16) interval = 16;
            s_ticks_to_spawn = interval;
        }
    } else if (count_active_enemies() == 0) {
        s_wave_intermission = true;
        s_ticks_to_next_wave = TD_WAVE_PREP_TICKS;
    }

    // Move enemies + breach check
    for (int i = 0; i < TD_MAX_ENEMIES; i++) {
        td_enemy_t *e = &s_enemies[i];
        if (!e->active) continue;
        e->x += e->vx;
        e->y += e->vy;
        int dx = e->x - TD_CENTER_X;
        int dy = e->y - TD_CENTER_Y;
        if (dx*dx + dy*dy < TD_BREACH_RADIUS_SQ) {
            e->active = false;
            s_lives--;
            if (s_lives <= 0) {
                s_lives = 0;
                s_game_over = true;
                return;
            }
        }
    }

    // Move bullets + collision
    for (int i = 0; i < TD_MAX_BULLETS; i++) {
        td_bullet_t *b = &s_bullets[i];
        if (!b->active) continue;
        b->x += b->dx;
        b->y += b->dy;
        b->life--;
        if (b->life == 0 ||
            b->x < 0 || b->x >= ST7306_WIDTH ||
            b->y < TD_PLAYFIELD_TOP || b->y >= TD_PLAYFIELD_BOTTOM) {
            b->active = false;
            continue;
        }
        for (int j = 0; j < TD_MAX_ENEMIES; j++) {
            td_enemy_t *e = &s_enemies[j];
            if (!e->active) continue;
            int dx = e->x - b->x;
            int dy = e->y - b->y;
            if (dx*dx + dy*dy < TD_HIT_RADIUS_SQ) {
                b->active = false;
                if (e->hp > 1) {
                    e->hp--;
                } else {
                    e->active = false;
                    s_score += (e->type == TD_TANKY) ? 50 :
                               (e->type == TD_FAST)  ? 20 : 10;
                }
                break;
            }
        }
    }
}

// ── Rendering ───────────────────────────────────────────────────────────────
static void draw_top_bar(void) {
    st7306_draw_filled_rect(0, 0, ST7306_WIDTH, TD_TOP_BAR_H, ST7306_COLOR_WHITE);
    st7306_draw_hline(0, ST7306_WIDTH - 1, TD_TOP_BAR_H, ST7306_COLOR_BLACK);

    // 分数 NNNN
    int x = 4;
    x = hzk16_draw_gb_text(x, 6, gb_fen_shu, ST7306_COLOR_BLACK);
    char buf[16];
    snprintf(buf, sizeof(buf), " %d", s_score);
    st7306_draw_text(x, 6, buf, ST7306_COLOR_BLACK);

    // Lives: filled squares (present) vs hollow (lost)
    x = 110;
    for (int i = 0; i < TD_LIVES_START; i++) {
        if (i < s_lives) {
            st7306_draw_filled_rect(x + i*10, 8, 8, 8, ST7306_COLOR_BLACK);
        } else {
            st7306_draw_rect(x + i*10, 8, 8, 8, ST7306_COLOR_BLACK);
        }
    }

    // 第 N 波
    x = 160;
    x = hzk16_draw_gb_text(x, 6, gb_di, ST7306_COLOR_BLACK);
    snprintf(buf, sizeof(buf), "%d ", s_wave);
    st7306_draw_text(x, 6, buf, ST7306_COLOR_BLACK);
    x += st7306_text_width(buf);
    hzk16_draw_gb_text(x, 6, gb_bo, ST7306_COLOR_BLACK);

    // Status bar (clock/battery/wifi) at right
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    xiaozhi_app_draw_status_bar(&ti, wifi_manager_get_rssi(), battery_get_level());
}

static void draw_bottom_bar(void) {
    int y = ST7306_HEIGHT - TD_BOTTOM_BAR_H;
    st7306_draw_filled_rect(0, y, ST7306_WIDTH, TD_BOTTOM_BAR_H, ST7306_COLOR_WHITE);
    st7306_draw_hline(0, ST7306_WIDTH - 1, y, ST7306_COLOR_BLACK);

    int xx = 4;
    st7306_draw_text(xx, y + 8, "PREV/NEXT:", ST7306_COLOR_BLACK);
    xx += 10 * 8;
    xx = hzk16_draw_gb_text(xx, y + 8, gb_miao_zhun, ST7306_COLOR_BLACK);
    xx += 8;
    st7306_draw_text(xx, y + 8, "OK:", ST7306_COLOR_BLACK);
    xx += 3 * 8;
    xx = hzk16_draw_gb_text(xx, y + 8, gb_kai_huo, ST7306_COLOR_BLACK);
    xx += 8;
    st7306_draw_text(xx, y + 8, "BACK:", ST7306_COLOR_BLACK);
    xx += 5 * 8;
    hzk16_draw_gb_text(xx, y + 8, gb_tui_chu, ST7306_COLOR_BLACK);
}

static void draw_playfield(void) {
    st7306_draw_filled_rect(0, TD_PLAYFIELD_TOP, ST7306_WIDTH,
                            TD_PLAYFIELD_BOTTOM - TD_PLAYFIELD_TOP, ST7306_COLOR_WHITE);

    // Enemies
    for (int i = 0; i < TD_MAX_ENEMIES; i++) {
        td_enemy_t *e = &s_enemies[i];
        if (!e->active) continue;
        if (e->type == TD_TANKY) {
            // 12×12 with white inset indicating armor + black core
            st7306_draw_filled_rect(e->x - 6, e->y - 6, 12, 12, ST7306_COLOR_BLACK);
            st7306_draw_filled_rect(e->x - 4, e->y - 4, 8, 8, ST7306_COLOR_WHITE);
            st7306_draw_filled_rect(e->x - 2, e->y - 2, 4, 4, ST7306_COLOR_BLACK);
        } else {
            // 8×8 solid (basic and fast look the same — speed differentiates)
            st7306_draw_filled_rect(e->x - 4, e->y - 4, 8, 8, ST7306_COLOR_BLACK);
        }
    }

    // Bullets (4×4)
    for (int i = 0; i < TD_MAX_BULLETS; i++) {
        td_bullet_t *b = &s_bullets[i];
        if (!b->active) continue;
        st7306_draw_filled_rect(b->x - 2, b->y - 2, 4, 4, ST7306_COLOR_BLACK);
    }

    // Turret base (12×12 with white center dot)
    st7306_draw_filled_rect(TD_CENTER_X - 6, TD_CENTER_Y - 6, 12, 12, ST7306_COLOR_BLACK);
    st7306_draw_filled_rect(TD_CENTER_X - 2, TD_CENTER_Y - 2, 4, 4, ST7306_COLOR_WHITE);
    // Barrel: 4×4 nub at offset dir*2
    int bx = TD_CENTER_X + dir16_x[s_turret_angle] * 2 - 2;
    int by = TD_CENTER_Y + dir16_y[s_turret_angle] * 2 - 2;
    st7306_draw_filled_rect(bx, by, 4, 4, ST7306_COLOR_BLACK);
}

static void draw_game_over_overlay(void) {
    int w = 240, h = 110;
    int x = (ST7306_WIDTH - w) / 2;
    int y = (ST7306_HEIGHT - h) / 2;
    st7306_draw_filled_rect(x, y, w, h, ST7306_COLOR_BLACK);
    st7306_draw_rect(x + 2, y + 2, w - 4, h - 4, ST7306_COLOR_WHITE);

    // 游戏结束
    int tw = hzk16_text_width(gb_you_xi_jieshu);
    hzk16_draw_gb_text((ST7306_WIDTH - tw) / 2, y + 14, gb_you_xi_jieshu, ST7306_COLOR_WHITE);

    char buf[24];
    snprintf(buf, sizeof(buf), "SCORE %d", s_score);
    int sw = st7306_text_width(buf);
    st7306_draw_text((ST7306_WIDTH - sw) / 2, y + 44, buf, ST7306_COLOR_WHITE);

    // OK: 重开
    const char *hint1 = "OK:";
    int h1_text = st7306_text_width(hint1);
    int h1_gb = hzk16_text_width(gb_chong_kai);
    int h1_total = h1_text + 4 + h1_gb;
    int hx1 = (ST7306_WIDTH - h1_total) / 2;
    st7306_draw_text(hx1, y + 70, hint1, ST7306_COLOR_WHITE);
    hzk16_draw_gb_text(hx1 + h1_text + 4, y + 70, gb_chong_kai, ST7306_COLOR_WHITE);

    // BACK: 返回菜单
    const char *hint2 = "BACK:";
    int h2_text = st7306_text_width(hint2);
    int h2_gb = hzk16_text_width(gb_fan_hui) + hzk16_text_width(gb_cai_dan);
    int h2_total = h2_text + 4 + h2_gb;
    int hx2 = (ST7306_WIDTH - h2_total) / 2;
    int xx = hx2;
    st7306_draw_text(xx, y + 90, hint2, ST7306_COLOR_WHITE);
    xx += h2_text + 4;
    xx = hzk16_draw_gb_text(xx, y + 90, gb_fan_hui, ST7306_COLOR_WHITE);
    hzk16_draw_gb_text(xx, y + 90, gb_cai_dan, ST7306_COLOR_WHITE);
}

static void render_game(void) {
    app_manager_display_lock();
    draw_playfield();
    draw_top_bar();
    draw_bottom_bar();
    if (s_game_over) draw_game_over_overlay();
    st7306_update_display();
    app_manager_display_unlock();
}

// ── Worker ──────────────────────────────────────────────────────────────────
static void tower_worker(void *arg) {
    ESP_LOGI(TAG, "Worker started");
    const TickType_t tick = pdMS_TO_TICKS(50);  // 20 Hz
    while (!s_stop_flag) {
        if (!s_game_over) step_game();
        render_game();
        vTaskDelay(tick);
    }
    ESP_LOGI(TAG, "Worker exiting");
    xSemaphoreGive(s_exit_sem);
    s_worker = NULL;
    vTaskDelete(NULL);
}

// ── Reset ───────────────────────────────────────────────────────────────────
static void reset_game(void) {
    memset(s_enemies, 0, sizeof(s_enemies));
    memset(s_bullets, 0, sizeof(s_bullets));
    s_turret_angle = 0;
    s_score = 0;
    s_lives = TD_LIVES_START;
    s_wave = 1;
    s_wave_total = 0;
    s_wave_spawned = 0;
    s_ticks_to_spawn = 30;  // 1.5s grace before first enemy
    s_ticks_to_next_wave = 0;
    s_fire_cooldown = 0;
    s_wave_intermission = false;
    s_game_over = false;
}

// ── Public API ──────────────────────────────────────────────────────────────
void tower_on_enter(void) {
    ESP_LOGI(TAG, "Entering Tower Defense");
    srand((unsigned)xTaskGetTickCount());

    s_stop_flag = false;
    if (!s_exit_sem) s_exit_sem = xSemaphoreCreateBinary();
    xSemaphoreTake(s_exit_sem, 0);

    reset_game();

    app_manager_display_lock();
    st7306_clear();
    draw_playfield();
    draw_top_bar();
    draw_bottom_bar();
    st7306_update_display();
    app_manager_display_unlock();

    if (xTaskCreate(tower_worker, "tower", 6144, NULL, 1, &s_worker) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create worker");
        s_worker = NULL;
    }
}

void tower_on_exit(void) {
    ESP_LOGI(TAG, "Exiting Tower Defense");
    s_stop_flag = true;
    if (s_worker) {
        if (xSemaphoreTake(s_exit_sem, pdMS_TO_TICKS(2000)) != pdPASS) {
            ESP_LOGE(TAG, "Worker did not exit in 2s, force killing");
            vTaskDelete(s_worker);
            s_worker = NULL;
        }
    }
}

void tower_on_key(key_event_t key) {
    // Game-over overrides everything
    if (s_game_over) {
        if (key == KEY_ENTER) {
            reset_game();
        } else if (key == KEY_BACK) {
            app_manager_switch(APP_ID_MENU);
        }
        return;
    }

    switch (key) {
        case KEY_PREV:
            s_turret_angle = (int8_t)(((int)s_turret_angle - 1 + 16) % 16);
            break;
        case KEY_NEXT:
            s_turret_angle = (int8_t)(((int)s_turret_angle + 1) % 16);
            break;
        case KEY_ENTER: {
            if (s_fire_cooldown > 0) break;
            int slot = find_free_bullet_slot();
            if (slot < 0) break;
            td_bullet_t *b = &s_bullets[slot];
            // Spawn bullet just past the turret barrel
            b->x = TD_CENTER_X + dir16_x[s_turret_angle] * 6;
            b->y = TD_CENTER_Y + dir16_y[s_turret_angle] * 6;
            b->dx = dir16_x[s_turret_angle];
            b->dy = dir16_y[s_turret_angle];
            b->life = TD_BULLET_LIFE;
            b->active = true;
            s_fire_cooldown = TD_FIRE_COOLDOWN;
            break;
        }
        case KEY_BACK:
            app_manager_switch(APP_ID_MENU);
            return;
        default:
            return;
    }
}
