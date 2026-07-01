#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "freertos/FreeRTOS.h"

typedef enum {
    KEY_NONE = 0,
    KEY_PREV,        // GPIO1
    KEY_NEXT,        // GPIO3
    KEY_ENTER,       // GPIO17
    KEY_BACK,        // GPIO43
    KEY_USER,        // GPIO18 short press (< 500ms)
    KEY_DOUBLE_CLICK, // GPIO18 two short presses within 250ms (single-key hw only)
    KEY_LONG_START,  // GPIO18 long press held >= 500ms
    KEY_LONG_END,    // GPIO18 released after long press
} key_event_t;

void keyboard_init(void);
key_event_t keyboard_poll(void);
key_event_t keyboard_wait(TickType_t timeout_ticks);

/**
 * Whether the device has the 4th physical key (BACK on GPIO43).
 * Cached at keyboard_init() time from NVS. When false, KEY_BACK events
 * are never generated (GPIO43 is left unconfigured), and GPIO18 short-press
 * goes through double-click detection (KEY_USER on single click,
 * KEY_DOUBLE_CLICK on two quick presses) instead of being aliased to BACK.
 */
bool keyboard_has_back_key(void);

#endif // KEYBOARD_H
