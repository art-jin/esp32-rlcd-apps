#ifndef LUNAR_H
#define LUNAR_H

#include <stdint.h>

typedef struct {
    int year;       // 农历年
    int month;      // 农历月 (1-12)
    int day;        // 农历日 (1-30)
    int is_leap;    // 是否闰月 (0=否, 1=是)
    int solar_term; // 节气索引 (0-23, -1=无)
} lunar_date_t;

/**
 * Convert solar (Gregorian) date to lunar date.
 * Covers 1901-2050. Returns 0 on success, -1 on error.
 */
int solar_to_lunar(int year, int month, int day, int hour, lunar_date_t *out);

/**
 * Get GB2312-encoded lunar month name (e.g. "正月", "二月", ..., "腊月").
 * month: 1-12. Returns null-terminated GB2312 string.
 */
const uint8_t *lunar_month_name(int month);

/**
 * Get GB2312-encoded lunar day name (e.g. "初一", "初二", ..., "三十").
 * day: 1-30. Returns null-terminated GB2312 string.
 */
const uint8_t *lunar_day_name(int day);

/**
 * Get GB2312-encoded solar term name (e.g. "小寒", "大寒", ..., "冬至").
 * idx: 0-23. Returns null-terminated GB2312 string.
 */
const uint8_t *solar_term_name(int idx);

/**
 * Get GB2312-encoded weekday name (e.g. "日", "一", ..., "六").
 * weekday: 0=Sunday, 1=Monday, ..., 6=Saturday.
 */
const uint8_t *weekday_name(int weekday);

/**
 * Get GB2312 byte for "年" character.
 */
const uint8_t *gb_year_char(void);

/**
 * Get GB2312 byte for "月" character.
 */
const uint8_t *gb_month_char(void);

/**
 * Get GB2312 byte for "日" character.
 */
const uint8_t *gb_day_char(void);

/**
 * Get GB2312 bytes for "星期" string.
 */
const uint8_t *gb_week_prefix(void);

/**
 * Get GB2312 bytes for "农历" string.
 */
const uint8_t *gb_lunar_prefix(void);

/**
 * Get GB2312 byte for "闰" character.
 */
const uint8_t *gb_leap_char(void);

#endif // LUNAR_H
