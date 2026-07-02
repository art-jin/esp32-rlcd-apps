#ifndef XIAOZHI_CONFIG_H
#define XIAOZHI_CONFIG_H

// I2C (codec control bus)
#define XZ_I2C_SDA   13
#define XZ_I2C_SCL   14

// I2S (audio data)
#define XZ_I2S_MCLK  16
#define XZ_I2S_WS    45
#define XZ_I2S_BCLK  9
#define XZ_I2S_DIN   10   // Mic (ES7210 -> ESP32)
#define XZ_I2S_DOUT  8    // Speaker (ESP32 -> ES8311)

// Power amplifier
#define XZ_PA_PIN    46

// Audio parameters
#define XZ_SAMPLE_RATE       24000
#define XZ_BITS_PER_SAMPLE   16

// OTA / Server
#define XZ_OTA_URL           "https://api.tenclass.net/xiaozhi/ota/"
#define XZ_BOARD_NAME        "waveshare-s3-rlcd-4.2"
#define XZ_USER_AGENT        "xiaozhi-esp32/2.1.0"
#define XZ_PROTOCOL_VERSION  2

// Opus codec
#define XZ_OPUS_SAMPLE_RATE  16000
#define XZ_OPUS_FRAME_MS     60
#define XZ_OPUS_FRAME_SAMPLES 960   // 16000 * 60 / 1000
#define XZ_OPUS_MAX_PACKET   400

// Audio pipeline
#define XZ_INPUT_FRAME_SAMPLES  1440  // 24000 * 60 / 1000 (one 60ms frame at DAC rate)
#define XZ_SEND_QUEUE_SIZE      40
#define XZ_DECODE_QUEUE_SIZE    40
#define XZ_PCM_QUEUE_SIZE       2

// BOOT button
#define XZ_BOOT_BUTTON       0

// NVS namespace
#define XZ_NVS_NAMESPACE     "xz_ws"
#define XZ_NVS_KEY_URL       "url"
#define XZ_NVS_KEY_TOKEN     "token"

// Task parameters
#define XZ_AUDIO_INPUT_STACK   4096
#define XZ_AUDIO_INPUT_PRIO    8
// Was 4096 — too small for the esp_codec_dev_write → ES8311 → I2S chain.
// Stack-overflow canary tripped immediately after task start when the
// pipeline came up under CodePilot's long-press STT path. 8 KB matches
// the headroom the upstream xiaozhi-esp32 project reserves here.
#define XZ_AUDIO_OUTPUT_STACK  8192
#define XZ_AUDIO_OUTPUT_PRIO   4
#define XZ_OPUS_CODEC_STACK    32768
#define XZ_OPUS_CODEC_PRIO     2

#endif // XIAOZHI_CONFIG_H
