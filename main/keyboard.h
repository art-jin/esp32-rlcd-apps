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
    KEY_LONG_START,  // GPIO18 long press held >= 500ms
    KEY_LONG_END,    // GPIO18 released after long press
} key_event_t;

void keyboard_init(void);
key_event_t keyboard_poll(void);
key_event_t keyboard_wait(TickType_t timeout_ticks);

#endif // KEYBOARD_H
