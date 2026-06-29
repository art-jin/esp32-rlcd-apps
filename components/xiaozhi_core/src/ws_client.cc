#include "ws_client.h"
#include "config.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_crt_bundle.h>
#include <nvs_flash.h>
#include <cstring>
#include <cstdio>
#include <cJSON.h>
#include <arpa/inet.h>

static const char *TAG = "XiaoZhi WS";

// ---------- UUID Generation ----------

std::string WsClient::GetClientUuid() {
    if (!client_uuid_.empty()) return client_uuid_;

    // Try loading from NVS first
    nvs_handle_t h;
    if (nvs_open("board", NVS_READONLY, &h) == ESP_OK) {
        char buf[64];
        size_t len = sizeof(buf);
        if (nvs_get_str(h, "uuid", buf, &len) == ESP_OK) {
            nvs_close(h);
            client_uuid_ = buf;
            return client_uuid_;
        }
        nvs_close(h);
    }

    // Generate new UUID v4
    uint8_t rnd[16];
    esp_fill_random(rnd, 16);
    rnd[6] = (rnd[6] & 0x0F) | 0x40;
    rnd[8] = (rnd[8] & 0x3F) | 0x80;

    char uuid[37];
    snprintf(uuid, sizeof(uuid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7],
             rnd[8], rnd[9], rnd[10], rnd[11], rnd[12], rnd[13], rnd[14], rnd[15]);

    if (nvs_open("board", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "uuid", uuid);
        nvs_commit(h);
        nvs_close(h);
    }
    client_uuid_ = uuid;
    ESP_LOGI(TAG, "Generated Client-Id: %s", uuid);
    return client_uuid_;
}

// ---------- Constructor / Destructor ----------

WsClient::WsClient() {
    events_ = xEventGroupCreate();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    mac_address_ = buf;

    recv_cap_ = 4096;
    recv_buf_ = (char *)malloc(recv_cap_);
    recv_len_ = 0;
}

WsClient::~WsClient() {
    Close();
    if (events_) vEventGroupDelete(events_);
    if (recv_buf_) free(recv_buf_);
}

void WsClient::SetBridgeEvents(EventGroupHandle_t events, EventBits_t data_bit) {
    bridge_events_ = events;
    bridge_data_bit_ = data_bit;
}

// ---------- WebSocket Event Handler ----------

void WsClient::WsEventHandler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data) {
    WsClient *self = static_cast<WsClient *>(arg);
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        self->connected_ = true;
        xEventGroupSetBits(self->events_, WS_EVT_DATA_READY);
        if (self->bridge_events_ && self->bridge_data_bit_) {
            xEventGroupSetBits(self->bridge_events_, self->bridge_data_bit_);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        self->connected_ = false;
        break;

    case WEBSOCKET_EVENT_DATA:
        // Accumulate frame data and queue complete frames
        if (data->data_len > 0) {
            // First chunk of a new frame
            if (data->payload_offset == 0) {
                self->recv_len_ = 0;
                self->recv_binary_ = data->op_code == 0x02;
            }
            // Ensure buffer is large enough
            int total_needed = data->payload_offset + data->data_len + 1;
            if (total_needed > self->recv_cap_) {
                int new_cap = total_needed + 512;
                char *new_buf = (char *)realloc(self->recv_buf_, new_cap);
                if (new_buf) {
                    self->recv_buf_ = new_buf;
                    self->recv_cap_ = new_cap;
                }
            }
            int copy_len = data->data_len;
            if (data->payload_offset + copy_len > self->recv_cap_ - 1)
                copy_len = self->recv_cap_ - 1 - data->payload_offset;
            if (copy_len > 0) {
                memcpy(self->recv_buf_ + data->payload_offset, data->data_ptr, copy_len);
                self->recv_len_ = data->payload_offset + copy_len;
                self->recv_buf_[self->recv_len_] = '\0';
            }
            // When full frame received, copy to queue
            if (data->payload_len > 0 &&
                data->payload_offset + data->data_len >= data->payload_len &&
                self->frame_count_ < self->MAX_FRAMES) {
                int idx = self->frame_write_idx_;
                self->frames_[idx].len = self->recv_len_;
                self->frames_[idx].binary = self->recv_binary_;
                self->frames_[idx].buf = (char *)malloc(self->recv_len_ + 1);
                if (self->frames_[idx].buf) {
                    memcpy(self->frames_[idx].buf, self->recv_buf_, self->recv_len_ + 1);
                    self->frame_write_idx_ = (self->frame_write_idx_ + 1) % self->MAX_FRAMES;
                    self->frame_count_++;
                    xEventGroupSetBits(self->events_, WS_EVT_DATA_READY);
                    if (self->bridge_events_ && self->bridge_data_bit_) {
                        xEventGroupSetBits(self->bridge_events_, self->bridge_data_bit_);
                    }
                }
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}

// ---------- Connect ----------

esp_err_t WsClient::Connect(const std::string &url, const std::string &token, int protocol_version) {
    protocol_version_ = protocol_version;

    // Build headers string (each header terminated with \r\n)
    std::string headers_str;
    if (token.find("Bearer ") == std::string::npos && token.find("bearer ") == std::string::npos) {
        headers_str += "Authorization: Bearer " + token + "\r\n";
    } else {
        headers_str += "Authorization: " + token + "\r\n";
    }
    headers_str += "Protocol-Version: " + std::to_string(protocol_version_) + "\r\n";
    headers_str += "Device-Id: " + mac_address_ + "\r\n";
    headers_str += "Client-Id: " + GetClientUuid() + "\r\n";

    // WebSocket client config
    esp_websocket_client_config_t cfg = {};
    cfg.uri = url.c_str();
    cfg.reconnect_timeout_ms = 0;
    cfg.network_timeout_ms = 10000;
    cfg.buffer_size = 8192;          // larger buffer for server hello + MCP messages
    cfg.skip_cert_common_name_check = true;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.headers = headers_str.c_str();

    ESP_LOGI(TAG, "Connecting to %s (version=%d)", url.c_str(), protocol_version_);

    ws_client_ = esp_websocket_client_init(&cfg);
    if (!ws_client_) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return ESP_FAIL;
    }

    // Register event handler
    esp_websocket_register_events(ws_client_, WEBSOCKET_EVENT_ANY,
                                   WsEventHandler, this);

    // Clear event flags
    xEventGroupClearBits(events_, WS_EVT_DATA_READY | WS_EVT_SERVER_HELLO);

    // Start connection (async - actual connection happens in background task)
    esp_err_t err = esp_websocket_client_start(ws_client_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(ws_client_);
        ws_client_ = nullptr;
        return ESP_FAIL;
    }

    // Wait for CONNECTED event (up to 15s)
    EventBits_t bits = xEventGroupWaitBits(events_, WS_EVT_DATA_READY,
                                             pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
    if (!(bits & WS_EVT_DATA_READY) || !connected_) {
        ESP_LOGE(TAG, "WebSocket connection timeout");
        Close();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WebSocket connected");

    // Send Client Hello
    esp_err_t hello_err = SendClientHello();
    if (hello_err != ESP_OK) {
        Close();
        return hello_err;
    }

    // Wait for Server Hello (10s timeout)
    hello_err = WaitForServerHello(10000);
    if (hello_err != ESP_OK) {
        Close();
        return hello_err;
    }

    return ESP_OK;
}

// ---------- Client Hello ----------

esp_err_t WsClient::SendClientHello() {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", protocol_version_);
    cJSON_AddStringToObject(root, "transport", "websocket");

    cJSON *features = cJSON_CreateObject();
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);

    cJSON *audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", XZ_OPUS_SAMPLE_RATE);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", XZ_OPUS_FRAME_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);

    char *json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_Delete(root);
    free(json_str);

    ESP_LOGI(TAG, "Sending Client Hello: %s", json.c_str());
    esp_err_t ret = SendText(json);
    ESP_LOGI(TAG, "Sent Client Hello (%zu bytes), ret=%d", json.length(), ret);
    return ret;
}

// ---------- Wait for Server Hello ----------

esp_err_t WsClient::WaitForServerHello(int timeout_ms) {
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        // Wait for data event
        EventBits_t bits = xEventGroupWaitBits(events_, WS_EVT_DATA_READY,
                                                 pdTRUE, pdFALSE, pdMS_TO_TICKS(500));
        if (bits & WS_EVT_DATA_READY) {
            // Process all queued frames
            while (frame_count_ > 0) {
                int idx = frame_read_idx_;
                char *fbuf = frames_[idx].buf;
                int flen = frames_[idx].len;
                bool fbin = frames_[idx].binary;

                ESP_LOGI(TAG, "Processing queued frame: %d bytes, binary=%d", flen, fbin);

                if (!fbin && fbuf) {
                    ESP_LOGI(TAG, "Text: %.*s", flen > 200 ? 200 : flen, fbuf);
                    esp_err_t err = ParseServerHello(fbuf, flen);
                    if (err == ESP_OK) {
                        free(fbuf);
                        frames_[idx].buf = nullptr;
                        frame_read_idx_ = (frame_read_idx_ + 1) % MAX_FRAMES;
                        frame_count_--;
                        return ESP_OK;
                    }
                }

                free(fbuf);
                frames_[idx].buf = nullptr;
                frame_read_idx_ = (frame_read_idx_ + 1) % MAX_FRAMES;
                frame_count_--;
            }
        }
        if (!connected_) {
            ESP_LOGE(TAG, "Disconnected while waiting for Server Hello");
            return ESP_FAIL;
        }
    }

    ESP_LOGE(TAG, "Server Hello timeout (%dms)", timeout_ms);
    return ESP_ERR_TIMEOUT;
}

// ---------- Parse Server Hello ----------

esp_err_t WsClient::ParseServerHello(const char *json_str, int len) {
    cJSON *root = cJSON_ParseWithLength(json_str, len);
    if (!root) return ESP_FAIL;

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!type || !cJSON_IsString(type) || strcmp(type->valuestring, "hello") != 0) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *session = cJSON_GetObjectItem(root, "session_id");
    if (session && cJSON_IsString(session)) {
        session_id_ = session->valuestring;
    }

    cJSON *audio = cJSON_GetObjectItem(root, "audio_params");
    if (audio) {
        cJSON *sr = cJSON_GetObjectItem(audio, "sample_rate");
        if (sr && cJSON_IsNumber(sr)) {
            server_sample_rate_ = (int)sr->valuedouble;
        }
        cJSON *fd = cJSON_GetObjectItem(audio, "frame_duration");
        if (fd && cJSON_IsNumber(fd)) {
            server_frame_duration_ = (int)fd->valuedouble;
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Server Hello: session=%s, sample_rate=%d, frame_duration=%d",
             session_id_.c_str(), server_sample_rate_, server_frame_duration_);

    xEventGroupSetBits(events_, WS_EVT_SERVER_HELLO);
    return ESP_OK;
}

// ---------- Send / Recv ----------

esp_err_t WsClient::SendText(const std::string &json) {
    if (!connected_ || !ws_client_) return ESP_FAIL;
    int ret = esp_websocket_client_send_text(ws_client_, json.c_str(), json.length(), 5000);
    if (ret < 0) {
        ESP_LOGE(TAG, "Send text failed");
        connected_ = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t WsClient::SendAudio(const uint8_t *data, size_t len, uint32_t timestamp) {
    if (!connected_ || !ws_client_) return ESP_FAIL;

    if (protocol_version_ == 2) {
        uint8_t pkt[BP2_HEADER_SIZE + XZ_OPUS_MAX_PACKET];
        BinaryProtocol2 *hdr = reinterpret_cast<BinaryProtocol2 *>(pkt);
        hdr->version = htons(protocol_version_);
        hdr->type = htons(0);
        hdr->reserved = 0;
        hdr->timestamp = htonl(timestamp);
        hdr->payload_size = htonl((uint32_t)len);
        memcpy(pkt + BP2_HEADER_SIZE, data, len);

        int ret = esp_websocket_client_send_bin(ws_client_, (const char *)pkt,
                                                 BP2_HEADER_SIZE + len, 5000);
        if (ret < 0) { connected_ = false; return ESP_FAIL; }
    } else if (protocol_version_ == 3) {
        uint8_t pkt[4 + XZ_OPUS_MAX_PACKET];
        pkt[0] = 0;
        pkt[1] = 0;
        pkt[2] = (uint8_t)((len >> 8) & 0xFF);
        pkt[3] = (uint8_t)(len & 0xFF);
        memcpy(pkt + 4, data, len);

        int ret = esp_websocket_client_send_bin(ws_client_, (const char *)pkt,
                                                 4 + len, 5000);
        if (ret < 0) { connected_ = false; return ESP_FAIL; }
    } else {
        int ret = esp_websocket_client_send_bin(ws_client_, (const char *)data, len, 5000);
        if (ret < 0) { connected_ = false; return ESP_FAIL; }
    }
    return ESP_OK;
}

int WsClient::Recv(char *buf, size_t buf_size, bool &is_binary, int timeout_ms) {
    if (!ws_client_) return -1;

    // Check if frame available in queue
    if (frame_count_ > 0) {
        int idx = frame_read_idx_;
        int copy_len = frames_[idx].len;
        if (copy_len > (int)buf_size) copy_len = (int)buf_size;
        memcpy(buf, frames_[idx].buf, copy_len);
        is_binary = frames_[idx].binary;
        free(frames_[idx].buf);
        frames_[idx].buf = nullptr;
        frame_read_idx_ = (frame_read_idx_ + 1) % MAX_FRAMES;
        frame_count_--;
        return copy_len;
    }

    // Wait for new data
    EventBits_t bits = xEventGroupWaitBits(events_, WS_EVT_DATA_READY,
                                             pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (!(bits & WS_EVT_DATA_READY)) return 0;

    if (frame_count_ > 0) {
        int idx = frame_read_idx_;
        int copy_len = frames_[idx].len;
        if (copy_len > (int)buf_size) copy_len = (int)buf_size;
        memcpy(buf, frames_[idx].buf, copy_len);
        is_binary = frames_[idx].binary;
        free(frames_[idx].buf);
        frames_[idx].buf = nullptr;
        frame_read_idx_ = (frame_read_idx_ + 1) % MAX_FRAMES;
        frame_count_--;
        return copy_len;
    }

    return 0;
}

void WsClient::Close() {
    if (ws_client_) {
        esp_websocket_client_stop(ws_client_);
        esp_websocket_client_destroy(ws_client_);
        ws_client_ = nullptr;
    }
    connected_ = false;
    recv_len_ = 0;

    // Free frame buffers and reset queue
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (frames_[i].buf) {
            free(frames_[i].buf);
            frames_[i].buf = nullptr;
        }
    }
    frame_write_idx_ = 0;
    frame_read_idx_ = 0;
    frame_count_ = 0;
}
