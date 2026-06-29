#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

/**
 * Initialize WiFi: check NVS for credentials.
 * If found → STA mode (connect to saved AP).
 * If not found → AP mode + HTTP config server.
 *
 * Returns ESP_OK if STA connected successfully.
 * Returns ESP_ERR_NOT_FOUND if entering AP config mode.
 */
esp_err_t wifi_manager_init(void);

/**
 * After STA connection, sync time via SNTP.
 * Returns true if time was successfully obtained.
 */
bool wifi_manager_get_time(struct tm *timeinfo);

/**
 * Get the AP SSID used in config mode (valid only in AP mode).
 */
const char *wifi_manager_get_ap_ssid(void);

/**
 * Get current WiFi RSSI (signal strength) in dBm.
 * Returns -100 if not connected.
 */
int wifi_manager_get_rssi(void);

#endif // WIFI_MANAGER_H
