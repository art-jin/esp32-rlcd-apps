#ifndef WS_SERVER_H
#define WS_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define WS_SERVER_PORT        7897    // match bridge.js default
#define WS_MAX_CLIENTS        1
#define WS_MAX_MESSAGE_LEN    2048

// Start httpd with WS handler on /ws. Idempotent.
bool ws_server_start(void);

// Stop httpd (called on CodePilot app exit).
void ws_server_stop(void);

// True if a client (PC bridge) is currently connected
bool ws_server_is_connected(void);

// Receive a raw NDJSON line. Blocks up to timeout_ticks.
// Returns true if a message was placed in buf (up to buf_size-1 chars + NUL).
bool ws_server_recv_line(char *buf, size_t buf_size, TickType_t timeout_ticks);

// Send a raw text frame to the connected client (no-op if not connected).
bool ws_server_send_text(const char *text);

#endif // WS_SERVER_H
