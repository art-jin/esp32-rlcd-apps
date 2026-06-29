#ifndef UTF8_GB2312_H
#define UTF8_GB2312_H

#include <stdint.h>

/**
 * Convert a UTF-8 string to GB2312 encoding.
 * ASCII characters (0x00-0x7F) pass through unchanged.
 * Chinese characters are looked up in the Unicode->GB2312 table.
 * Characters not in GB2312 are replaced with '?'.
 *
 * @param utf8    Null-terminated UTF-8 input string
 * @param gb_out  Output buffer for GB2312 bytes
 * @param gb_max  Size of output buffer
 * @return Number of bytes written to gb_out (excluding null terminator), or -1 on error
 */
int utf8_to_gb2312(const char *utf8, uint8_t *gb_out, int gb_max);

#endif /* UTF8_GB2312_H */
