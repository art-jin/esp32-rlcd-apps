#ifndef AUDIO_PIPELINE_H
#define AUDIO_PIPELINE_H

#include <cstdint>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_codec_dev.h"
#include "opus_codec.h"
#include "config.h"

// Queue item types
struct EncodeItem {
    int16_t pcm[XZ_OPUS_FRAME_SAMPLES];  // 960 samples at 16kHz = 1920 bytes
};

struct SendItem {
    uint8_t opus[XZ_OPUS_MAX_PACKET];
    uint16_t opus_len;
    uint32_t timestamp;
};

struct DecodeItem {
    uint8_t opus[XZ_OPUS_MAX_PACKET];
    uint16_t opus_len;
};

struct PlaybackItem {
    int16_t pcm[XZ_INPUT_FRAME_SAMPLES];  // 1440 samples at 24kHz = 2880 bytes
    uint16_t pcm_len;  // in samples
};

class WsClient;

class AudioPipeline {
public:
    AudioPipeline();
    ~AudioPipeline();

    /**
     * Initialize pipeline: create queues and OpusCodec.
     */
    esp_err_t Init(OpusCodec *opus, int server_sample_rate);

    /**
     * Start the audio pipeline tasks.
     */
    esp_err_t Start(esp_codec_dev_handle_t input_dev, esp_codec_dev_handle_t output_dev);

    /** Stop all pipeline tasks */
    void Stop();

    /** Enqueue a received Opus packet for decoding and playback */
    void EnqueueReceivedAudio(const uint8_t *opus, int len);

    /** Dequeue an encoded Opus packet for sending (non-blocking) */
    bool DequeueSendAudio(SendItem &item);

    /**
     * Register bridge event group for signaling when encoded audio is ready.
     * The pipeline sets bridge_audio_bit on bridge_events after encoding.
     */
    void SetBridgeEvents(EventGroupHandle_t events, EventBits_t audio_bit);

    bool is_input_running() const { return input_running_; }
    bool is_output_running() const { return output_running_; }

private:
    static void AudioInputTask(void *arg);
    static void OpusCodecTask(void *arg);
    static void AudioOutputTask(void *arg);

    int Resample24to16(const int16_t *in, int in_samples, int16_t *out, int out_max);

    OpusCodec *opus_ = nullptr;
    esp_codec_dev_handle_t input_dev_ = nullptr;
    esp_codec_dev_handle_t output_dev_ = nullptr;

    QueueHandle_t encode_queue_ = nullptr;
    QueueHandle_t send_queue_ = nullptr;
    QueueHandle_t decode_queue_ = nullptr;
    QueueHandle_t playback_queue_ = nullptr;

    TaskHandle_t input_task_ = nullptr;
    TaskHandle_t codec_task_ = nullptr;
    TaskHandle_t output_task_ = nullptr;

    volatile bool input_running_ = false;
    volatile bool output_running_ = false;
    volatile bool codec_running_ = false;
    int server_sample_rate_ = 24000;

    // Bridge event signaling
    EventGroupHandle_t bridge_events_ = nullptr;
    EventBits_t bridge_audio_bit_ = 0;
};

#endif // AUDIO_PIPELINE_H
