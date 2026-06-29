/*
 * tetris_app.c — Classic Tetris (Phase D)
 *
 * Playfield: 10 cols × 20 rows of 12px cells (120×240), placed at x=30, y=28
 * Right panel: next piece preview + score + level (x=170 to 400)
 *
 * Controls:
 *   PREV (GPIO1)  = move left
 *   NEXT (GPIO3)  = move right
 *   ENTER (GPIO17)= rotate clockwise
 *   BACK short    = return to menu
 *   BACK long     = hard drop (slam piece to bottom)
 *
 * Gravity: starts at 500ms/tick, decreases 50ms per level (min 100ms).
 * Level up every 10 lines cleared.
 * Scoring: 1 line=100, 2=300, 3=500, 4=800 (× level).
 */

#include "tetris_app.h"
#include "app_manager.h"
#include "st7306.h"
#include "hzk16.h"
#include "xiaozhi_app_display.h"
#include "wifi_manager.h"
#include "battery.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "Tetris";

// Grid geometry
#define GRID_COLS    10
#define GRID_ROWS    20
#define CELL_SIZE    12
#define PLAYFIELD_X  30
#define PLAYFIELD_Y  28
#define PLAYFIELD_W  (GRID_COLS * CELL_SIZE)   // 120
#define PLAYFIELD_H  (GRID_ROWS * CELL_SIZE)   // 240

// Right panel
#define PANEL_X      170

// Game timing
#define GRAVITY_MS_INITIAL  500
#define GRAVITY_MS_MIN      100
#define LINES_PER_LEVEL     10

// Pieces
typedef enum {
    PIECE_I = 0, PIECE_O, PIECE_T, PIECE_S, PIECE_Z, PIECE_L, PIECE_J, PIECE_COUNT
} piece_type_t;

// Each piece has 4 rotation states. Cells are {x,y} offsets in a 4x4 bounding box.
typedef struct { int8_t x, y; } cell_t;

static const cell_t PIECE_SHAPES[PIECE_COUNT][4][4] = {
    // I piece
    {
        {{0,1},{1,1},{2,1},{3,1}},
        {{2,0},{2,1},{2,2},{2,3}},
        {{0,2},{1,2},{2,2},{3,2}},
        {{1,0},{1,1},{1,2},{1,3}},
    },
    // O piece (same in all rotations)
    {
        {{1,0},{2,0},{1,1},{2,1}},
        {{1,0},{2,0},{1,1},{2,1}},
        {{1,0},{2,0},{1,1},{2,1}},
        {{1,0},{2,0},{1,1},{2,1}},
    },
    // T piece
    {
        {{1,0},{0,1},{1,1},{2,1}},
        {{1,0},{1,1},{2,1},{1,2}},
        {{0,1},{1,1},{2,1},{1,2}},
        {{1,0},{0,1},{1,1},{1,2}},
    },
    // S piece
    {
        {{1,0},{2,0},{0,1},{1,1}},
        {{1,0},{1,1},{2,1},{2,2}},
        {{1,1},{2,1},{0,2},{1,2}},
        {{0,0},{0,1},{1,1},{1,2}},
    },
    // Z piece
    {
        {{0,0},{1,0},{1,1},{2,1}},
        {{2,0},{1,1},{2,1},{1,2}},
        {{0,1},{1,1},{1,2},{2,2}},
        {{1,0},{0,1},{1,1},{0,2}},
    },
    // L piece
    {
        {{2,0},{0,1},{1,1},{2,1}},
        {{1,0},{1,1},{1,2},{2,2}},
        {{0,1},{1,1},{2,1},{0,2}},
        {{0,0},{1,0},{1,1},{1,2}},
    },
    // J piece
    {
        {{0,0},{0,1},{1,1},{2,1}},
        {{1,0},{2,0},{1,1},{1,2}},
        {{0,1},{1,1},{2,1},{2,2}},
        {{1,0},{1,1},{0,2},{1,2}},
    },
};

// Game state
typedef struct {
    bool   grid[GRID_ROWS][GRID_COLS];  // true = filled
    piece_type_t current_type;
    int    current_rotation;
    int    current_x;        // top-left of bounding box in grid coords
    int    current_y;
    piece_type_t next_type;
    int    score;
    int    level;
    int    lines;
    int    gravity_ms;
    bool   game_over;
    bool   paused;
} tetris_state_t;

static tetris_state_t s_state;

// Lifecycle
static volatile bool s_stop_flag = false;
static SemaphoreHandle_t s_exit_sem = NULL;
static TaskHandle_t s_worker = NULL;
static uint64_t s_last_drop_tick = 0;  // for BACK long-press detection (unused now)

// GB2312 strings
static const uint8_t gb_fenshu[]  = {0xB7, 0xD6, 0xCA, 0xFD, 0};            // 分数
static const uint8_t gb_guan[]    = {0xB9, 0xD8, 0};                        // 关 (level)
static const uint8_t gb_xing[]    = {0xD0, 0xD0, 0};                        // 行 (lines)
static const uint8_t gb_xia[]     = {0xCF, 0xC2, 0xD2, 0xBB, 0};            // 下一
static const uint8_t gb_ge[]      = {0xB8, 0xF6, 0};                        // 个
static const uint8_t gb_zanting[] = {0xD4, 0xDD, 0xCD, 0xA3, 0};
static const uint8_t gb_jixu[]    = {0xBC, 0xD3, 0xD0, 0xF8, 0};
static const uint8_t gb_youxi[]   = {0xD3, 0xCE, 0xCF, 0xB7, 0xBD, 0xE1, 0xCA, 0xF8, 0};
static const uint8_t gb_chongkai[]= {0xD6, 0xD8, 0xBF, 0xAA, 0};
static const uint8_t gb_fan[]     = {0xB7, 0xB5, 0xBB, 0xD8, 0};
static const uint8_t gb_caidan[]  = {0xB2, 0xCB, 0xB5, 0xA5, 0};

// ── Helpers ──────────────────────────────────────────────────────────────────
static piece_type_t random_piece(void)
{
    return (piece_type_t)(rand() % PIECE_COUNT);
}

static const cell_t *current_cells(void)
{
    return PIECE_SHAPES[s_state.current_type][s_state.current_rotation];
}

// Check if piece at (px,py) with given rotation collides with grid/walls
static bool collides(piece_type_t type, int rotation, int px, int py)
{
    const cell_t *cells = PIECE_SHAPES[type][rotation];
    for (int i = 0; i < 4; i++) {
        int gx = px + cells[i].x;
        int gy = py + cells[i].y;
        if (gx < 0 || gx >= GRID_COLS) return true;
        if (gy >= GRID_ROWS) return true;
        if (gy >= 0 && s_state.grid[gy][gx]) return true;
    }
    return false;
}

// Lock current piece into the grid
static void lock_piece(void)
{
    const cell_t *cells = current_cells();
    for (int i = 0; i < 4; i++) {
        int gx = s_state.current_x + cells[i].x;
        int gy = s_state.current_y + cells[i].y;
        if (gy >= 0 && gy < GRID_ROWS && gx >= 0 && gx < GRID_COLS) {
            s_state.grid[gy][gx] = true;
        }
    }
}

// Clear full lines, return count cleared
static int clear_lines(void)
{
    int cleared = 0;
    for (int y = GRID_ROWS - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < GRID_COLS; x++) {
            if (!s_state.grid[y][x]) { full = false; break; }
        }
        if (full) {
            // Shift everything above down by 1
            for (int yy = y; yy > 0; yy--) {
                memcpy(s_state.grid[yy], s_state.grid[yy - 1], GRID_COLS);
            }
            memset(s_state.grid[0], 0, GRID_COLS);
            cleared++;
            // Stay at same y (since rows shifted down)
            y++;
        }
    }
    return cleared;
}

static void spawn_piece(void)
{
    s_state.current_type = s_state.next_type;
    s_state.next_type = random_piece();
    s_state.current_rotation = 0;
    s_state.current_x = (GRID_COLS - 4) / 2;  // center 4-wide bounding box
    s_state.current_y = -1;  // start partially above visible area

    if (collides(s_state.current_type, s_state.current_rotation,
                 s_state.current_x, s_state.current_y)) {
        s_state.game_over = true;
    }
}

static void reset_game(void)
{
    memset(s_state.grid, 0, sizeof(s_state.grid));
    s_state.next_type = random_piece();
    s_state.score = 0;
    s_state.level = 1;
    s_state.lines = 0;
    s_state.gravity_ms = GRAVITY_MS_INITIAL;
    s_state.game_over = false;
    s_state.paused = false;
    spawn_piece();
}

// Try to move piece by (dx,dy). Returns true if moved.
static bool try_move(int dx, int dy)
{
    int nx = s_state.current_x + dx;
    int ny = s_state.current_y + dy;
    if (!collides(s_state.current_type, s_state.current_rotation, nx, ny)) {
        s_state.current_x = nx;
        s_state.current_y = ny;
        return true;
    }
    return false;
}

// Try to rotate clockwise. Returns true if rotated.
static bool try_rotate(void)
{
    int new_rot = (s_state.current_rotation + 1) % 4;
    // Try basic rotation, then with wall kicks (simplified: ±1 horizontal)
    int kicks[4] = {0, -1, 1, -2};
    for (int i = 0; i < 4; i++) {
        if (!collides(s_state.current_type, new_rot,
                      s_state.current_x + kicks[i], s_state.current_y)) {
            s_state.current_rotation = new_rot;
            s_state.current_x += kicks[i];
            return true;
        }
    }
    return false;
}

// Hard drop: move piece to lowest valid position, then lock
static void hard_drop(void)
{
    while (try_move(0, 1)) {}
    // Lock + spawn handled by gravity tick or explicitly here
    lock_piece();
    int cleared = clear_lines();
    if (cleared > 0) {
        s_state.lines += cleared;
        s_state.score += cleared * 100 * (cleared == 4 ? 2 : 1) * s_state.level;
        int new_level = 1 + s_state.lines / LINES_PER_LEVEL;
        if (new_level != s_state.level) {
            s_state.level = new_level;
            s_state.gravity_ms = GRAVITY_MS_INITIAL - (s_state.level - 1) * 50;
            if (s_state.gravity_ms < GRAVITY_MS_MIN) s_state.gravity_ms = GRAVITY_MS_MIN;
        }
    }
    spawn_piece();
}

// Gravity step: move piece down by 1, lock if can't
static void gravity_step(void)
{
    if (s_state.paused || s_state.game_over) return;
    if (!try_move(0, 1)) {
        // Can't move down — lock
        lock_piece();
        int cleared = clear_lines();
        if (cleared > 0) {
            s_state.lines += cleared;
            s_state.score += cleared * 100 * (cleared == 4 ? 2 : 1) * s_state.level;
            int new_level = 1 + s_state.lines / LINES_PER_LEVEL;
            if (new_level != s_state.level) {
                s_state.level = new_level;
                s_state.gravity_ms = GRAVITY_MS_INITIAL - (s_state.level - 1) * 50;
                if (s_state.gravity_ms < GRAVITY_MS_MIN) s_state.gravity_ms = GRAVITY_MS_MIN;
            }
        }
        spawn_piece();
    }
}

// ── Rendering ────────────────────────────────────────────────────────────────
static void draw_cell(int gx, int gy, bool filled)
{
    if (gy < 0) return;  // above playfield (piece spawning)
    int px = PLAYFIELD_X + gx * CELL_SIZE;
    int py = PLAYFIELD_Y + gy * CELL_SIZE;
    if (filled) {
        st7306_draw_filled_rect(px, py, CELL_SIZE - 1, CELL_SIZE - 1, ST7306_COLOR_BLACK);
    } else {
        st7306_draw_filled_rect(px, py, CELL_SIZE - 1, CELL_SIZE - 1, ST7306_COLOR_WHITE);
    }
}

static void draw_playfield(void)
{
    app_manager_display_lock();
    // Clear playfield interior
    st7306_draw_filled_rect(PLAYFIELD_X, PLAYFIELD_Y, PLAYFIELD_W, PLAYFIELD_H, ST7306_COLOR_WHITE);

    // Draw locked grid cells
    for (int y = 0; y < GRID_ROWS; y++) {
        for (int x = 0; x < GRID_COLS; x++) {
            if (s_state.grid[y][x]) {
                draw_cell(x, y, true);
            }
        }
    }

    // Draw current piece
    const cell_t *cells = current_cells();
    for (int i = 0; i < 4; i++) {
        draw_cell(s_state.current_x + cells[i].x, s_state.current_y + cells[i].y, true);
    }

    // Border
    st7306_draw_rect(PLAYFIELD_X - 1, PLAYFIELD_Y - 1, PLAYFIELD_W + 2, PLAYFIELD_H + 2, ST7306_COLOR_BLACK);
    st7306_update_display();
    app_manager_display_unlock();
}

static void draw_top_bar(void)
{
    st7306_draw_filled_rect(0, 0, ST7306_WIDTH, 28, ST7306_COLOR_WHITE);
    st7306_draw_hline(0, ST7306_WIDTH - 1, 28, ST7306_COLOR_BLACK);
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    xiaozhi_app_draw_status_bar(&ti, wifi_manager_get_rssi(), battery_get_level());
}

static void draw_right_panel(void)
{
    int x = PANEL_X;
    int y = 36;

    app_manager_display_lock();
    st7306_draw_filled_rect(x, 28, ST7306_WIDTH - x, GRID_ROWS * CELL_SIZE, ST7306_COLOR_WHITE);

    // Next piece preview
    int w = hzk16_text_width(gb_xia);
    hzk16_draw_gb_text(x, y, gb_xia, ST7306_COLOR_BLACK);
    hzk16_draw_gb_text(x + w, y, gb_ge, ST7306_COLOR_BLACK);
    y += 24;

    // 4x4 preview box
    int box_x = x;
    int box_y = y;
    int box_size = 12;
    int box_w = 4 * box_size;
    st7306_draw_rect(box_x - 1, box_y - 1, box_w + 2, box_w + 2, ST7306_COLOR_BLACK);
    // Clear interior
    st7306_draw_filled_rect(box_x, box_y, box_w, box_w, ST7306_COLOR_WHITE);
    // Draw next piece
    const cell_t *cells = PIECE_SHAPES[s_state.next_type][0];
    for (int i = 0; i < 4; i++) {
        int px = box_x + cells[i].x * box_size;
        int py = box_y + cells[i].y * box_size;
        st7306_draw_filled_rect(px, py, box_size - 1, box_size - 1, ST7306_COLOR_BLACK);
    }
    y = box_y + box_w + 16;

    // Score
    hzk16_draw_gb_text(x, y, gb_fenshu, ST7306_COLOR_BLACK);
    y += 20;
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", s_state.score);
    st7306_draw_text(x, y, buf, ST7306_COLOR_BLACK);
    y += 24;

    // Level
    hzk16_draw_gb_text(x, y, gb_guan, ST7306_COLOR_BLACK);
    st7306_draw_text(x + 32, y, ":", ST7306_COLOR_BLACK);
    snprintf(buf, sizeof(buf), "%d", s_state.level);
    st7306_draw_text(x + 40, y, buf, ST7306_COLOR_BLACK);
    y += 24;

    // Lines
    hzk16_draw_gb_text(x, y, gb_xing, ST7306_COLOR_BLACK);
    st7306_draw_text(x + 32, y, ":", ST7306_COLOR_BLACK);
    snprintf(buf, sizeof(buf), "%d", s_state.lines);
    st7306_draw_text(x + 40, y, buf, ST7306_COLOR_BLACK);

    st7306_update_display();
    app_manager_display_unlock();
}

static void draw_bottom_bar(void)
{
    int y = ST7306_HEIGHT - 32;
    st7306_draw_filled_rect(0, y, ST7306_WIDTH, 32, ST7306_COLOR_WHITE);
    st7306_draw_hline(0, ST7306_WIDTH - 1, y, ST7306_COLOR_BLACK);

    int x = 4;
    st7306_draw_text(x, y + 8, "<-: ok:rot", ST7306_COLOR_BLACK);

    int rx = ST7306_WIDTH - 130;
    st7306_draw_text(rx, y + 8, "BACK long=drop", ST7306_COLOR_BLACK);
}

static void render_pause_overlay(void)
{
    app_manager_display_lock();
    int box_w = 160, box_h = 40;
    int bx = (ST7306_WIDTH - box_w) / 2;
    int by = (ST7306_HEIGHT - box_h) / 2;
    st7306_draw_filled_rect(bx, by, box_w, box_h, ST7306_COLOR_WHITE);
    st7306_draw_rect(bx, by, box_w, box_h, ST7306_COLOR_BLACK);
    int w = hzk16_text_width(gb_zanting);
    hzk16_draw_gb_text((ST7306_WIDTH - w) / 2, by + 8, gb_zanting, ST7306_COLOR_BLACK);
    w = hzk16_text_width(gb_jixu);
    hzk16_draw_gb_text((ST7306_WIDTH - w) / 2, by + 24, gb_jixu, ST7306_COLOR_BLACK);
    st7306_update_display();
    app_manager_display_unlock();
}

static void render_game_over_overlay(void)
{
    app_manager_display_lock();
    int box_w = 260, box_h = 100;
    int bx = (ST7306_WIDTH - box_w) / 2;
    int by = (ST7306_HEIGHT - box_h) / 2;
    st7306_draw_filled_rect(bx, by, box_w, box_h, ST7306_COLOR_BLACK);

    int w = hzk16_text_width(gb_youxi);
    hzk16_draw_gb_text((ST7306_WIDTH - w) / 2, by + 8, gb_youxi, ST7306_COLOR_WHITE);

    char s[40];
    snprintf(s, sizeof(s), "Score: %d  Lines: %d", s_state.score, s_state.lines);
    int sw = st7306_text_width(s);
    st7306_draw_text((ST7306_WIDTH - sw) / 2, by + 36, s, ST7306_COLOR_WHITE);

    w = hzk16_text_width(gb_chongkai);
    int total = 3 * 8 + 16 + w + 5 * 8 + 16 + 16;
    int hx = (ST7306_WIDTH - total) / 2;
    st7306_draw_text(hx, by + 68, "OK:", ST7306_COLOR_WHITE);
    hx += 3 * 8;
    hzk16_draw_gb_text(hx, by + 68, gb_chongkai, ST7306_COLOR_WHITE);
    hx += 16 + 8;
    st7306_draw_text(hx, by + 68, "BACK:", ST7306_COLOR_WHITE);
    hx += 5 * 8;
    hzk16_draw_gb_text(hx, by + 68, gb_fan, ST7306_COLOR_WHITE);
    hzk16_draw_gb_text(hx + 16, by + 68, gb_caidan, ST7306_COLOR_WHITE);

    st7306_update_display();
    app_manager_display_unlock();
}

static void render_full(void)
{
    draw_top_bar();
    draw_playfield();
    draw_right_panel();
    draw_bottom_bar();
}

// ── Worker ───────────────────────────────────────────────────────────────────
static void tetris_worker(void *arg)
{
    ESP_LOGI(TAG, "Worker started");
    while (!s_stop_flag) {
        vTaskDelay(pdMS_TO_TICKS(s_state.gravity_ms));
        if (s_stop_flag || s_state.paused || s_state.game_over) continue;
        gravity_step();
        render_full();
        if (s_state.game_over) {
            render_game_over_overlay();
            ESP_LOGI(TAG, "Game over: score=%d lines=%d", s_state.score, s_state.lines);
        }
    }
    ESP_LOGI(TAG, "Worker exiting");
    xSemaphoreGive(s_exit_sem);
    s_worker = NULL;
    vTaskDelete(NULL);
}

// ── Public API ───────────────────────────────────────────────────────────────
void tetris_on_enter(void)
{
    ESP_LOGI(TAG, "Entering Tetris");
    s_stop_flag = false;
    if (!s_exit_sem) s_exit_sem = xSemaphoreCreateBinary();
    xSemaphoreTake(s_exit_sem, 0);

    srand((unsigned)xTaskGetTickCount());
    reset_game();

    app_manager_display_lock();
    st7306_clear();
    render_full();
    st7306_update_display();
    app_manager_display_unlock();

    if (xTaskCreate(tetris_worker, "tetris", 6144, NULL, 1, &s_worker) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create worker");
        s_worker = NULL;
    }
}

void tetris_on_exit(void)
{
    ESP_LOGI(TAG, "Exiting Tetris");
    s_stop_flag = true;
    if (s_worker) {
        if (xSemaphoreTake(s_exit_sem, pdMS_TO_TICKS(2000)) != pdPASS) {
            ESP_LOGE(TAG, "Worker did not exit, force killing");
            vTaskDelete(s_worker);
            s_worker = NULL;
        }
    }
}

void tetris_on_key(key_event_t key)
{
    if (s_state.game_over) {
        if (key == KEY_ENTER) {
            reset_game();
            app_manager_display_lock();
            st7306_clear();
            render_full();
            st7306_update_display();
            app_manager_display_unlock();
        } else if (key == KEY_BACK) {
            app_manager_switch(APP_ID_MENU);
        }
        return;
    }

    switch (key) {
        case KEY_PREV:
            if (!s_state.paused) {
                try_move(-1, 0);
                draw_playfield();
            }
            break;
        case KEY_NEXT:
            if (!s_state.paused) {
                try_move(1, 0);
                draw_playfield();
            }
            break;
        case KEY_ENTER:
            if (!s_state.paused) {
                try_rotate();
                draw_playfield();
            }
            break;
        case KEY_LONG_START:
            // BACK long-press alias (Key4 long): hard drop
            // NOTE: per design, this should be KEY_BACK_LONG but we re-use KEY_LONG_START
            // since GPIO18 long-press is the only long event available
            if (!s_state.paused) {
                hard_drop();
                render_full();
            }
            break;
        case KEY_BACK:
            // Short Key4 / GPIO43: return to menu
            app_manager_switch(APP_ID_MENU);
            return;
        case KEY_LONG_END:
            // Release after long press — no action
            break;
        default:
            return;
    }
}
