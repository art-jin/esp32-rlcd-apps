/*
 * UTF-8 to GB2312 conversion using static lookup table.
 * Uses binary search on a pre-generated table (utf8_gb2312_table.h).
 */

#include "utf8_gb2312.h"
#include "utf8_gb2312_table.h"
#include <string.h>

/**
 * Look up a Unicode code point in the GB2312 table via binary search.
 * Returns 1 if found (writes gb_high, gb_low), 0 if not found.
 */
static int lookup_gb2312(uint16_t cp, uint8_t *gb_high, uint8_t *gb_low)
{
    int lo = 0, hi = UNICODE_GB2312_TABLE_SIZE - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (unicode_gb2312_table[mid].unicode == cp) {
            *gb_high = unicode_gb2312_table[mid].gb_high;
            *gb_low  = unicode_gb2312_table[mid].gb_low;
            return 1;
        } else if (unicode_gb2312_table[mid].unicode < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return 0;
}

/**
 * Decode one UTF-8 character, return its Unicode code point.
 * Advances *idx past the character bytes.
 * Returns 0 on invalid sequence.
 */
static uint32_t utf8_decode(const char *str, int *idx)
{
    unsigned char c = (unsigned char)str[*idx];
    if (c == 0) return 0;

    if (c < 0x80) {
        // 1-byte: 0xxxxxxx
        (*idx)++;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        // 2-byte: 110xxxxx 10xxxxxx
        if ((unsigned char)str[*idx + 1] == 0) return 0;
        uint32_t cp = ((c & 0x1F) << 6) | ((unsigned char)str[*idx + 1] & 0x3F);
        *idx += 2;
        return cp;
    } else if ((c & 0xF0) == 0xE0) {
        // 3-byte: 1110xxxx 10xxxxxx 10xxxxxx
        if ((unsigned char)str[*idx + 1] == 0 || (unsigned char)str[*idx + 2] == 0) return 0;
        uint32_t cp = ((c & 0x0F) << 12)
                    | ((unsigned char)str[*idx + 1] & 0x3F) << 6
                    | ((unsigned char)str[*idx + 2] & 0x3F);
        *idx += 3;
        return cp;
    } else if ((c & 0xF8) == 0xF0) {
        // 4-byte: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
        if ((unsigned char)str[*idx + 1] == 0 || (unsigned char)str[*idx + 2] == 0
            || (unsigned char)str[*idx + 3] == 0) return 0;
        uint32_t cp = ((c & 0x07) << 18)
                    | ((unsigned char)str[*idx + 1] & 0x3F) << 12
                    | ((unsigned char)str[*idx + 2] & 0x3F) << 6
                    | ((unsigned char)str[*idx + 3] & 0x3F);
        *idx += 4;
        return cp;
    }

    // Invalid lead byte
    (*idx)++;
    return 0;
}

int utf8_to_gb2312(const char *utf8, uint8_t *gb_out, int gb_max)
{
    if (!utf8 || !gb_out || gb_max < 1) return -1;

    int out_pos = 0;
    int i = 0;

    while (utf8[i] != 0) {
        int start = i;
        uint32_t cp = utf8_decode(utf8, &i);
        if (cp == 0) break;

        if (cp < 0x80) {
            // ASCII — pass through
            if (out_pos + 1 > gb_max) break;
            gb_out[out_pos++] = (uint8_t)cp;
        } else {
            // Non-ASCII — look up in GB2312 table
            uint8_t gb_hi, gb_lo;
            if (lookup_gb2312((uint16_t)cp, &gb_hi, &gb_lo)) {
                if (out_pos + 2 > gb_max) break;
                gb_out[out_pos++] = gb_hi;
                gb_out[out_pos++] = gb_lo;
            } else {
                // Character not in GB2312 — use '?'
                if (out_pos + 1 > gb_max) break;
                gb_out[out_pos++] = '?';
            }
        }
    }

    gb_out[out_pos] = 0;  // null terminate
    return out_pos;
}
