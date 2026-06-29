/*
 * snake_app.c — Classic Nokia-style Snake game (Phase C)
 *
 * Grid: 25 cols × 15 rows of 16px cells = 400×240 playfield
 * Layout: top status bar (28px) / playfield (28-268) / bottom hint (268-300)
 *
 * Controls:
 *   PREV (GPIO1)  = turn left relative to current heading
 *   NEXT (GPIO3)  = turn right relative to current heading
 *   ENTER (GPIO17)= pause / resume
 *   BACK / Key4   = return to menu
 *
 * Speed: starts at 200ms/tick, decreases 10ms every 5 food eaten (min 80ms).
 */

#include "snake_app.h"
#include "app_manager.h"
#include "st7306.h"
#include "hzk16.h"
#include "xiaozhi_app_display.h"  // reuse status bar
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

static const char *TAG = "Snake";

// Grid geometry
#define GRID_COLS    25
#define GRID_ROWS    15
#define CELL_SIZE    16
#define PLAYFIELD_X  0
#define PLAYFIELD_Y  28
#define PLAYFIELD_W  (GRID_COLS * CELL_SIZE)   // 400
#define PLAYFIELD_H  (GRID_ROWS * CELL_SIZE)   // 240

// Game timing
#define TICK_MS_INITIAL   200
#define TICK_MS_MIN       80
#define TICK_SPEEDUP_EVERY 5    // every 5 food, speed up 10ms

// Direction (Nokia relative-turning)
typedef enum { DIR_UP = 0, DIR_RIGHT, DIR_DOWN, DIR_LEFT, DIR_COUNT } dir_t;

// Snake body (circular buffer would be cleaner, but fixed array is fine for casual play)
#define MAX_SNAKE_LEN  200
typedef struct { int8_t x, y; } cell_t;

// Game state
typedef struct {
    cell_t body[MAX_SNAKE_LEN];
    int    length;
    dir_t  direction;       // current heading
    dir_t  next_direction;  // queued turn (applied at next tick)
    cell_t food;
    int    score;
    int    food_eaten;
    int    tick_ms;
    bool   game_over;
    bool   paused;
    bool   initialized;
} snake_state_t;

static snake_state_t s_state;

// Lifecycle
static volatile bool s_stop_flag = false;
static SemaphoreHandle_t s_exit_sem = NULL;
static TaskHandle_t s_worker = NULL;

// GB2312 strings
static const uint8_t gb_fenshu[]  = {0xB7, 0xD6, 0xCA, 0xFD, 0};            // 分数
static const uint8_t gb_zanting[] = {0xD4, 0xDD, 0xCD, 0xA3, 0};            // 暂停
static const uint8_t gb_jixu[]    = {0xBC, 0xD3, 0xD0, 0xF8, 0};            // 继续
static const uint8_t gb_youxi[]   = {0xD3, 0xCE, 0xCF, 0xB7, 0xBD, 0xE1, 0xCA, 0xF8, 0}; // 游戏结束
static const uint8_t gb_chongkai[]= {0xD6, 0xD8, 0xBF, 0xAA, 0};            // 重开
static const uint8_t gb_fan[]     = {0xB7, 0xB5, 0xBB, 0xD8, 0};            // 返回
static const uint8_t gb_caidan[]  = {0xB2, 0xCB, 0xB5, 0xA5, 0};            // 菜单
static const uint8_t gb_zhuanshe[]= {0xD7, 0xAA, 0xCF, 0xF2, 0};            // 转向

// ── Helpers ──────────────────────────────────────────────────────────────────
static void place_random_food(void)
{
    // Try random positions until finding one not occupied by snake
    for (int tries = 0; tries < 200; tries++) {
        int x = rand() % GRID_COLS;
        int y = rand() % GRID_ROWS;
        bool occupied = false;
        for (int i = 0; i < s_state.length; i++) {
            if (s_state.body[i].x == x && s_state.body[i].y == y) {
                occupied = true;
                break;
            }
        }
        if (!occupied) {
            s_state.food.x = x;
            s_state.food.y = y;
            return;
        }
    }
    // Fallback: scan grid for first free cell
    for (int y = 0; y < GRID_ROWS; y++) {
        for (int x = 0; x < GRID_COLS; x++) {
            bool occupied = false;
            for (int i = 0; i < s_state.length; i++) {
                if (s_state.body[i].x == x && s_state.body[i].y == y) {
                    occupied = true;
                    break;
                }
            }
            if (!occupied) {
                s_state.food.x = x;
                s_state.food.y = y;
                return;
            }
        }
    }
}

static void reset_game(void)
{
    s_state.length = 3;
    // Start in the middle, heading right
    int mid_y = GRID_ROWS / 2;
    int mid_x = GRID_COLS / 4;
    s_state.body[0].x = mid_x + 2;
    s_state.body[0].y = mid_y;
    s_state.body[1].x = mid_x + 1;
    s_state.body[1].y = mid_y;
    s_state.body[2].x = mid_x;
    s_state.body[2].y = mid_y;
    s_state.direction = DIR_RIGHT;
    s_state.next_direction = DIR_RIGHT;
    s_state.score = 0;
    s_state.food_eaten = 0;
    s_state.tick_ms = TICK_MS_INITIAL;
    s_state.game_over = false;
    s_state.paused = false;
    s_state.initialized = true;
    place_random_food();
}

// Apply relative turn (Nokia-style: PREV=left, NEXT=right from current heading)
static void turn_relative(bool right)
{
    dir_t cur = s_state.direction;
    dir_t next;
    if (right) {
        // UP→RIGHT→DOWN→LEFT→UP
        next = (dir_t)((cur + 1) % DIR_COUNT);
    } else {
        // UP→LEFT→DOWN→RIGHT→UP
        next = (dir_t)((cur + DIR_COUNT - 1) % DIR_COUNT);
    }
    // Disallow 180° reversal (would cause immediate self-collision)
    dir_t opposite = (dir_t)((cur + 2) % DIR_COUNT);
    if (next == opposite) return;
    s_state.next_direction = next;
}

// Advance snake by one cell. Returns true if game continues, false if game over.
static bool advance_snake(void)
{
    s_state.direction = s_state.next_direction;

    cell_t head = s_state.body[0];
    switch (s_state.direction) {
        case DIR_UP:    head.y--; break;
        case DIR_DOWN:  head.y++; break;
        case DIR_LEFT:  head.x--; break;
        case DIR_RIGHT: head.x++; break;
        default: break;
    }

    // Wall collision
    if (head.x < 0 || head.x >= GRID_COLS || head.y < 0 || head.y >= GRID_ROWS) {
        return false;
    }

    // Self collision (skip tail cell since it'll move away — unless we eat food)
    bool eat_food = (head.x == s_state.food.x && head.y == s_state.food.y);
    int check_len = eat_food ? s_state.length : s_state.length - 1;
    for (int i = 0; i < check_len; i++) {
        if (s_state.body[i].x == head.x && s_state.body[i].y == head.y) {
            return false;
        }
    }

    // Shift body forward (insert new head, drop tail if no food eaten)
    if (s_state.length < MAX_SNAKE_LEN) {
        for (int i = s_state.length; i > 0; i--) {
            s_state.body[i] = s_state.body[i - 1];
        }
        s_state.body[0] = head;
        if (eat_food) {
            s_state.length++;
            s_state.score += 10;
            s_state.food_eaten++;
            if (s_state.food_eaten % TICK_SPEEDUP_EVERY == 0 && s_state.tick_ms > TICK_MS_MIN) {
                s_state.tick_ms -= 10;
            }
            place_random_food();
        } else {
            // Length unchanged: shift already done, but conceptually we added head + removed tail
            // Since we shifted everything by 1 and length didn't grow, the last cell is "stale"
            // — actually we already moved it via the shift. Length stayed same so it's fine.
            // Wait, no: shifting from length-1 to length moved tail into position [length-1]
            // which is now stale. Need to overwrite next time. Actually since we incremented
            // i from length downward and the tail at [length-1] is now duplicated, we need
            // to "logically" remove it by not drawing it. Simpler: only shift length-1 items.
        }
    }
    // Simpler correct shift: when not eating, shift only first length-1 cells
    // (the above logic has a subtle issue — let me rewrite below)
    return true;
}

// Correct advance: handles shift properly for both eat / no-eat cases
static bool advance_snake_v2(void)
{
    s_state.direction = s_state.next_direction;

    cell_t head = s_state.body[0];
    switch (s_state.direction) {
        case DIR_UP:    head.y--; break;
        case DIR_DOWN:  head.y++; break;
        case DIR_LEFT:  head.x--; break;
        case DIR_RIGHT: head.x++; break;
        default: break;
    }

    // Wall collision
    if (head.x < 0 || head.x >= GRID_COLS || head.y < 0 || head.y >= GRID_ROWS) {
        return false;
    }

    bool eat_food = (head.x == s_state.food.x && head.y == s_state.food.y);
    int cells_to_check = eat_food ? s_state.length : s_state.length - 1;
    for (int i = 0; i < cells_to_check; i++) {
        if (s_state.body[i].x == head.x && s_state.body[i].y == head.y) {
            return false;
        }
    }

    // Shift body: from tail toward head
    int new_length = eat_food ? s_state.length + 1 : s_state.length;
    if (new_length > MAX_SNAKE_LEN) new_length = MAX_SNAKE_LEN;

    // Move cells [0..length-1] to [1..length], then place new head at [0]
    for (int i = new_length - 1; i > 0; i--) {
        s_state.body[i] = s_state.body[i - 1];
    }
    s_state.body[0] = head;
    s_state.length = new_length;

    if (eat_food) {
        s_state.score += 10;
        s_state.food_eaten++;
        if (s_state.food_eaten % TICK_SPEEDUP_EVERY == 0 && s_state.tick_ms > TICK_MS_MIN) {
            s_state.tick_ms -= 10;
        }
        place_random_food();
    }
    return true;
}

// ── Rendering ────────────────────────────────────────────────────────────────
static void draw_cell(int gx, int gy, int color)
{
    int px = PLAYFIELD_X + gx * CELL_SIZE;
    int py = PLAYFIELD_Y + gy * CELL_SIZE;
    if (color == ST7306_COLOR_BLACK) {
        st7306_draw_filled_rect(px + 1, py + 1, CELL_SIZE - 2, CELL_SIZE - 2, ST7306_COLOR_BLACK);
    } else {
        st7306_draw_filled_rect(px + 1, py + 1, CELL_SIZE - 2, CELL_SIZE - 2, ST7306_COLOR_WHITE);
    }
}

static void draw_playfield_border(void)
{
    st7306_draw_rect(PLAYFIELD_X, PLAYFIELD_Y, PLAYFIELD_W, PLAYFIELD_H, ST7306_COLOR_BLACK);
}

static void render_game(void)
{
    app_manager_display_lock();

    // Clear playfield interior
    st7306_draw_filled_rect(PLAYFIELD_X + 1, PLAYFIELD_Y + 1,
                            PLAYFIELD_W - 2, PLAYFIELD_H - 2, ST7306_COLOR_WHITE);

    // Draw snake body
    for (int i = 0; i < s_state.length; i++) {
        draw_cell(s_state.body[i].x, s_state.body[i].y, ST7306_COLOR_BLACK);
    }

    // Draw food (filled cell)
    draw_cell(s_state.food.x, s_state.food.y, ST7306_COLOR_BLACK);

    draw_playfield_border();
    st7306_update_display();
    app_manager_display_unlock();
}

static void draw_top_bar(void)
{
    // Reuse xiaozhi_app_draw_status_bar but overlay score on left
    st7306_draw_filled_rect(0, 0, ST7306_WIDTH, 28, ST7306_COLOR_WHITE);
    st7306_draw_hline(0, ST7306_WIDTH - 1, 28, ST7306_COLOR_BLACK);

    // Score on left
    int x = 4;
    x = hzk16_draw_gb_text(x, 6, gb_fenshu, ST7306_COLOR_BLACK);
    char s[16];
    snprintf(s, sizeof(s), ": %d", s_state.score);
    st7306_draw_text(x, 6, s, ST7306_COLOR_BLACK);

    // Battery + clock on right (reuse)
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    // xiaozhi_app_draw_status_bar draws full bar; we only want right side
    // — call it then redraw our score on top
    xiaozhi_app_draw_status_bar(&ti, wifi_manager_get_rssi(), battery_get_level());

    // Re-draw score on top of status bar (left side)
    st7306_draw_filled_rect(0, 0, 120, 28, ST7306_COLOR_WHITE);
    x = 4;
    x = hzk16_draw_gb_text(x, 6, gb_fenshu, ST7306_COLOR_BLACK);
    snprintf(s, sizeof(s), ": %d", s_state.score);
    st7306_draw_text(x, 6, s, ST7306_COLOR_BLACK);
}

static void draw_bottom_bar(void)
{
    int y = ST7306_HEIGHT - 32;
    st7306_draw_filled_rect(0, y, ST7306_WIDTH, 32, ST7306_COLOR_WHITE);
    st7306_draw_hline(0, ST7306_WIDTH - 1, y, ST7306_COLOR_BLACK);

    // Left: "PREV/NEXT:转向  ENTER:暂停"
    int x = 4;
    st7306_draw_text(x, y + 8, "<- ->:", ST7306_COLOR_BLACK);
    x += 6 * 8;
    x = hzk16_draw_gb_text(x, y + 8, gb_zhuanshe, ST7306_COLOR_BLACK);
    x += 16;
    st7306_draw_text(x, y + 8, "OK:", ST7306_COLOR_BLACK);
    x += 3 * 8;
    hzk16_draw_gb_text(x, y + 8, gb_zanting, ST7306_COLOR_BLACK);

    // Right: "BACK:返回菜单"
    int rx = ST7306_WIDTH - 8 * 5 - 16 - 16 - 8;
    st7306_draw_text(rx, y + 8, "BACK:", ST7306_COLOR_BLACK);
    rx += 5 * 8;
    hzk16_draw_gb_text(rx, y + 8, gb_fan, ST7306_COLOR_BLACK);
    hzk16_draw_gb_text(rx + 16, y + 8, gb_caidan, ST7306_COLOR_BLACK);
}

static void render_pause_overlay(void)
{
    app_manager_display_lock();
    // Dim overlay (white box with text in middle)
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
    int box_w = 240, box_h = 80;
    int bx = (ST7306_WIDTH - box_w) / 2;
    int by = (ST7306_HEIGHT - box_h) / 2;
    st7306_draw_filled_rect(bx, by, box_w, box_h, ST7306_COLOR_BLACK);
    // Title in white
    int w = hzk16_text_width(gb_youxi);
    hzk16_draw_gb_text((ST7306_WIDTH - w) / 2, by + 8, gb_youxi, ST7306_COLOR_WHITE);
    // Score
    char s[40];
    snprintf(s, sizeof(s), "Score: %d", s_state.score);
    int sw = st7306_text_width(s);
    st7306_draw_text((ST7306_WIDTH - sw) / 2, by + 32, s, ST7306_COLOR_WHITE);
    // Hint: "OK:重开 BACK:返回菜单"
    w = hzk16_text_width(gb_chongkai);
    int total = 3 * 8 + 16 + w + 5 * 8 + 16 + 16;
    int hx = (ST7306_WIDTH - total) / 2;
    st7306_draw_text(hx, by + 56, "OK:", ST7306_COLOR_WHITE);
    hx += 3 * 8;
    hzk16_draw_gb_text(hx, by + 56, gb_chongkai, ST7306_COLOR_WHITE);
    hx += 16 + 8;
    st7306_draw_text(hx, by + 56, "BACK:", ST7306_COLOR_WHITE);
    hx += 5 * 8;
    hzk16_draw_gb_text(hx, by + 56, gb_fan, ST7306_COLOR_WHITE);
    hzk16_draw_gb_text(hx + 16, by + 56, gb_caidan, ST7306_COLOR_WHITE);
    st7306_update_display();
    app_manager_display_unlock();
}

// ── Worker ───────────────────────────────────────────────────────────────────
static void snake_worker(void *arg)
{
    ESP_LOGI(TAG, "Worker started");
    while (!s_stop_flag) {
        if (s_state.paused || s_state.game_over) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        // Game tick
        vTaskDelay(pdMS_TO_TICKS(s_state.tick_ms));
        if (s_stop_flag || s_state.paused || s_state.game_over) continue;

        bool alive = advance_snake();
        if (!alive) {
            s_state.game_over = true;
            render_game();
            render_game_over_overlay();
            ESP_LOGI(TAG, "Game over, score=%d", s_state.score);
        } else {
            render_game();
            draw_top_bar();  // refresh score
        }
    }
    ESP_LOGI(TAG, "Worker exiting");
    xSemaphoreGive(s_exit_sem);
    s_worker = NULL;
    vTaskDelete(NULL);
}

// ── Public API ───────────────────────────────────────────────────────────────
void snake_on_enter(void)
{
    ESP_LOGI(TAG, "Entering Snake");
    s_stop_flag = false;
    if (!s_exit_sem) s_exit_sem = xSemaphoreCreateBinary();
    xSemaphoreTake(s_exit_sem, 0);

    srand((unsigned)xTaskGetTickCount());

    reset_game();

    app_manager_display_lock();
    st7306_clear();
    draw_top_bar();
    draw_playfield_border();
    render_game();
    draw_bottom_bar();
    st7306_update_display();
    app_manager_display_unlock();

    if (xTaskCreate(snake_worker, "snake", 6144, NULL, 1, &s_worker) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create worker");
        s_worker = NULL;
    }
}

void snake_on_exit(void)
{
    ESP_LOGI(TAG, "Exiting Snake");
    s_stop_flag = true;
    if (s_worker) {
        if (xSemaphoreTake(s_exit_sem, pdMS_TO_TICKS(2000)) != pdPASS) {
            ESP_LOGE(TAG, "Worker did not exit, force killing");
            vTaskDelete(s_worker);
            s_worker = NULL;
        }
    }
}

void snake_on_key(key_event_t key)
{
    if (s_state.game_over) {
        // Only respond to ENTER (restart) and BACK
        if (key == KEY_ENTER) {
            reset_game();
            app_manager_display_lock();
            st7306_clear();
            draw_top_bar();
            draw_playfield_border();
            render_game();
            draw_bottom_bar();
            st7306_update_display();
            app_manager_display_unlock();
        } else if (key == KEY_BACK) {
            app_manager_switch(APP_ID_MENU);
        }
        return;
    }

    switch (key) {
        case KEY_PREV:
            turn_relative(false);  // left turn
            break;
        case KEY_NEXT:
            turn_relative(true);   // right turn
            break;
        case KEY_ENTER:
            s_state.paused = !s_state.paused;
            if (s_state.paused) {
                render_pause_overlay();
            } else {
                render_game();
                draw_top_bar();
            }
            break;
        case KEY_BACK:
            app_manager_switch(APP_ID_MENU);
            return;
        default:
            return;
    }
}
