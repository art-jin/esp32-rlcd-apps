#ifndef SERIAL_INPUT_H
#define SERIAL_INPUT_H

#include <stddef.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"

/**
 * USB Serial/JTAG RX → NDJSON line queue.
 *
 * bridge_usb.js (PC side) writes lines to the ESP32 over USB CDC. The
 * console driver (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y) owns the TX path
 * for log output but does NOT install the underlying usb_serial_jtag
 * driver. We install it ourselves on start() (or share if already
 * installed) so that usb_serial_jtag_read_bytes() can pull bytes from
 * the RX FIFO.
 *
 * Lines are accumulated up to '\n' then pushed to a FreeRTOS queue. Use
 * serial_input_recv_line() from the consumer task (mirrors ws_server.h).
 */

// Start the RX reader task. Idempotent.
bool serial_input_start(void);

// Stop the reader task and free resources. Idempotent.
void serial_input_stop(void);

// True between start() and stop(). Does NOT reflect USB physical connection
// (the USB stack doesn't expose a reliable plug/unplug event here).
bool serial_input_is_connected(void);

// Receive one accumulated line (without trailing '\n'). Blocks up to
// timeout_ticks. Returns true if a line was placed in buf (NUL-terminated).
// buf_size must be >= 2.
bool serial_input_recv_line(char *buf, size_t buf_size, TickType_t timeout_ticks);

#endif // SERIAL_INPUT_H
