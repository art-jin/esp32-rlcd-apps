#ifndef WEATHER_H
#define WEATHER_H

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    float temperature;           /* degrees Celsius */
    int humidity;                /* percent 0-100 */
    int weather_code;            /* WMO weather code */
    const uint8_t *description;  /* GB2312 encoded description string */
    const uint8_t *city;         /* GB2312 encoded city label, NULL = default (北京) */
} weather_data_t;

/**
 * Fetch current weather from Open-Meteo API.
 * Coordinates hardcoded to Beijing (39.9, 116.4).
 * Returns ESP_OK on success, data filled via pointer.
 */
esp_err_t weather_fetch(weather_data_t *data);

#endif // WEATHER_H
