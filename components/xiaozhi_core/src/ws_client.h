#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include <string>
#include <cstdint>
#include "esp_err.h"
#include <esp_websocket_client.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Binary Protocol V2 header: 16 bytes packed
struct __attribute__((packed)) BinaryProtocol2 {
    uint16_t version;       // network byte order
    uint16_t type;          // 0 = audio
    uint32_t reserved;
    uint32_t timestamp;     // milliseconds
    uint32_t payload_size;  // payload bytes
};

static const int BP2_HEADER_SIZE = 16;

// Event bits
#define WS_EVT_SERVER_HELLO  (1 << 0)
#define WS_EVT_DATA_READY    (1 << 1)

class WsClient {
public:
    WsClient();
    ~WsClient();

    esp_err_t Connect(const std::string &url, const std::string &token, int protocol_version);
    esp_err_t SendText(const std::string &json);
    esp_err_t SendAudio(const uint8_t *data, size_t len, uint32_t timestamp);
    int Recv(char *buf, size_t buf_size, bool &is_binary, int timeout_ms);
    void Close();

    /**
     * Register bridge event group for signaling when WS data is received.
     * The WS client sets bridge_data_bit on bridge_events when data arrives.
     */
    void SetBridgeEvents(EventGroupHandle_t events, EventBits_t data_bit);

    bool IsConnected() const { return connected_; }

    int server_sample_rate() const { return server_sample_rate_; }
    int server_frame_duration() const { return server_frame_duration_; }
    const std::string &session_id() const { return session_id_; }
    int protocol_version() const { return protocol_version_; }

private:
    esp_err_t SendClientHello();
    esp_err_t WaitForServerHello(int timeout_ms);
    esp_err_t ParseServerHello(const char *json, int len);
    std::string GetClientUuid();

    // WebSocket event handler
    static void WsEventHandler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data);

    esp_websocket_client_handle_t ws_client_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    bool connected_ = false;

    // Buffer for received data (filled by event handler)
    char *recv_buf_ = nullptr;
    int recv_len_ = 0;
    int recv_cap_ = 0;
    bool recv_binary_ = false;

    // Frame queue to prevent overwrite
    struct Frame {
        char *buf = nullptr;
        int len = 0;
        bool binary = false;
    };
    static const int MAX_FRAMES = 8;
    Frame frames_[MAX_FRAMES];
    int frame_write_idx_ = 0;
    int frame_read_idx_ = 0;
    int frame_count_ = 0;

    std::string session_id_;
    int server_sample_rate_ = 24000;
    int server_frame_duration_ = 60;
    int protocol_version_ = 2;
    std::string mac_address_;
    std::string client_uuid_;

    // Bridge event signaling
    EventGroupHandle_t bridge_events_ = nullptr;
    EventBits_t bridge_data_bit_ = 0;
};

#endif // WS_CLIENT_H
