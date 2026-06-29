/*
 * ws_server.c — minimal WebSocket server (PC bridge connects as client).
 *
 * - httpd instance with CONFIG_HTTPD_WS_SUPPORT=y
 * - URI /ws: WS handshake + recv handler pushes raw frames to a queue
 * - Single-client model: new connection replaces the previous handle
 *
 * Wire format: NDJSON (one JSON per WS text frame + newline).
 */

#include "ws_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include <string.h>

static const char *TAG = "WS_SERVER";

static httpd_handle_t s_server = NULL;
static QueueHandle_t  s_rx_queue = NULL;
static int            s_client_fd = -1;   // -1 = no client

#define WS_QUEUE_LEN  8

typedef struct {
    size_t len;
    char   buf[WS_MAX_MESSAGE_LEN];
} ws_msg_t;

static bool is_ws_frame(httpd_req_t *req)
{
    return strcmp(req->uri, "/ws") == 0;
}

// WS recv handler — called by httpd for each incoming packet
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method != HTTP_GET) {
        // Initial HTTP handshake: just return OK, httpd promotes to WS
        return ESP_OK;
    }
    // If we get here with WS flag, it's a WS frame
    httpd_ws_frame_t ws_pkt;
    uint8_t buf[WS_MAX_MESSAGE_LEN + 1];
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = buf;
    ws_pkt.len = sizeof(buf);  // in: max; out: actual

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WS recv failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ws_pkt.type != HTTPD_WS_TYPE_TEXT) {
        // Ignore binary/close/ping frames (close handled by httpd internally)
        return ESP_OK;
    }

    // Enqueue the message
    ws_msg_t msg;
    size_t n = ws_pkt.len < sizeof(msg.buf) - 1 ? ws_pkt.len : sizeof(msg.buf) - 1;
    memcpy(msg.buf, ws_pkt.payload, n);
    msg.buf[n] = '\0';
    msg.len = n;

    // Drop oldest if queue full (don't block the WS thread)
    if (xQueueSend(s_rx_queue, &msg, 0) != pdPASS) {
        ws_msg_t old;
        xQueueReceive(s_rx_queue, &old, 0);
        xQueueSend(s_rx_queue, &msg, 0);
    }

    return ESP_OK;
}

// Track client connect/disconnect via session open/close callbacks
static esp_err_t on_session_open(httpd_handle_t hd, int sockfd)
{
    ESP_LOGI(TAG, "Client connected: fd=%d", sockfd);
    if (s_client_fd != -1 && s_client_fd != sockfd) {
        ESP_LOGW(TAG, "Old client still in fd=%d, replacing", s_client_fd);
        httpd_sess_trigger_close(hd, s_client_fd);
    }
    s_client_fd = sockfd;
    return ESP_OK;
}

static void on_session_close(httpd_handle_t hd, int sockfd)
{
    ESP_LOGI(TAG, "Client disconnected: fd=%d", sockfd);
    if (s_client_fd == sockfd) {
        s_client_fd = -1;
    }
}

bool ws_server_start(void)
{
    if (s_server) {
        ESP_LOGI(TAG, "Already running");
        return true;
    }
    if (!s_rx_queue) {
        s_rx_queue = xQueueCreate(WS_QUEUE_LEN, sizeof(ws_msg_t));
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = WS_SERVER_PORT;
    cfg.max_open_sockets = WS_MAX_CLIENTS + 1;  // +1 allow brief overlap during reconnect
    cfg.open_fn = on_session_open;
    cfg.close_fn = on_session_close;
    // Allow recv of long frames
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    esp_err_t ret = httpd_start(&s_server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Register WS URI
    httpd_uri_t ws_uri = {
        .uri        = "/ws",
        .method     = HTTP_GET,
        .handler    = ws_handler,
        .user_ctx   = NULL,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &ws_uri);

    ESP_LOGI(TAG, "WS server listening on port %d", WS_SERVER_PORT);
    return true;
}

void ws_server_stop(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = NULL;
    s_client_fd = -1;
    ESP_LOGI(TAG, "WS server stopped");
}

bool ws_server_is_connected(void)
{
    return s_client_fd != -1;
}

bool ws_server_recv_line(char *buf, size_t buf_size, TickType_t timeout_ticks)
{
    if (!s_rx_queue || !buf || buf_size == 0) return false;
    ws_msg_t msg;
    if (xQueueReceive(s_rx_queue, &msg, timeout_ticks) != pdPASS) {
        return false;
    }
    size_t n = msg.len < buf_size - 1 ? msg.len : buf_size - 1;
    memcpy(buf, msg.buf, n);
    buf[n] = '\0';
    return true;
}

bool ws_server_send_text(const char *text)
{
    if (!s_server || s_client_fd < 0 || !text) return false;

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = (uint8_t *)text;
    ws_pkt.len = strlen(text);

    esp_err_t ret = httpd_ws_send_frame_async(s_server, s_client_fd, &ws_pkt);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "send_text failed: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

// Avoid unused-warning on is_ws_frame (kept for future per-URI filtering)
__attribute__((unused)) static void keep_alive_is_ws_frame(void) { (void)is_ws_frame; }
