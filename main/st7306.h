#ifndef ST7306_H
#define ST7306_H

#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Display dimensions (landscape orientation)
#define ST7306_WIDTH  400
#define ST7306_HEIGHT 300
#define ST7306_BUF_SIZE 15000

// GPIO pins (Waveshare ESP32-S3-RLCD-4.2)
#define ST7306_CLK_PIN  GPIO_NUM_11
#define ST7306_MOSI_PIN GPIO_NUM_12
#define ST7306_CS_PIN   GPIO_NUM_40
#define ST7306_DC_PIN   GPIO_NUM_5
#define ST7306_RST_PIN  GPIO_NUM_41

// Colors
#define ST7306_COLOR_WHITE 0
#define ST7306_COLOR_BLACK 1

/**
 * Initialize SPI bus, panel IO, and ST7306 LCD controller.
 * Must be called once before any other st7306_* functions.
 */
esp_err_t st7306_init(void);

/**
 * Set a single pixel in the frame buffer (landscape coordinates).
 * x: [0, ST7306_WIDTH-1], y: [0, ST7306_HEIGHT-1]
 */
void st7306_set_pixel(int x, int y, int color);

/**
 * Clear the entire frame buffer to white.
 */
void st7306_clear(void);

/**
 * Push the frame buffer to the display via SPI.
 */
void st7306_update_display(void);

/**
 * Draw a UTF-8 text string at the given landscape position.
 * ASCII characters are 8px wide, Chinese characters are 16px wide.
 * All characters are 16px tall.
 */
void st7306_draw_text(int x, int y, const char *text, int color);

/**
 * Calculate the pixel width of a UTF-8 text string.
 */
int st7306_text_width(const char *text);

/**
 * Draw a single ASCII character scaled to 7×14 pixels.
 */
void st7306_draw_char_7x14(int x, int y, unsigned char c, int color);

/**
 * Draw a single digit (0-9) using custom 10×20 bitmap font.
 */
void st7306_draw_digit_10x20(int x, int y, char c, int color);

/**
 * Draw a single ASCII character scaled from 8×16 to arbitrary size.
 * Uses nearest-neighbor scaling from the built-in font.
 */
void st7306_draw_char_scaled(int x, int y, char c, int tw, int th, int color);

/**
 * Get the frame buffer mutex for coordinating display updates.
 */
SemaphoreHandle_t st7306_get_mutex(void);

/**
 * Draw a horizontal line from x1 to x2 at row y.
 */
void st7306_draw_hline(int x1, int x2, int y, int color);

/**
 * Draw a vertical line from y1 to y2 at column x.
 */
void st7306_draw_vline(int x, int y1, int y2, int color);

/**
 * Draw a rectangle outline.
 */
void st7306_draw_rect(int x, int y, int w, int h, int color);

/**
 * Draw a filled rectangle.
 */
void st7306_draw_filled_rect(int x, int y, int w, int h, int color);

#endif // ST7306_H
