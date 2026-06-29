#ifndef BUTTON_HANDLER_H
#define BUTTON_HANDLER_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_err.h"

#define BTN_PRESS_BIT  (1 << 0)

/**
 * Initialize BOOT button (GPIO 0) with falling-edge ISR.
 * Presses set BTN_PRESS_BIT on the given event group.
 */
esp_err_t xz_button_init(gpio_num_t pin, EventGroupHandle_t events);

#endif // BUTTON_HANDLER_H
