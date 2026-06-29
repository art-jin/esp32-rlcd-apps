#include "button_handler.h"

#include <esp_log.h>
#include <esp_check.h>
#include <driver/gpio.h>

static const char *TAG = "XiaoZhi BTN";

static EventGroupHandle_t s_events = nullptr;
static volatile TickType_t s_last_press = 0;

static void IRAM_ATTR button_isr(void *arg) {
    TickType_t now = xTaskGetTickCountFromISR();
    if ((now - s_last_press) < pdMS_TO_TICKS(300)) return;
    s_last_press = now;
    BaseType_t higher = pdFALSE;
    xEventGroupSetBitsFromISR(s_events, BTN_PRESS_BIT, &higher);
    portYIELD_FROM_ISR(higher);
}

esp_err_t xz_button_init(gpio_num_t pin, EventGroupHandle_t events) {
    s_events = events;

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_NEGEDGE;
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "GPIO config failed");

    // ISR service may already be installed by another component
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "ISR service install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(pin, button_isr, nullptr),
                        TAG, "ISR handler add failed");

    ESP_LOGI(TAG, "BOOT button initialized on GPIO %d", pin);
    return ESP_OK;
}
