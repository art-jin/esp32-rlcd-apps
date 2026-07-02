/*
 * serial_input.c — USB Serial/JTAG RX path for bridge_usb.js.
 *
 * See serial_input.h for the architectural note. Short version:
 *   - console owns TX (printf / ESP_LOGI) but does NOT install the driver
 *   - we install the driver ourselves on first start() — the call returns
 *     ESP_ERR_INVALID_STATE if anything else already installed it, which
 *     we treat as success
 *   - reader task calls usb_serial_jtag_read_bytes() directly
 *
 * Reader task accumulates bytes into a line buffer, splits on '\n', and
 * pushes complete lines (NUL-terminated, no newline) into a queue.
 */

#include "serial_input.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "SERIAL_IN";

#define SERIAL_LINE_MAX     512
#define SERIAL_QUEUE_LEN    16
#define SERIAL_READ_CHUNK   64
#define SERIAL_TASK_STACK   3072

typedef struct {
    size_t len;
    char   buf[SERIAL_LINE_MAX];
} line_msg_t;

static QueueHandle_t s_queue    = NULL;
static TaskHandle_t  s_task     = NULL;
static bool          s_running  = false;
static bool          s_driver_installed = false;

// Hold the in-flight partial line across read calls. Single reader task → no lock.
static char    s_rxline[SERIAL_LINE_MAX];
static size_t  s_rxlen = 0;

static void push_line(const char *src, size_t n)
{
    if (n >= SERIAL_LINE_MAX) n = SERIAL_LINE_MAX - 1;

    line_msg_t msg;
    msg.len = n;
    memcpy(msg.buf, src, n);
    msg.buf[n] = '\0';

    // Drop oldest if full (don't block the reader)
    if (xQueueSend(s_queue, &msg, 0) != pdPASS) {
        line_msg_t old;
        xQueueReceive(s_queue, &old, 0);
        xQueueSend(s_queue, &msg, 0);
    }
}

static void reader_task(void *arg)
{
    ESP_LOGI(TAG, "Reader task started (USB Serial/JTAG RX)");

    uint8_t chunk[SERIAL_READ_CHUNK];
    while (s_running) {
        int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(200));
        if (n <= 0) continue;  // timeout or error — try again

        for (int i = 0; i < n; i++) {
            char c = (char)chunk[i];
            if (c == '\n') {
                // Skip empty lines (e.g. CR/LF pairs)
                if (s_rxlen > 0) {
                    push_line(s_rxline, s_rxlen);
                    s_rxlen = 0;
                }
            } else if (c != '\r') {
                if (s_rxlen < sizeof(s_rxline) - 1) {
                    s_rxline[s_rxlen++] = c;
                }
                // else: line too long — silently drop until next \n
            }
        }
    }

    ESP_LOGI(TAG, "Reader task exiting");
    s_rxlen = 0;
    s_task = NULL;
    vTaskDelete(NULL);
}

bool serial_input_start(void)
{
    if (s_running) return true;

    // Console (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y) owns TX but does NOT
    // install the RX driver. usb_serial_jtag_read_bytes() needs the driver
    // context — without it, the first read NULL-derefs inside the driver.
    // Install ourselves; if something else already did, that's fine.
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install: %s", esp_err_to_name(err));
        return false;
    }
    s_driver_installed = (err == ESP_OK);
    ESP_LOGI(TAG, "USB Serial/JTAG driver %s",
             err == ESP_OK ? "installed" : "already installed (sharing)");

    if (!s_queue) {
        s_queue = xQueueCreate(SERIAL_QUEUE_LEN, sizeof(line_msg_t));
        if (!s_queue) {
            ESP_LOGE(TAG, "xQueueCreate failed");
            return false;
        }
    }

    s_running = true;
    s_rxlen = 0;

    if (xTaskCreate(reader_task, "cp_serial", SERIAL_TASK_STACK,
                    NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        s_running = false;
        return false;
    }
    return true;
}

void serial_input_stop(void)
{
    if (!s_running) return;
    s_running = false;
    // Reader exits within ~200ms (its poll timeout)
    if (s_task) {
        // Best-effort join; if it didn't exit, force-delete
        vTaskDelay(pdMS_TO_TICKS(300));
        if (s_task) {
            vTaskDelete(s_task);
            s_task = NULL;
        }
    }
    // Drain queue
    if (s_queue) xQueueReset(s_queue);
}

bool serial_input_is_connected(void)
{
    return s_running;
}

bool serial_input_recv_line(char *buf, size_t buf_size, TickType_t timeout_ticks)
{
    if (!buf || buf_size < 2 || !s_queue) return false;

    line_msg_t msg;
    if (xQueueReceive(s_queue, &msg, timeout_ticks) != pdPASS) {
        return false;
    }
    size_t n = msg.len;
    if (n >= buf_size) n = buf_size - 1;
    memcpy(buf, msg.buf, n);
    buf[n] = '\0';
    return true;
}
