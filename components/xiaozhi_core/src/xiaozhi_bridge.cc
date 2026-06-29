#include "xiaozhi_bridge.h"
#include "config.h"

#include <esp_log.h>
#include <esp_check.h>
#include <cstring>
#include <arpa/inet.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <driver/i2s_tdm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <cJSON.h>
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev_vol.h"

#include "ota_client.h"
#include "ws_client.h"
#include "opus_codec.h"
#include "audio_pipeline.h"
#include "button_handler.h"

#define TAG "XiaoZhi"

static const int DMA_DESC_NUM  = 6;
static const int DMA_FRAME_NUM = 240;

// Event bits
#define XZ_EVT_BUTTON     (1 << 0)
#define XZ_EVT_STOP       (1 << 1)
#define XZ_EVT_AUDIO_SEND (1 << 2)  // Pipeline has encoded audio ready
#define XZ_EVT_WS_DATA    (1 << 3)  // WebSocket has received data
#define XZ_EVT_KILL       (1 << 4)  // Force exit (from xiaozhi_force_disconnect)

// State
static xiaozhi_state_t s_state = XZ_IDLE;
static xiaozhi_text_cb  s_text_cb  = nullptr;
static xiaozhi_state_cb s_state_cb = nullptr;

// Audio handles
static i2c_master_bus_handle_t s_i2c_bus   = nullptr;
static i2s_chan_handle_t       s_tx_handle = nullptr;
static i2s_chan_handle_t       s_rx_handle = nullptr;
static const audio_codec_data_if_t*   s_data_if      = nullptr;
static const audio_codec_ctrl_if_t*   s_out_ctrl_if  = nullptr;
static const audio_codec_ctrl_if_t*   s_in_ctrl_if   = nullptr;
static const audio_codec_gpio_if_t*   s_gpio_if      = nullptr;
static const audio_codec_if_t*        s_out_codec_if = nullptr;
static const audio_codec_if_t*        s_in_codec_if  = nullptr;
static esp_codec_dev_handle_t s_output_dev = nullptr;
static esp_codec_dev_handle_t s_input_dev  = nullptr;

// Protocol modules
static OtaClient     *s_ota      = nullptr;
static WsClient      *s_ws       = nullptr;
static OpusCodec     *s_opus     = nullptr;
static AudioPipeline *s_pipeline = nullptr;
static EventGroupHandle_t s_events = nullptr;

// Session info
static std::string s_session_id;

// Forward declarations
static void disconnect_from_server(void);

static void set_state(xiaozhi_state_t state) {
    s_state = state;
    if (s_state_cb) {
        s_state_cb(state);
    }
}

static void fire_text_cb(const char *text) {
    if (s_text_cb) {
        s_text_cb(text);
    }
}

// ---------- Audio Hardware Init (from Phase 2) ----------

static esp_err_t init_i2c(void) {
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = (gpio_num_t)XZ_I2C_SDA;
    cfg.scl_io_num = (gpio_num_t)XZ_I2C_SCL;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.intr_priority = 0;
    cfg.trans_queue_depth = 0;
    cfg.flags.enable_internal_pullup = 1;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_i2c_bus), TAG, "I2C bus init failed");
    ESP_LOGI(TAG, "I2C bus initialized (SDA=%d, SCL=%d)", XZ_I2C_SDA, XZ_I2C_SCL);
    return ESP_OK;
}

static esp_err_t init_i2s(void) {
    i2s_chan_config_t chan_cfg = {};
    chan_cfg.id = I2S_NUM_0;
    chan_cfg.role = I2S_ROLE_MASTER;
    chan_cfg.dma_desc_num = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    chan_cfg.auto_clear_after_cb = true;
    chan_cfg.auto_clear_before_cb = false;
    chan_cfg.intr_priority = 0;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle),
                        TAG, "I2S channel create failed");

    // TX: Standard I2S mode (ES8311 DAC output)
    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = {
        .sample_rate_hz = XZ_SAMPLE_RATE,
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .ext_clk_freq_hz = 0,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bclk_div = 8,
    };
    std_cfg.slot_cfg = {
        .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
        .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
        .slot_mode = I2S_SLOT_MODE_STEREO,
        .slot_mask = I2S_STD_SLOT_BOTH,
        .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
        .ws_pol = false,
        .bit_shift = true,
        .left_align = true,
        .big_endian = false,
        .bit_order_lsb = false,
    };
    std_cfg.gpio_cfg = {
        .mclk = (gpio_num_t)XZ_I2S_MCLK,
        .bclk = (gpio_num_t)XZ_I2S_BCLK,
        .ws = (gpio_num_t)XZ_I2S_WS,
        .dout = (gpio_num_t)XZ_I2S_DOUT,
        .din = I2S_GPIO_UNUSED,
        .invert_flags = {},
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg),
                        TAG, "I2S STD mode init failed");

    // RX: TDM mode (ES7210 ADC input, 4 mic channels)
    i2s_tdm_config_t tdm_cfg = {};
    tdm_cfg.clk_cfg = {
        .sample_rate_hz = XZ_SAMPLE_RATE,
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .ext_clk_freq_hz = 0,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bclk_div = 8,
    };
    tdm_cfg.slot_cfg = {
        .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
        .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
        .slot_mode = I2S_SLOT_MODE_STEREO,
        .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 |
                                          I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .ws_width = I2S_TDM_AUTO_WS_WIDTH,
        .ws_pol = false,
        .bit_shift = true,
        .left_align = false,
        .big_endian = false,
        .bit_order_lsb = false,
        .skip_mask = false,
        .total_slot = I2S_TDM_AUTO_SLOT_NUM,
    };
    tdm_cfg.gpio_cfg = {
        .mclk = (gpio_num_t)XZ_I2S_MCLK,
        .bclk = (gpio_num_t)XZ_I2S_BCLK,
        .ws = (gpio_num_t)XZ_I2S_WS,
        .dout = I2S_GPIO_UNUSED,
        .din = (gpio_num_t)XZ_I2S_DIN,
        .invert_flags = {},
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_rx_handle, &tdm_cfg),
                        TAG, "I2S TDM mode init failed");

    ESP_LOGI(TAG, "I2S duplex channels created (TX=STD, RX=TDM, %dHz)", XZ_SAMPLE_RATE);
    return ESP_OK;
}

static esp_err_t init_codecs(void) {
    // Shared I2S data interface
    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = I2S_NUM_0;
    i2s_cfg.rx_handle = s_rx_handle;
    i2s_cfg.tx_handle = s_tx_handle;
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!s_data_if) {
        ESP_LOGE(TAG, "Failed to create I2S data interface");
        return ESP_FAIL;
    }

    // --- ES8311 (Output / Speaker) ---
    audio_codec_i2c_cfg_t i2c_out_cfg = {};
    i2c_out_cfg.port = (i2c_port_t)1;
    i2c_out_cfg.addr = ES8311_CODEC_DEFAULT_ADDR;
    i2c_out_cfg.bus_handle = s_i2c_bus;
    s_out_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_out_cfg);
    if (!s_out_ctrl_if) {
        ESP_LOGE(TAG, "Failed to create ES8311 I2C ctrl");
        return ESP_FAIL;
    }

    s_gpio_if = audio_codec_new_gpio();
    if (!s_gpio_if) return ESP_FAIL;

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = s_out_ctrl_if;
    es8311_cfg.gpio_if = s_gpio_if;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = XZ_PA_PIN;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    s_out_codec_if = es8311_codec_new(&es8311_cfg);
    if (!s_out_codec_if) {
        ESP_LOGE(TAG, "Failed to create ES8311 codec");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t out_dev_cfg = {};
    out_dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    out_dev_cfg.codec_if = s_out_codec_if;
    out_dev_cfg.data_if = s_data_if;
    s_output_dev = esp_codec_dev_new(&out_dev_cfg);
    if (!s_output_dev) return ESP_FAIL;

    ESP_LOGI(TAG, "ES8311 output codec initialized (addr=0x%02X)", ES8311_CODEC_DEFAULT_ADDR);

    // --- ES7210 (Input / Microphone) ---
    audio_codec_i2c_cfg_t i2c_in_cfg = {};
    i2c_in_cfg.port = (i2c_port_t)1;
    i2c_in_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
    i2c_in_cfg.bus_handle = s_i2c_bus;
    s_in_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_in_cfg);
    if (!s_in_ctrl_if) return ESP_FAIL;

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = s_in_ctrl_if;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    s_in_codec_if = es7210_codec_new(&es7210_cfg);
    if (!s_in_codec_if) {
        ESP_LOGE(TAG, "Failed to create ES7210 codec");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t in_dev_cfg = {};
    in_dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    in_dev_cfg.codec_if = s_in_codec_if;
    in_dev_cfg.data_if = s_data_if;
    s_input_dev = esp_codec_dev_new(&in_dev_cfg);
    if (!s_input_dev) return ESP_FAIL;

    ESP_LOGI(TAG, "ES7210 input codec initialized (addr=0x%02X)", ES7210_CODEC_DEFAULT_ADDR);
    return ESP_OK;
}

static esp_err_t start_audio(void) {
    // Enable I2S channels
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "TX enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "RX enable failed");

    // Open input codec (mic, 4-channel TDM, reference channel for AEC)
    esp_codec_dev_sample_info_t in_fs = {};
    in_fs.bits_per_sample = 16;
    in_fs.channel = 4;
    in_fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
                         ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
    in_fs.sample_rate = XZ_SAMPLE_RATE;
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_input_dev, &in_fs), TAG, "Input open failed");
    ESP_RETURN_ON_ERROR(
        esp_codec_dev_set_in_channel_gain(s_input_dev,
            ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), 30.0f),
        TAG, "Set mic gain failed");

    // Open output codec (speaker, mono)
    esp_codec_dev_sample_info_t out_fs = {};
    out_fs.bits_per_sample = 16;
    out_fs.channel = 1;
    out_fs.sample_rate = XZ_SAMPLE_RATE;
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_output_dev, &out_fs), TAG, "Output open failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_output_dev, 70), TAG, "Set volume failed");

    ESP_LOGI(TAG, "Audio pipeline started (24kHz, 16-bit, mic gain 30dB, vol 70)");
    return ESP_OK;
}

// ---------- JSON Message Handling ----------

static void handle_json_message(const char *json, int len) {
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return;

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!type || !cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    const char *type_str = type->valuestring;

    if (strcmp(type_str, "tts") == 0) {
        cJSON *state = cJSON_GetObjectItem(root, "state");
        if (state && cJSON_IsString(state)) {
            if (strcmp(state->valuestring, "start") == 0) {
                ESP_LOGI(TAG, "TTS start");
                set_state(XZ_SPEAKING);
            } else if (strcmp(state->valuestring, "stop") == 0) {
                ESP_LOGI(TAG, "TTS stop, back to listening");
                // Stay connected — go back to listening for continuous conversation
                set_state(XZ_LISTENING);
                // Drain stale encoded audio accumulated during TTS playback
                if (s_pipeline) {
                    SendItem discard;
                    while (s_pipeline->DequeueSendAudio(discard)) {}
                }
                // Tell server to restart listening (matching factory behavior)
                if (s_ws && s_ws->IsConnected()) {
                    cJSON *msg = cJSON_CreateObject();
                    cJSON_AddStringToObject(msg, "type", "listen");
                    cJSON_AddStringToObject(msg, "state", "start");
                    if (!s_session_id.empty())
                        cJSON_AddStringToObject(msg, "session_id", s_session_id.c_str());
                    cJSON_AddStringToObject(msg, "mode", "auto");
                    char *json_str = cJSON_PrintUnformatted(msg);
                    s_ws->SendText(json_str);
                    free(json_str);
                    cJSON_Delete(msg);
                    ESP_LOGI(TAG, "Sent listen start (auto) to restart listening");
                }
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                cJSON *text = cJSON_GetObjectItem(root, "text");
                if (text && cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "TTS text: %s", text->valuestring);
                    fire_text_cb(text->valuestring);
                }
            }
        }
    } else if (strcmp(type_str, "stt") == 0) {
        cJSON *text = cJSON_GetObjectItem(root, "text");
        if (text && cJSON_IsString(text)) {
            ESP_LOGI(TAG, "STT: %s", text->valuestring);
            fire_text_cb(text->valuestring);
        }
    } else if (strcmp(type_str, "listen") == 0) {
        cJSON *state = cJSON_GetObjectItem(root, "state");
        if (state && cJSON_IsString(state)) {
            ESP_LOGI(TAG, "Server listen: %s", state->valuestring);
            if (strcmp(state->valuestring, "stop") == 0) {
                // Server ended the session (timeout or explicit stop)
                disconnect_from_server();
                set_state(XZ_IDLE);
            }
        }
    }

    cJSON_Delete(root);
}

// ---------- Protocol Connection ----------

static esp_err_t connect_to_server(void) {
    set_state(XZ_CONNECTING);

    // Get credentials
    std::string url, token;
    int version = 2;
    esp_err_t ret = s_ota->LoadCredentials(url, token, version);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "No cached credentials, performing OTA activation...");
        ret = s_ota->Activate();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "OTA activation failed");
            return ret;
        }
        ret = s_ota->LoadCredentials(url, token, version);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to load credentials after activation");
            return ret;
        }
    }

    // Initialize Opus with server sample rate (we'll get it after hello, use default first)
    ret = s_opus->Init(XZ_SAMPLE_RATE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Opus init failed");
        return ret;
    }

    // Connect WebSocket
    ret = s_ws->Connect(url, token, version);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket connect failed");
        return ret;
    }

    // Store session info
    s_session_id = s_ws->session_id();

    // If server sample rate differs from what we initialized, reinit decoder
    if (s_ws->server_sample_rate() != XZ_SAMPLE_RATE) {
        ESP_LOGI(TAG, "Re-initializing Opus for server rate %d", s_ws->server_sample_rate());
        s_opus->Init(s_ws->server_sample_rate());
    }

    // Re-enable I2S channels if they were disabled (e.g., after disconnect)
    i2s_channel_enable(s_tx_handle);
    i2s_channel_enable(s_rx_handle);

    // Initialize and start audio pipeline
    ret = s_pipeline->Init(s_opus, s_ws->server_sample_rate());
    if (ret != ESP_OK) return ret;

    // Register bridge events so pipeline/WS signal the main event loop
    s_pipeline->SetBridgeEvents(s_events, XZ_EVT_AUDIO_SEND);
    s_ws->SetBridgeEvents(s_events, XZ_EVT_WS_DATA);

    ret = s_pipeline->Start(s_input_dev, s_output_dev);
    if (ret != ESP_OK) return ret;

    // Send listen start
    cJSON *hello = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "type", "listen");
    cJSON_AddStringToObject(hello, "state", "start");
    cJSON_AddStringToObject(hello, "session_id", s_session_id.c_str());
    cJSON_AddStringToObject(hello, "mode", "auto");
    char *json_str = cJSON_PrintUnformatted(hello);
    s_ws->SendText(json_str);
    free(json_str);
    cJSON_Delete(hello);

    set_state(XZ_LISTENING);
    ESP_LOGI(TAG, "Connected to XiaoZhi server, listening...");
    return ESP_OK;
}

static void disconnect_from_server(void) {
    if (s_pipeline) s_pipeline->Stop();
    if (s_ws) s_ws->Close();
    s_session_id.clear();
    // Disable I2S channels to silence mic/speaker between sessions
    if (s_tx_handle) i2s_channel_disable(s_tx_handle);
    if (s_rx_handle) i2s_channel_disable(s_rx_handle);
}

// ---------- Public C API ----------

esp_err_t xiaozhi_init(void) {
    static bool s_inited = false;
    if (s_inited) {
        ESP_LOGI(TAG, "XiaoZhi already initialized, skipping");
        return ESP_OK;
    }
    s_inited = true;

    ESP_LOGI(TAG, "Initializing XiaoZhi audio + protocol...");
    set_state(XZ_CONNECTING);

    esp_err_t ret = init_i2c();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(ret));
        set_state(XZ_ERROR);
        return ret;
    }

    ret = init_i2s();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(ret));
        set_state(XZ_ERROR);
        return ret;
    }

    ret = init_codecs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Codec init failed: %s", esp_err_to_name(ret));
        set_state(XZ_ERROR);
        return ret;
    }

    ret = start_audio();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Audio start failed: %s", esp_err_to_name(ret));
        set_state(XZ_ERROR);
        return ret;
    }

    // Create protocol modules
    s_events = xEventGroupCreate();
    s_ota = new OtaClient();
    s_ws = new WsClient();
    s_opus = new OpusCodec();
    s_pipeline = new AudioPipeline();

    // Initialize BOOT button
    xz_button_init((gpio_num_t)XZ_BOOT_BUTTON, s_events);

    set_state(XZ_IDLE);
    ESP_LOGI(TAG, "XiaoZhi ready (press BOOT button to talk)");
    return ESP_OK;
}

void xiaozhi_register_text_cb(xiaozhi_text_cb cb) {
    s_text_cb = cb;
}

void xiaozhi_register_state_cb(xiaozhi_state_cb cb) {
    s_state_cb = cb;
}

void xiaozhi_start_listening(void) {
    if (s_events) {
        xEventGroupSetBits(s_events, XZ_EVT_BUTTON);
    }
}

void xiaozhi_stop_listening(void) {
    if (s_events) {
        xEventGroupSetBits(s_events, XZ_EVT_STOP);
    }
}

void xiaozhi_disable_audio(void) {
    if (s_tx_handle) i2s_channel_disable(s_tx_handle);
    if (s_rx_handle) i2s_channel_disable(s_rx_handle);
    ESP_LOGI(TAG, "Audio disabled");
}

void xiaozhi_prepare_reconnect(void) {
    if (s_events) {
        // Clear all stale event bits from previous session
        xEventGroupClearBits(s_events,
            XZ_EVT_BUTTON | XZ_EVT_STOP | XZ_EVT_AUDIO_SEND | XZ_EVT_WS_DATA);
    }
    set_state(XZ_IDLE);
    ESP_LOGI(TAG, "Prepared for reconnect (events cleared, state=IDLE)");
}

void xiaozhi_run(void) {
    ESP_LOGI(TAG, "XiaoZhi event loop running");

    char recv_buf[4096];

    while (1) {
        // Quick check for KILL at top of each iteration
        EventBits_t kill_check = xEventGroupGetBits(s_events);
        if (kill_check & XZ_EVT_KILL) {
            ESP_LOGI(TAG, "KILL received, exiting event loop");
            break;
        }

        switch (s_state) {
        case XZ_IDLE: {
            // Wait for BOOT button press or KILL
            EventBits_t bits = xEventGroupWaitBits(s_events,
                XZ_EVT_BUTTON | XZ_EVT_STOP | XZ_EVT_KILL, pdTRUE, pdFALSE, portMAX_DELAY);
            if (bits & XZ_EVT_KILL) goto exit_loop;
            if (bits & XZ_EVT_BUTTON) {
                esp_err_t ret = connect_to_server();
                if (ret != ESP_OK) {
                    set_state(XZ_ERROR);
                }
            }
            break;
        }

        case XZ_LISTENING: {
            // Wait for any event: audio ready, WS data, stop, or KILL
            EventBits_t bits = xEventGroupWaitBits(s_events,
                XZ_EVT_STOP | XZ_EVT_AUDIO_SEND | XZ_EVT_WS_DATA | XZ_EVT_KILL,
                pdTRUE, pdFALSE, pdMS_TO_TICKS(20));

            if (bits & XZ_EVT_KILL) goto exit_loop;

            // Check stop first
            if (bits & XZ_EVT_STOP) {
                // Send listen stop
                cJSON *msg = cJSON_CreateObject();
                cJSON_AddStringToObject(msg, "type", "listen");
                cJSON_AddStringToObject(msg, "state", "stop");
                if (!s_session_id.empty())
                    cJSON_AddStringToObject(msg, "session_id", s_session_id.c_str());
                char *json_str = cJSON_PrintUnformatted(msg);
                s_ws->SendText(json_str);
                free(json_str);
                cJSON_Delete(msg);
                disconnect_from_server();
                set_state(XZ_IDLE);
                break;
            }

            // Send encoded audio from pipeline
            if (s_pipeline && s_ws && s_ws->IsConnected()) {
                SendItem item;
                while (s_pipeline->DequeueSendAudio(item)) {
                    s_ws->SendAudio(item.opus, item.opus_len, item.timestamp);
                }
            }

            // Receive WS data (non-blocking — event told us data is ready)
            if (s_ws && s_ws->IsConnected()) {
                bool is_binary = false;
                int len = s_ws->Recv(recv_buf, sizeof(recv_buf), is_binary, 0);
                if (len > 0) {
                    if (is_binary) {
                        // Strip binary protocol header
                        int version = s_ws->protocol_version();
                        const uint8_t *payload = nullptr;
                        int payload_len = 0;

                        if (version == 2 && len >= BP2_HEADER_SIZE) {
                            BinaryProtocol2 *hdr = (BinaryProtocol2 *)recv_buf;
                            payload_len = ntohl(hdr->payload_size);
                            payload = (const uint8_t *)(recv_buf + BP2_HEADER_SIZE);
                            if (payload_len > len - BP2_HEADER_SIZE)
                                payload_len = len - BP2_HEADER_SIZE;
                        } else if (version == 3 && len >= 4) {
                            payload_len = (uint8_t)recv_buf[2] << 8 | (uint8_t)recv_buf[3];
                            payload = (const uint8_t *)(recv_buf + 4);
                            if (payload_len > len - 4)
                                payload_len = len - 4;
                        } else {
                            // V1: raw
                            payload = (const uint8_t *)recv_buf;
                            payload_len = len;
                        }

                        if (payload && payload_len > 0 && s_pipeline) {
                            s_pipeline->EnqueueReceivedAudio(payload, payload_len);
                        }
                    } else {
                        // Text JSON message
                        handle_json_message(recv_buf, len);
                    }
                } else if (len < 0) {
                    ESP_LOGW(TAG, "WebSocket disconnected");
                    disconnect_from_server();
                    set_state(XZ_ERROR);
                }
            }
            break;
        }

        case XZ_SPEAKING: {
            // Wait for WS data, stop, or KILL
            EventBits_t bits = xEventGroupWaitBits(s_events,
                XZ_EVT_STOP | XZ_EVT_WS_DATA | XZ_EVT_KILL,
                pdTRUE, pdFALSE, pdMS_TO_TICKS(50));

            if (bits & XZ_EVT_KILL) goto exit_loop;

            if (bits & XZ_EVT_STOP) {
                disconnect_from_server();
                set_state(XZ_IDLE);
                break;
            }

            // Receive WS data (non-blocking)
            if (s_ws && s_ws->IsConnected()) {
                bool is_binary = false;
                int len = s_ws->Recv(recv_buf, sizeof(recv_buf), is_binary, 0);
                if (len > 0) {
                    if (is_binary) {
                        int version = s_ws->protocol_version();
                        const uint8_t *payload = nullptr;
                        int payload_len = 0;

                        if (version == 2 && len >= BP2_HEADER_SIZE) {
                            BinaryProtocol2 *hdr = (BinaryProtocol2 *)recv_buf;
                            payload_len = ntohl(hdr->payload_size);
                            payload = (const uint8_t *)(recv_buf + BP2_HEADER_SIZE);
                            if (payload_len > len - BP2_HEADER_SIZE)
                                payload_len = len - BP2_HEADER_SIZE;
                        } else if (version == 3 && len >= 4) {
                            payload_len = (uint8_t)recv_buf[2] << 8 | (uint8_t)recv_buf[3];
                            payload = (const uint8_t *)(recv_buf + 4);
                            if (payload_len > len - 4)
                                payload_len = len - 4;
                        } else {
                            payload = (const uint8_t *)recv_buf;
                            payload_len = len;
                        }

                        if (payload && payload_len > 0 && s_pipeline) {
                            s_pipeline->EnqueueReceivedAudio(payload, payload_len);
                        }
                    } else {
                        handle_json_message(recv_buf, len);
                    }
                } else if (len < 0) {
                    ESP_LOGW(TAG, "WebSocket disconnected during speaking");
                    disconnect_from_server();
                    set_state(XZ_ERROR);
                }
            }
            break;
        }

        case XZ_CONNECTING:
            // Should not be here, but just wait
            vTaskDelay(pdMS_TO_TICKS(100));
            break;

        case XZ_ERROR:
            ESP_LOGW(TAG, "Error state, waiting 5s before retry...");
            disconnect_from_server();
            // Brief wait so KILL has a chance to be detected (don't sleep 5s blocking)
            for (int i = 0; i < 50 && !(xEventGroupGetBits(s_events) & XZ_EVT_KILL); i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (xEventGroupGetBits(s_events) & XZ_EVT_KILL) goto exit_loop;
            set_state(XZ_IDLE);
            break;
        }
    }

exit_loop:
    ESP_LOGI(TAG, "xiaozhi_run cleanup: disconnecting from server");
    disconnect_from_server();
    // Clear KILL bit so future calls start fresh
    xEventGroupClearBits(s_events, XZ_EVT_KILL);
}

xiaozhi_state_t xiaozhi_get_state(void) {
    return s_state;
}

void xiaozhi_force_disconnect(void) {
    if (!s_events) {
        ESP_LOGW(TAG, "force_disconnect: event group not initialized");
        return;
    }
    ESP_LOGI(TAG, "force_disconnect: setting KILL bit");
    // Direct disconnect (closes WS + stops pipeline) so resources are released
    // even if the run loop is stuck somewhere
    disconnect_from_server();
    // Set KILL bit so xiaozhi_run exits its main loop
    xEventGroupSetBits(s_events, XZ_EVT_KILL);
}

extern "C" void *xiaozhi_get_i2c_bus(void) {
    return (void *)s_i2c_bus;
}

extern "C" void xiaozhi_set_speaker_mute(bool mute) {
    if (mute) {
        if (s_tx_handle) i2s_channel_disable(s_tx_handle);
        ESP_LOGI(TAG, "Speaker muted (TX disabled)");
    } else {
        if (s_tx_handle) i2s_channel_enable(s_tx_handle);
        ESP_LOGI(TAG, "Speaker unmuted (TX enabled)");
    }
}
