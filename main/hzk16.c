/*
 * HZK16 Chinese font rendering module for ESP32-S3-RLCD-4.2
 * Reads HZK16 font file from SPIFFS partition, renders GB2312 characters.
 */

#include "hzk16.h"
#include "st7306.h"
#include <string.h>
#include <stdio.h>
#include "esp_spiffs.h"
#include "esp_log.h"

static const char *TAG = "hzk16";
static FILE *s_hzk16_file = NULL;

esp_err_t hzk16_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 2,
        .format_if_mount_failed = false,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: total=%d, used=%d", total, used);
    }

    s_hzk16_file = fopen("/spiffs/HZK16", "rb");
    if (!s_hzk16_file) {
        ESP_LOGE(TAG, "Failed to open /spiffs/HZK16");
        esp_vfs_spiffs_unregister(NULL);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "HZK16 font file opened successfully");
    return ESP_OK;
}

static int read_bitmap_16x16(const uint8_t *gb_bytes, uint8_t *bitmap)
{
    if (!s_hzk16_file) return -1;

    uint8_t b0 = gb_bytes[0], b1 = gb_bytes[1];
    if (b0 < 0xA1 || b1 < 0xA1) return -1;

    int qu = b0 - 0xA0;
    int wei = b1 - 0xA0;
    long offset = ((long)(qu - 1) * 94 + (wei - 1)) * 32;

    if (fseek(s_hzk16_file, offset, SEEK_SET) != 0) return -1;
    if (fread(bitmap, 1, 32, s_hzk16_file) != 32) return -1;

    return 0;
}

void hzk16_draw_char(int x, int y, const uint8_t *gb_bytes, int color)
{
    uint8_t bitmap[32];
    if (read_bitmap_16x16(gb_bytes, bitmap) != 0) return;

    // HZK16 bitmap: 16 rows, each row 2 bytes (16 pixels), MSB first
    for (int row = 0; row < 16; row++) {
        uint16_t row_bits = ((uint16_t)bitmap[row * 2] << 8) | bitmap[row * 2 + 1];
        for (int col = 0; col < 16; col++) {
            if (row_bits & (1 << (15 - col))) {
                st7306_set_pixel(x + col, y + row, color);
            }
        }
    }
}

void hzk16_draw_char_12x12(int x, int y, const uint8_t *gb_bytes, int color)
{
    uint8_t bitmap[32];
    if (read_bitmap_16x16(gb_bytes, bitmap) != 0) return;

    // Scale 16x16 → 12x12 by sampling every 4/3 pixel
    // Use nearest-neighbor: for each output pixel (ox, oy), sample at (ox*16/12, oy*16/12)
    for (int oy = 0; oy < 12; oy++) {
        int src_row = oy * 16 / 12;  // 0-15
        if (src_row >= 16) src_row = 15;
        uint16_t row_bits = ((uint16_t)bitmap[src_row * 2] << 8) | bitmap[src_row * 2 + 1];
        for (int ox = 0; ox < 12; ox++) {
            int src_col = ox * 16 / 12;  // 0-15
            if (src_col >= 16) src_col = 15;
            if (row_bits & (1 << (15 - src_col))) {
                st7306_set_pixel(x + ox, y + oy, color);
            }
        }
    }
}

int hzk16_draw_gb_text(int x, int y, const uint8_t *gb_text, int color)
{
    int cur_x = x;
    int i = 0;
    while (gb_text[i] != 0) {
        uint8_t b = gb_text[i];
        if (b < 0x80) {
            // ASCII — use existing 8x16 font via st7306
            char s[2] = {(char)b, 0};
            st7306_draw_text(cur_x, y, s, color);
            cur_x += 8;
            i++;
        } else if (b >= 0xA1) {
            // GB2312 double-byte Chinese character
            hzk16_draw_char(cur_x, y, &gb_text[i], color);
            cur_x += 16;
            i += 2;
        } else {
            // Skip invalid bytes
            i++;
        }
    }
    return cur_x;
}

void hzk16_draw_char_14x14(int x, int y, const uint8_t *gb_bytes, int color)
{
    uint8_t bitmap[32];
    if (read_bitmap_16x16(gb_bytes, bitmap) != 0) return;

    // Scale 16x16 → 14x14 by nearest-neighbor sampling (87.5%, only drops 2 pixels)
    for (int oy = 0; oy < 14; oy++) {
        int src_row = oy * 16 / 14;
        if (src_row >= 16) src_row = 15;
        uint16_t row_bits = ((uint16_t)bitmap[src_row * 2] << 8) | bitmap[src_row * 2 + 1];
        for (int ox = 0; ox < 14; ox++) {
            int src_col = ox * 16 / 14;
            if (src_col >= 16) src_col = 15;
            if (row_bits & (1 << (15 - src_col))) {
                st7306_set_pixel(x + ox, y + oy, color);
            }
        }
    }
}

int hzk16_draw_gb_text_14(int x, int y, const uint8_t *gb_text, int color)
{
    int cur_x = x;
    int i = 0;
    while (gb_text[i] != 0) {
        uint8_t b = gb_text[i];
        if (b < 0x80) {
            // ASCII — render at 7×14
            st7306_draw_char_7x14(cur_x, y, b, color);
            cur_x += 7;
            i++;
        } else if (b >= 0xA1) {
            // GB2312 double-byte — 14×14 scaled
            hzk16_draw_char_14x14(cur_x, y, &gb_text[i], color);
            cur_x += 14;
            i += 2;
        } else {
            i++;
        }
    }
    return cur_x;
}

int hzk16_text_width_14(const uint8_t *gb_text)
{
    int w = 0;
    int i = 0;
    while (gb_text[i] != 0) {
        uint8_t b = gb_text[i];
        if (b < 0x80) {
            w += 7;
            i++;
        } else if (b >= 0xA1) {
            w += 14;
            i += 2;
        } else {
            i++;
        }
    }
    return w;
}

int hzk16_draw_gb_text_12(int x, int y, const uint8_t *gb_text, int color)
{
    int cur_x = x;
    int i = 0;
    while (gb_text[i] != 0) {
        uint8_t b = gb_text[i];
        if (b < 0x80) {
            // ASCII — render small (just use 6px width for now, still 8x16 font)
            // For P0 we'll use the existing 8x16 font but could add ASC12 later
            char s[2] = {(char)b, 0};
            st7306_draw_text(cur_x, y, s, color);
            cur_x += 8;
            i++;
        } else if (b >= 0xA1) {
            // GB2312 double-byte — 12x12 scaled
            hzk16_draw_char_12x12(cur_x, y, &gb_text[i], color);
            cur_x += 12;
            i += 2;
        } else {
            i++;
        }
    }
    return cur_x;
}

int hzk16_text_width(const uint8_t *gb_text)
{
    int w = 0;
    int i = 0;
    while (gb_text[i] != 0) {
        uint8_t b = gb_text[i];
        if (b < 0x80) {
            w += 8;
            i++;
        } else if (b >= 0xA1) {
            w += 16;
            i += 2;
        } else {
            i++;
        }
    }
    return w;
}

int hzk16_text_width_12(const uint8_t *gb_text)
{
    int w = 0;
    int i = 0;
    while (gb_text[i] != 0) {
        uint8_t b = gb_text[i];
        if (b < 0x80) {
            w += 8;
            i++;
        } else if (b >= 0xA1) {
            w += 12;
            i += 2;
        } else {
            i++;
        }
    }
    return w;
}

void hzk16_deinit(void)
{
    if (s_hzk16_file) {
        fclose(s_hzk16_file);
        s_hzk16_file = NULL;
    }
    esp_vfs_spiffs_unregister(NULL);
}
