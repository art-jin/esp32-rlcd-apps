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
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "keyboard";

#define DEBOUNCE_MS        300
#define LONG_PRESS_MS      500
#define POLL_PERIOD_MS     50
#define KB_QUEUE_LEN       16
#define DOUBLE_CLICK_MS    250  // window for second press to count as double-click

#define KB_GPIO_USER       18

// Cached at init: whether GPIO43 BACK button is physically present.
// Read from NVS ("wifi_creds" namespace, key "has_back_key"). Default false
// matches original Waveshare 3-key RLCD hardware.
static bool s_has_back_key = false;

static QueueHandle_t s_kb_queue = NULL;

// Simple-key debounce (shared across all 4 simple keys)
static volatile TickType_t s_last_simple_press = 0;

// GPIO18 long-press state (written from ISR + timer)
static volatile TickType_t s_user_press_tick = 0;
static volatile bool       s_user_pressed     = false;
static volatile bool       s_user_long_reported = false;
static esp_timer_handle_t  s_user_timer       = NULL;

// Double-click pending state (single-key hw only, !s_has_back_key).
// Set on first short-press release; cleared by either the second press's
// release (→ emit KEY_DOUBLE_CLICK) or by the periodic timer after
// DOUBLE_CLICK_MS elapsed without a second press (→ emit KEY_USER).
static volatile bool       s_pending_click     = false;
static volatile TickType_t s_pending_click_tick = 0;

static const char *key_name(key_event_t k)
{
    switch (k) {
        case KEY_PREV:         return "PREV";
        case KEY_NEXT:         return "NEXT";
        case KEY_ENTER:        return "ENTER";
        case KEY_BACK:         return "BACK";
        case KEY_USER:         return "USER(short)";
        case KEY_DOUBLE_CLICK: return "DOUBLE";
        case KEY_LONG_START:   return "LONG_START";
        case KEY_LONG_END:     return "LONG_END";
        default:               return "NONE";
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

// ── GPIO18 long-press + double-click state machine ───────────────────────────
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
                    if (!s_has_back_key && s_pending_click) {
                        // Second short press within window → double-click
                        s_pending_click = false;
                        key_event_t evt = KEY_DOUBLE_CLICK;
                        xQueueSendFromISR(s_kb_queue, &evt, &xHigherPriorityTaskWoken);
                    } else if (!s_has_back_key) {
                        // Single-key mode: defer emission to detect potential
                        // double-click. Timer emits KEY_USER after window elapses.
                        s_pending_click = true;
                        s_pending_click_tick = now;
                    } else {
                        // 4-key mode: emit immediately (legacy behavior)
                        key_event_t evt = KEY_USER;
                        xQueueSendFromISR(s_kb_queue, &evt, &xHigherPriorityTaskWoken);
                    }
                }
                // else: right at threshold, ignore (long_start may have just fired)
            }
            s_user_pressed = false;
        }
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// 50ms periodic timer: detects long-press threshold crossing and
// (in single-key mode) flushes a pending single-click if no second press arrived.
static void user_timer_cb(void *arg)
{
    // Long-press detection (existing)
    if (s_user_pressed && !s_user_long_reported) {
        int lvl = gpio_get_level(KB_GPIO_USER);
        // Double-check level (race window between ISR and timer)
        if (lvl != 0) {
            s_user_pressed = false;
        } else {
            TickType_t now = xTaskGetTickCount();
            if ((now - s_user_press_tick) >= pdMS_TO_TICKS(LONG_PRESS_MS)) {
                s_user_long_reported = true;
                key_event_t evt = KEY_LONG_START;
                xQueueSend(s_kb_queue, &evt, 0);
            }
        }
    }

    // Pending click timeout (single-key mode only). The s_user_pressed
    // guard prevents racing with a second press that's currently held —
    // in that case the ISR will resolve the gesture on release.
    if (!s_has_back_key && s_pending_click && !s_user_pressed) {
        TickType_t now = xTaskGetTickCount();
        if ((now - s_pending_click_tick) >= pdMS_TO_TICKS(DOUBLE_CLICK_MS)) {
            s_pending_click = false;
            key_event_t evt = KEY_USER;
            xQueueSend(s_kb_queue, &evt, 0);
        }
    }
}

// ── Public API ───────────────────────────────────────────────────────────────
void keyboard_init(void)
{
    s_kb_queue = xQueueCreate(KB_QUEUE_LEN, sizeof(key_event_t));

    // Read 4-key hardware flag from NVS. Default false (3-key factory build).
    nvs_handle_t h;
    if (nvs_open("wifi_creds", NVS_READONLY, &h) == ESP_OK) {
        uint8_t val = 0;
        nvs_get_u8(h, "has_back_key", &val);
        s_has_back_key = (val != 0);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Has BACK key (GPIO43): %s", s_has_back_key ? "yes" : "no");

    // Register simple keys. GPIO43 (BACK) is included only when the device
    // physically has the 4th key — otherwise the pin stays floating/unconfigured
    // and KEY_BACK is reached via the KEY_USER alias in app_manager.c.
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
        if (simple_pins[i].key == KEY_BACK && !s_has_back_key) {
            ESP_LOGI(TAG, "GPIO %d (BACK) skipped — no 4-key hardware",
                     simple_pins[i].gpio);
            continue;
        }
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
            ESP_LOGI(TAG, "GPIO %d (USER) initialized with long-press (%dms threshold)%s, level=%d",
                     KB_GPIO_USER, LONG_PRESS_MS,
                     s_has_back_key ? "" : " + double-click (250ms window)",
                     gpio_get_level(KB_GPIO_USER));
        }
    }

    ESP_LOGI(TAG, "Keyboard: PREV=GPIO1, NEXT=GPIO3, ENTER=GPIO17, BACK=GPIO43%s, USER=GPIO18(long-press)",
             s_has_back_key ? "" : "(via USER alias)");
}

bool keyboard_has_back_key(void)
{
    return s_has_back_key;
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
