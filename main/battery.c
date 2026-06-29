/*
 * Battery ADC reader for ESP32-S3-RLCD-4.2
 * Uses ADC1_CHANNEL_3 (GPIO2) with 3x voltage divider.
 */

#include "battery.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "battery";

static adc_oneshot_unit_handle_t s_adc1_handle;
static adc_cali_handle_t s_cali_handle;
static bool s_initialized = false;

void battery_init(void)
{
    // ADC1 unit
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &s_adc1_handle));

    // Configure channel 3 (GPIO2)
    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc1_handle, ADC_CHANNEL_3, &chan_config));

    // Calibration
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_config, &s_cali_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration failed, using raw values");
        s_cali_handle = NULL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Battery ADC initialized");
}

int battery_get_voltage(void)
{
    if (!s_initialized) return 0;

    int raw;
    esp_err_t err = adc_oneshot_read(s_adc1_handle, ADC_CHANNEL_3, &raw);
    if (err != ESP_OK) return 0;

    int voltage_mv = 0;
    if (s_cali_handle) {
        adc_cali_raw_to_voltage(s_cali_handle, raw, &voltage_mv);
    } else {
        // Rough estimate without calibration: 12-bit, 3.3V ref, 11dB atten (~2500mV full scale)
        voltage_mv = (int)((float)raw * 2500.0f / 4095.0f);
    }

    // 3x voltage divider
    return voltage_mv * 3;
}

int battery_get_level(void)
{
    int voltage_mv = battery_get_voltage();
    if (voltage_mv <= 0) return 0;

    // 3.0V = 0%, 4.12V = 100%
    if (voltage_mv < 3000) return 0;
    if (voltage_mv > 4120) return 100;

    return (int)((float)(voltage_mv - 3000) / 1120.0f * 100.0f);
}
