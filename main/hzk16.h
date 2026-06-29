#ifndef HZK16_H
#define HZK16_H

#include <stdint.h>
#include "esp_err.h"

/**
 * Mount SPIFFS and open HZK16 font file.
 * Must be called once after st7306_init().
 */
esp_err_t hzk16_init(void);

/**
 * Draw a single 16x16 Chinese character at (x, y) using GB2312 encoding.
 * gb_bytes: 2-byte GB2312 code (both bytes >= 0xA1).
 */
void hzk16_draw_char(int x, int y, const uint8_t *gb_bytes, int color);

/**
 * Draw a single Chinese character scaled to 12x12 at (x, y).
 * Reads 16x16 from HZK16 and downscales by 3/4 sampling.
 */
void hzk16_draw_char_12x12(int x, int y, const uint8_t *gb_bytes, int color);

/**
 * Draw a single Chinese character scaled to 14x14 at (x, y).
 * Reads 16x16 from HZK16 and downscales by 7/8 sampling (87.5%).
 */
void hzk16_draw_char_14x14(int x, int y, const uint8_t *gb_bytes, int color);

/**
 * Draw a GB2312 encoded string. ASCII chars use 8x16 font,
 * Chinese chars (double-byte GB2312) use HZK16 16x16.
 * Returns the x position after the last character.
 */
int hzk16_draw_gb_text(int x, int y, const uint8_t *gb_text, int color);

/**
 * Draw a GB2312 string using 12x12 scaled Chinese chars.
 * Returns the x position after the last character.
 */
int hzk16_draw_gb_text_12(int x, int y, const uint8_t *gb_text, int color);

/**
 * Draw a GB2312 string using 14x14 scaled Chinese chars + 7x14 ASCII.
 * Returns the x position after the last character.
 */
int hzk16_draw_gb_text_14(int x, int y, const uint8_t *gb_text, int color);

/**
 * Calculate pixel width of a GB2312 string (ASCII=8px, Chinese=16px).
 */
int hzk16_text_width(const uint8_t *gb_text);

/**
 * Calculate pixel width with 12x12 Chinese (ASCII=6px, Chinese=12px).
 */
int hzk16_text_width_12(const uint8_t *gb_text);

/**
 * Calculate pixel width with 14x14 Chinese (ASCII=7px, Chinese=14px).
 */
int hzk16_text_width_14(const uint8_t *gb_text);

/**
 * Close HZK16 file and unmount SPIFFS.
 */
void hzk16_deinit(void);

#endif // HZK16_H
