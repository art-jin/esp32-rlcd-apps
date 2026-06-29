/*
 * Lunar calendar module for ESP32-S3-RLCD-4.2
 * Converts solar dates to Chinese lunar calendar dates.
 * Algorithm from vincascm/cdate (BSD licensed), covers 1901-2050.
 * All output strings are pre-encoded as GB2312 for HZK16 rendering.
 */

#include "lunar.h"
#include <string.h>

// ── 1900-2051 lunar calendar data ──────────────────────────────────────────────
// Format per entry (bits):
//   1-4:   leap month number (0 = no leap)
//   5-16:  big/small month flags for months 1-12
//   17:    leap month size (1=30, 0=29)
static const unsigned int calendar_data[] = {
    0x04bd8, 0x04ae0, 0x0a570, 0x054d5, 0x0d260, 0x0d950, 0x16554, 0x056a0, 0x09ad0, 0x055d2,
    0x04ae0, 0x0a5b6, 0x0a4d0, 0x0d250, 0x1d255, 0x0b540, 0x0d6a0, 0x0ada2, 0x095b0, 0x14977,
    0x04970, 0x0a4b0, 0x0b4b5, 0x06a50, 0x06d40, 0x1ab54, 0x02b60, 0x09570, 0x052f2, 0x04970,
    0x06566, 0x0d4a0, 0x0ea50, 0x06e95, 0x05ad0, 0x02b60, 0x186e3, 0x092e0, 0x1c8d7, 0x0c950,
    0x0d4a0, 0x1d8a6, 0x0b550, 0x056a0, 0x1a5b4, 0x025d0, 0x092d0, 0x0d2b2, 0x0a950, 0x0b557,
    0x06ca0, 0x0b550, 0x15355, 0x04da0, 0x0a5b0, 0x14573, 0x052b0, 0x0a9a8, 0x0e950, 0x06aa0,
    0x0aea6, 0x0ab50, 0x04b60, 0x0aae4, 0x0a570, 0x05260, 0x0f263, 0x0d950, 0x05b57, 0x056a0,
    0x096d0, 0x04dd5, 0x04ad0, 0x0a4d0, 0x0d4d4, 0x0d250, 0x0d558, 0x0b540, 0x0b6a0, 0x195a6,
    0x095b0, 0x049b0, 0x0a974, 0x0a4b0, 0x0b27a, 0x06a50, 0x06d40, 0x0af46, 0x0ab60, 0x09570,
    0x04af5, 0x04970, 0x064b0, 0x074a3, 0x0ea50, 0x06b58, 0x055c0, 0x0ab60, 0x096d5, 0x092e0,
    0x0c960, 0x0d954, 0x0d4a0, 0x0da50, 0x07552, 0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5,
    0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9, 0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930,
    0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, 0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530,
    0x05aa0, 0x076a3, 0x096d0, 0x04bd7, 0x04ad0, 0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45,
    0x0b5a0, 0x056d0, 0x055b2, 0x049b0, 0x0a577, 0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0,
    0x14b63,
};

// ── Solar term precomputed offsets (minutes from reference) ─────────────────────
static const int jieqi_data[] = {
       0, 21208, 42467, 63836, 85337, 107014, 128867, 150921, 173149,
    195551, 218072, 240693, 263343, 285989, 308563, 331033, 353350,
    375494, 397447, 419210, 440795, 462224, 483532, 504758,
};

// ── Internal helpers ───────────────────────────────────────────────────────────

#define NIAN_BIT(nian, bit) (calendar_data[nian - 1900] & bit)
#define WHICH_RUN_YUE(nian) (NIAN_BIT(nian, 0xF))

static int days_of_run_yue(int nian)
{
    if (WHICH_RUN_YUE(nian))
        return (NIAN_BIT(nian, 0x10000) > 0) ? 30 : 29;
    return 0;
}

static int days_of_nian(int nian)
{
    int sum = 348;
    for (int i = 0x8000; i > 8; i >>= 1)
        if (NIAN_BIT(nian, i) > 0) sum++;
    return sum + days_of_run_yue(nian);
}

static int is_bissextile(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int sum_to_premonth(int year, int month)
{
    int sum = (month - 1) ? (month - 1) * 30 : 0;
    for (int i = 1; i <= month - 1; i++) {
        switch (i) {
        case 1: case 3: case 5: case 7:
        case 8: case 10: case 12:
            sum++; break;
        case 2:
            sum -= 2;
            if (is_bissextile(year)) sum++;
            break;
        }
    }
    return sum;
}

static int sub_two_date(int year1, int month1, int day1,
                        int year2, int month2, int day2)
{
    int sum = sum_to_premonth(year1, month1) +
              365 - sum_to_premonth(year2, month2);
    if (is_bissextile(year2)) sum--;
    sum += day1 - day2;

    for (int i = year2 + 1; i < year1; i++)
        if (is_bissextile(i)) sum++;

    if (year1 > year2)
        sum += (year1 - year2 - 1) * 365;
    else if (year1 == year2)
        sum -= 365;
    else
        return -1;

    return sum;
}

static int which_day_is_jieqi(int year, int n)
{
    short days_of_permonth[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (is_bissextile(year)) days_of_permonth[2]++;

    int a, i = 0;
    a = 525948 * (year - 1900) -
        sub_two_date(year, 1, 6, 1900, 1, 6) * 24 * 60 +
        jieqi_data[n] + 125;

    if (a < 0) i--;
    a = i + 6 + a / (24 * 60);

    for (i = 0; (i < 13) && (a > 28); i++)
        a -= days_of_permonth[i];

    return a;
}

// ── Public API ─────────────────────────────────────────────────────────────────

int solar_to_lunar(int year, int month, int day, int hour, lunar_date_t *out)
{
    (void)hour; // not needed for basic conversion

    static const int percalc_val[] = {0, 18279, 36529};

    if (year < 1901 || year > 2050) return -1;

    int all_days = sub_two_date(year, month, day, 1900, 1, 31);

    // Optimized: use pre-calculated values per 50-year block
    unsigned int block;
    for (block = 0; block < 5; block++)
        if (year > (int)(block * 50 + 1900) && year <= (int)((block + 1) * 50 + 1900))
            break;

    all_days -= percalc_val[block];

    unsigned int i;
    unsigned int x = 0;
    int run_yue, is_run_yue = 0;

    for (i = block * 50 + 1900; (all_days > 0) && (i < 2050); i++) {
        x = days_of_nian(i);
        all_days -= x;
    }

    if (all_days < 0) {
        all_days += x;
        i--;
    }

    out->year = i;
    run_yue = WHICH_RUN_YUE(i);

    for (i = 1; i < 13 && all_days > 0; i++) {
        if ((run_yue > 0) && (i == (unsigned)(run_yue + 1)) && (is_run_yue == 0)) {
            --i;
            is_run_yue = 1;
            x = days_of_run_yue(out->year);
        } else {
            x = NIAN_BIT(out->year, 0x10000 >> i) ? 30 : 29;
        }
        all_days -= x;
        if (is_run_yue == 1 && i == (unsigned)(run_yue + 1)) is_run_yue = 0;
    }

    if (all_days < 0) {
        all_days += x;
        i--;
    }

    if ((all_days == 0) && (run_yue > 0) && (i == (unsigned)(run_yue + 1))) {
        if (is_run_yue == 1)
            is_run_yue = 0;
        else {
            is_run_yue = 1;
            --i;
        }
    }

    out->month = i;
    out->day = all_days + 1;
    out->is_leap = is_run_yue;

    // Check for solar term
    if (day == which_day_is_jieqi(year, (month - 1) * 2))
        out->solar_term = (month - 1) * 2;
    else if (day == which_day_is_jieqi(year, (month - 1) * 2 + 1))
        out->solar_term = (month - 1) * 2 + 1;
    else
        out->solar_term = -1;

    return 0;
}

// ── GB2312 pre-encoded string tables ───────────────────────────────────────────

// Month names: 正月 二月 三月 四月 五月 六月 七月 八月 九月 十月 十一月 腊月
static const uint8_t month_names[][7] = {
    {0xD5, 0xFD, 0xD4, 0xC2, 0},               // 正月
    {0xB6, 0xFE, 0xD4, 0xC2, 0},               // 二月
    {0xC8, 0xFD, 0xD4, 0xC2, 0},               // 三月
    {0xCB, 0xC4, 0xD4, 0xC2, 0},               // 四月
    {0xCE, 0xE5, 0xD4, 0xC2, 0},               // 五月
    {0xC1, 0xF9, 0xD4, 0xC2, 0},               // 六月
    {0xC6, 0xDF, 0xD4, 0xC2, 0},               // 七月
    {0xB0, 0xCB, 0xD4, 0xC2, 0},               // 八月
    {0xBE, 0xC5, 0xD4, 0xC2, 0},               // 九月
    {0xCA, 0xAE, 0xD4, 0xC2, 0},               // 十月
    {0xCA, 0xAE, 0xD2, 0xBB, 0xD4, 0xC2, 0},   // 十一月
    {0xC0, 0xB0, 0xD4, 0xC2, 0},               // 腊月
};

// Day names: 初一 to 三十
static const uint8_t day_names[][5] = {
    {0xB3, 0xF5, 0xD2, 0xBB, 0},  // 初一
    {0xB3, 0xF5, 0xB6, 0xFE, 0},  // 初二
    {0xB3, 0xF5, 0xC8, 0xFD, 0},  // 初三
    {0xB3, 0xF5, 0xCB, 0xC4, 0},  // 初四
    {0xB3, 0xF5, 0xCE, 0xE5, 0},  // 初五
    {0xB3, 0xF5, 0xC1, 0xF9, 0},  // 初六
    {0xB3, 0xF5, 0xC6, 0xDF, 0},  // 初七
    {0xB3, 0xF5, 0xB0, 0xCB, 0},  // 初八
    {0xB3, 0xF5, 0xBE, 0xC5, 0},  // 初九
    {0xB3, 0xF5, 0xCA, 0xAE, 0},  // 初十
    {0xCA, 0xAE, 0xD2, 0xBB, 0},  // 十一
    {0xCA, 0xAE, 0xB6, 0xFE, 0},  // 十二
    {0xCA, 0xAE, 0xC8, 0xFD, 0},  // 十三
    {0xCA, 0xAE, 0xCB, 0xC4, 0},  // 十四
    {0xCA, 0xAE, 0xCE, 0xE5, 0},  // 十五
    {0xCA, 0xAE, 0xC1, 0xF9, 0},  // 十六
    {0xCA, 0xAE, 0xC6, 0xDF, 0},  // 十七
    {0xCA, 0xAE, 0xB0, 0xCB, 0},  // 十八
    {0xCA, 0xAE, 0xBE, 0xC5, 0},  // 十九
    {0xB6, 0xFE, 0xCA, 0xAE, 0},  // 二十
    {0xD8, 0xA5, 0xD2, 0xBB, 0},  // 廿一
    {0xD8, 0xA5, 0xB6, 0xFE, 0},  // 廿二
    {0xD8, 0xA5, 0xC8, 0xFD, 0},  // 廿三
    {0xD8, 0xA5, 0xCB, 0xC4, 0},  // 廿四
    {0xD8, 0xA5, 0xCE, 0xE5, 0},  // 廿五
    {0xD8, 0xA5, 0xC1, 0xF9, 0},  // 廿六
    {0xD8, 0xA5, 0xC6, 0xDF, 0},  // 廿七
    {0xD8, 0xA5, 0xB0, 0xCB, 0},  // 廿八
    {0xD8, 0xA5, 0xBE, 0xC5, 0},  // 廿九
    {0xC8, 0xFD, 0xCA, 0xAE, 0},  // 三十
};

// Solar term names: 小寒 大寒 立春 ... 冬至
static const uint8_t solar_term_names[][5] = {
    {0xD0, 0xA1, 0xBA, 0xAE, 0},  // 小寒
    {0xB4, 0xF3, 0xBA, 0xAE, 0},  // 大寒
    {0xC1, 0xA2, 0xB4, 0xBA, 0},  // 立春
    {0xD3, 0xEA, 0xCB, 0xAE, 0},  // 雨水
    {0xBE, 0xAA, 0xD5, 0xDD, 0},  // 惊蛰
    {0xB4, 0xBA, 0xB7, 0xD6, 0},  // 春分
    {0xC7, 0xE5, 0xC3, 0xF7, 0},  // 清明
    {0xB9, 0xC8, 0xD3, 0xEA, 0},  // 谷雨
    {0xC1, 0xA2, 0xCF, 0xC4, 0},  // 立夏
    {0xD0, 0xA1, 0xC2, 0xFA, 0},  // 小满
    {0xC3, 0xA2, 0xD6, 0xD6, 0},  // 芒种
    {0xCF, 0xC4, 0xD6, 0xC1, 0},  // 夏至
    {0xD0, 0xA1, 0xCA, 0xEE, 0},  // 小暑
    {0xB4, 0xF3, 0xCA, 0xEE, 0},  // 大暑
    {0xC1, 0xA2, 0xC7, 0xEF, 0},  // 立秋
    {0xB4, 0xA6, 0xCA, 0xEE, 0},  // 处暑
    {0xB0, 0xD7, 0xC2, 0xB6, 0},  // 白露
    {0xC7, 0xEF, 0xB7, 0xD6, 0},  // 秋分
    {0xBA, 0xAE, 0xC2, 0xB6, 0},  // 寒露
    {0xCB, 0xAA, 0xBD, 0xB5, 0},  // 霜降
    {0xC1, 0xA2, 0xB6, 0xAC, 0},  // 立冬
    {0xD0, 0xA1, 0xD1, 0xA9, 0},  // 小雪
    {0xB4, 0xF3, 0xD1, 0xA9, 0},  // 大雪
    {0xB6, 0xAC, 0xD6, 0xC1, 0},  // 冬至
};

// Weekday names: 日 一 二 三 四 五 六
static const uint8_t weekday_names[][3] = {
    {0xC8, 0xD5, 0},  // 日
    {0xD2, 0xBB, 0},  // 一
    {0xB6, 0xFE, 0},  // 二
    {0xC8, 0xFD, 0},  // 三
    {0xCB, 0xC4, 0},  // 四
    {0xCE, 0xE5, 0},  // 五
    {0xC1, 0xF9, 0},  // 六
};

// UI strings
static const uint8_t gb_year[] = {0xC4, 0xEA, 0};        // 年
static const uint8_t gb_month[] = {0xD4, 0xC2, 0};       // 月
static const uint8_t gb_day[] = {0xC8, 0xD5, 0};         // 日
static const uint8_t gb_week[] = {0xD0, 0xC7, 0xC6, 0xDA, 0}; // 星期
static const uint8_t gb_nongli[] = {0xC5, 0xA9, 0xC0, 0xFA, 0}; // 农历
static const uint8_t gb_run[] = {0xC8, 0xF2, 0};          // 闰

const uint8_t *lunar_month_name(int month)
{
    if (month < 1 || month > 12) return (const uint8_t *)"";
    return month_names[month - 1];
}

const uint8_t *lunar_day_name(int day)
{
    if (day < 1 || day > 30) return (const uint8_t *)"";
    return day_names[day - 1];
}

const uint8_t *solar_term_name(int idx)
{
    if (idx < 0 || idx > 23) return (const uint8_t *)"";
    return solar_term_names[idx];
}

const uint8_t *weekday_name(int weekday)
{
    if (weekday < 0 || weekday > 6) return (const uint8_t *)"";
    return weekday_names[weekday];
}

const uint8_t *gb_year_char(void)    { return gb_year; }
const uint8_t *gb_month_char(void)   { return gb_month; }
const uint8_t *gb_day_char(void)     { return gb_day; }
const uint8_t *gb_week_prefix(void)  { return gb_week; }
const uint8_t *gb_lunar_prefix(void) { return gb_nongli; }
const uint8_t *gb_leap_char(void)    { return gb_run; }
