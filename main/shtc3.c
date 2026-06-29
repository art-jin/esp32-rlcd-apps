/*
 * SHTC3 (Sensirion SHTC3) temperature/humidity sensor driver.
 *
 * - I2C address 0x70 (same bus as ES8311/ES7210, GPIO13/14)
 * - Normal mode measurement: write 0x7866 → wait 12-15ms → read 6 bytes
 *   (T_msb, T_lsb, T_crc, RH_msb, RH_lsb, RH_crc)
 * - Conversions:
 *     T (°C) = -45 + 175 * rawT / 65535
 *     RH (%) = 100 * rawH / 65535
 *
 * Uses XiaoZhi's I2C bus (xiaozhi_get_i2c_bus) — assumes xiaozhi_init() ran.
 */

#include "shtc3.h"
#include "xiaozhi_bridge.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include <string.h>

static const char *TAG = "SHTC3";

#define SHTC3_I2C_ADDR       0x70
#define SHTC3_CMD_WAKE       0x3517
#define SHTC3_CMD_MEASURE_T  0x7866   // T first, clock stretching enabled, normal power
#define SHTC3_CMD_SLEEP      0xB098
#define SHTC3_MEASURE_MS     15

static i2c_master_dev_handle_t s_dev = NULL;
static float s_last_temp = 0.0f;
static float s_last_hum  = 0.0f;

// CRC-8 (Sensirion polynomial 0x31)
static uint8_t crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
        }
    }
    return crc;
}

bool shtc3_init(void)
{
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)xiaozhi_get_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "I2C bus not initialized (xiaozhi_init() must run first)");
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SHTC3_I2C_ADDR,
        .scl_speed_hz    = 100 * 1000,  // 100kHz, SHTC3 supports up to 1MHz
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device: %s", esp_err_to_name(ret));
        return false;
    }

    // Initial probe
    ret = i2c_master_probe(bus, SHTC3_I2C_ADDR, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SHTC3 probe failed: %s (sensor not present?)", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "SHTC3 detected at 0x%02X", SHTC3_I2C_ADDR);
    return true;
}

bool shtc3_measure(void)
{
    if (!s_dev) return false;

    // Send measurement command
    uint8_t cmd[2] = { (uint8_t)(SHTC3_CMD_MEASURE_T >> 8), (uint8_t)(SHTC3_CMD_MEASURE_T & 0xFF) };
    esp_err_t ret = i2c_master_transmit(s_dev, cmd, sizeof(cmd), pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "transmit measure cmd: %s", esp_err_to_name(ret));
        return false;
    }

    // Wait for measurement
    vTaskDelay(pdMS_TO_TICKS(SHTC3_MEASURE_MS));

    // Read 6 bytes
    uint8_t buf[6] = {0};
    ret = i2c_master_receive(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(200));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "receive measure data: %s", esp_err_to_name(ret));
        return false;
    }

    // CRC check
    if (crc8(buf, 2) != buf[2] || crc8(buf + 3, 2) != buf[5]) {
        ESP_LOGW(TAG, "CRC mismatch: %02X%02X%02X %02X%02X%02X",
                 buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
        return false;
    }

    // Convert
    uint16_t raw_t = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t raw_h = ((uint16_t)buf[3] << 8) | buf[4];
    s_last_temp = -45.0f + 175.0f * raw_t / 65535.0f;
    s_last_hum  = 100.0f * raw_h / 65535.0f;

    ESP_LOGI(TAG, "T=%.2fC H=%.2f%%", s_last_temp, s_last_hum);
    return true;
}

float shtc3_last_temperature(void)
{
    return s_last_temp;
}

float shtc3_last_humidity(void)
{
    return s_last_hum;
}

// Functions expected by state_manager.c (CodePilot env_data accessor).
// Implemented here so the existing linker symbols resolve.
float get_shtc3_temperature(void)
{
    return s_last_temp;
}

float get_shtc3_humidity(void)
{
    return s_last_hum;
}
