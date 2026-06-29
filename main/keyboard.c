/*
 * 5-key + long-press keyboard driver for ESP32-S3-RLCD-4.2
 *   GPIO1  = PREV    (NEGEDGE + 300ms debounce)
 *   GPIO3  = NEXT    (NEGEDGE + 300ms debounce)
 *   GPIO17 = ENTER   (NEGEDGE + 300ms debounce)
 *   GPIO43 = BACK    (NEGEDGE + 300ms debounce)
 *   GPIO18 = USER    (ANYEDGE + esp_timer 50ms poll, 500ms long-press threshold)
 *
 * Events delivered via FreeRTOS Queue (depth 16).
 */

#include "keyboard.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "keyboard";

#define DEBOUNCE_MS        300
#define LONG_PRESS_MS      500
#define POLL_PERIOD_MS     50
#define KB_QUEUE_LEN       16

#define KB_GPIO_USER       18

static QueueHandle_t s_kb_queue = NULL;

// Simple-key debounce (shared across all 4 simple keys)
static volatile TickType_t s_last_simple_press = 0;

// GPIO18 long-press state (written from ISR + timer)
static volatile TickType_t s_user_press_tick = 0;
static volatile bool       s_user_pressed     = false;
static volatile bool       s_user_long_reported = false;
static esp_timer_handle_t  s_user_timer       = NULL;

static const char *key_name(key_event_t k)
{
    switch (k) {
        case KEY_PREV:       return "PREV";
        case KEY_NEXT:       return "NEXT";
        case KEY_ENTER:      return "ENTER";
        case KEY_BACK:       return "BACK";
        case KEY_USER:       return "USER(short)";
        case KEY_LONG_START: return "LONG_START";
        case KEY_LONG_END:   return "LONG_END";
        default:             return "NONE";
    }
}

// ── Simple keys (GPIO1/3/17/43): NEGEDGE + global 300ms debounce ──────────────
static void IRAM_ATTR kb_simple_isr(void *arg)
{
    key_event_t key = (key_event_t)(int)arg;
    TickType_t now = xTaskGetTickCountFromISR();
    if (now - s_last_simple_press < pdMS_TO_TICKS(DEBOUNCE_MS)) return;
    s_last_simple_press = now;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(s_kb_queue, &key, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// ── GPIO18 long-press state machine ──────────────────────────────────────────
static void IRAM_ATTR kb_user_isr(void *arg)
{
    int level = gpio_get_level(KB_GPIO_USER);  // 0 = pressed, 1 = released
    TickType_t now = xTaskGetTickCountFromISR();
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (level == 0) {
        // Pressed (falling edge)
        s_user_press_tick   = now;
        s_user_pressed      = true;
        s_user_long_reported = false;
    } else {
        // Released (rising edge)
        if (s_user_pressed) {
            if (s_user_long_reported) {
                // Long press ended
                key_event_t evt = KEY_LONG_END;
                xQueueSendFromISR(s_kb_queue, &evt, &xHigherPriorityTaskWoken);
            } else {
                // Short press if released before threshold
                TickType_t elapsed = now - s_user_press_tick;
                if (elapsed < pdMS_TO_TICKS(LONG_PRESS_MS)) {
                    key_event_t evt = KEY_USER;
                    xQueueSendFromISR(s_kb_queue, &evt, &xHigherPriorityTaskWoken);
                }
                // else: right at threshold, ignore (long_start may have just fired)
            }
            s_user_pressed = false;
        }
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// 50ms periodic timer: detects long-press threshold crossing
static void user_timer_cb(void *arg)
{
    if (s_user_long_reported || !s_user_pressed) return;

    int lvl = gpio_get_level(KB_GPIO_USER);
    // Double-check level (race window between ISR and timer)
    if (lvl != 0) {
        s_user_pressed = false;
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if ((now - s_user_press_tick) >= pdMS_TO_TICKS(LONG_PRESS_MS)) {
        s_user_long_reported = true;
        key_event_t evt = KEY_LONG_START;
        xQueueSend(s_kb_queue, &evt, 0);
    }
}

// ── Public API ───────────────────────────────────────────────────────────────
void keyboard_init(void)
{
    s_kb_queue = xQueueCreate(KB_QUEUE_LEN, sizeof(key_event_t));

    // Register 4 simple keys (GPIO1/3/17/43)
    static const struct {
        int gpio;
        key_event_t key;
        const char *label;
    } simple_pins[] = {
        { 1,  KEY_PREV,  "PREV"  },
        { 3,  KEY_NEXT,  "NEXT"  },
        { 17, KEY_ENTER, "ENTER" },
        { 43, KEY_BACK,  "BACK"  },
    };
    const int simple_count = sizeof(simple_pins) / sizeof(simple_pins[0]);

    for (int i = 0; i < simple_count; i++) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << simple_pins[i].gpio),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_NEGEDGE,
        };
        esp_err_t ret = gpio_config(&io_conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "GPIO %d config failed: %s",
                     simple_pins[i].gpio, esp_err_to_name(ret));
            continue;
        }
        ret = gpio_isr_handler_add(simple_pins[i].gpio, kb_simple_isr,
                                   (void *)(int)simple_pins[i].key);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "GPIO %d ISR add failed: %s",
                     simple_pins[i].gpio, esp_err_to_name(ret));
            continue;
        }
        ESP_LOGI(TAG, "GPIO %d (%s) initialized, level=%d",
                 simple_pins[i].gpio, simple_pins[i].label,
                 gpio_get_level(simple_pins[i].gpio));
    }

    // Register GPIO18 (USER) with ANYEDGE for long-press detection
    gpio_config_t user_conf = {
        .pin_bit_mask = (1ULL << KB_GPIO_USER),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t ret = gpio_config(&user_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO %d config failed: %s",
                 KB_GPIO_USER, esp_err_to_name(ret));
    } else {
        ret = gpio_isr_handler_add(KB_GPIO_USER, kb_user_isr, NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "GPIO %d ISR add failed: %s",
                     KB_GPIO_USER, esp_err_to_name(ret));
        } else {
            // Start 50ms periodic timer for long-press polling
            esp_timer_create_args_t timer_args = {
                .callback = user_timer_cb,
                .arg = NULL,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "kb_user_poll",
            };
            esp_timer_create(&timer_args, &s_user_timer);
            esp_timer_start_periodic(s_user_timer, POLL_PERIOD_MS * 1000);
            ESP_LOGI(TAG, "GPIO %d (USER) initialized with long-press (%dms threshold), level=%d",
                     KB_GPIO_USER, LONG_PRESS_MS, gpio_get_level(KB_GPIO_USER));
        }
    }

    ESP_LOGI(TAG, "Keyboard: PREV=GPIO1, NEXT=GPIO3, ENTER=GPIO17, BACK=GPIO43, USER=GPIO18(long-press)");
}

key_event_t keyboard_poll(void)
{
    key_event_t key = KEY_NONE;
    if (xQueueReceive(s_kb_queue, &key, 0) == pdPASS) {
        ESP_LOGI(TAG, "Key event: %s", key_name(key));
    }
    return key;
}

key_event_t keyboard_wait(TickType_t timeout_ticks)
{
    key_event_t key = KEY_NONE;
    if (xQueueReceive(s_kb_queue, &key, timeout_ticks) == pdPASS) {
        ESP_LOGI(TAG, "Key event: %s", key_name(key));
        return key;
    }
    return KEY_NONE;
}
