#include "audio_pipeline.h"

#include <esp_log.h>
#include <cstring>

static const char *TAG = "XiaoZhi Pipeline";

AudioPipeline::AudioPipeline() {}

AudioPipeline::~AudioPipeline() {
    Stop();
    if (encode_queue_) vQueueDelete(encode_queue_);
    if (send_queue_) vQueueDelete(send_queue_);
    if (decode_queue_) vQueueDelete(decode_queue_);
    if (playback_queue_) vQueueDelete(playback_queue_);
}

void AudioPipeline::SetBridgeEvents(EventGroupHandle_t events, EventBits_t audio_bit) {
    bridge_events_ = events;
    bridge_audio_bit_ = audio_bit;
}

esp_err_t AudioPipeline::Init(OpusCodec *opus, int server_sample_rate) {
    opus_ = opus;
    server_sample_rate_ = server_sample_rate;

    // Delete old queues if they exist (prevent memory leak on reconnect)
    if (encode_queue_) { vQueueDelete(encode_queue_); encode_queue_ = nullptr; }
    if (send_queue_) { vQueueDelete(send_queue_); send_queue_ = nullptr; }
    if (decode_queue_) { vQueueDelete(decode_queue_); decode_queue_ = nullptr; }
    if (playback_queue_) { vQueueDelete(playback_queue_); playback_queue_ = nullptr; }

    encode_queue_ = xQueueCreate(XZ_PCM_QUEUE_SIZE, sizeof(EncodeItem));
    send_queue_ = xQueueCreate(XZ_SEND_QUEUE_SIZE, sizeof(SendItem));
    decode_queue_ = xQueueCreate(XZ_DECODE_QUEUE_SIZE, sizeof(DecodeItem));
    playback_queue_ = xQueueCreate(XZ_PCM_QUEUE_SIZE, sizeof(PlaybackItem));

    if (!encode_queue_ || !send_queue_ || !decode_queue_ || !playback_queue_) {
        ESP_LOGE(TAG, "Failed to create audio queues");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Audio pipeline initialized (server_rate=%d)", server_sample_rate_);
    return ESP_OK;
}

int AudioPipeline::Resample24to16(const int16_t *in, int in_samples, int16_t *out, int out_max) {
    int target = in_samples * 2 / 3;
    if (target > out_max) target = out_max;

    for (int i = 0; i < target; i++) {
        float src_idx = (float)i * in_samples / target;
        int idx0 = (int)src_idx;
        int idx1 = idx0 + 1;
        if (idx1 >= in_samples) idx1 = in_samples - 1;
        float frac = src_idx - idx0;
        out[i] = (int16_t)(in[idx0] * (1.0f - frac) + in[idx1] * frac);
    }
    return target;
}

void AudioPipeline::AudioInputTask(void *arg) {
    AudioPipeline *self = (AudioPipeline *)arg;
    ESP_LOGI(TAG, "Audio input task started");

    // Read buffer: 4-channel TDM, 1440 samples * 4 channels * 2 bytes
    const int READ_SAMPLES = XZ_INPUT_FRAME_SAMPLES * 4;
    int16_t *read_buf = (int16_t *)heap_caps_malloc(READ_SAMPLES * 2, MALLOC_CAP_SPIRAM);
    if (!read_buf) {
        ESP_LOGE(TAG, "Failed to allocate input buffer");
        self->input_running_ = false;
        vTaskDelete(NULL);
        return;
    }

    int16_t *mono_buf = (int16_t *)heap_caps_malloc(XZ_INPUT_FRAME_SAMPLES * 2, MALLOC_CAP_SPIRAM);
    if (!mono_buf) {
        free(read_buf);
        self->input_running_ = false;
        vTaskDelete(NULL);
        return;
    }

    EncodeItem enc_item;

    while (self->input_running_) {
        esp_err_t ret = esp_codec_dev_read(self->input_dev_, read_buf, READ_SAMPLES * 2);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "I2S read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Deinterleave: extract channel 0 only
        for (int i = 0; i < XZ_INPUT_FRAME_SAMPLES; i++) {
            mono_buf[i] = read_buf[i * 4];
        }

        // Resample 24kHz -> 16kHz
        int resampled = self->Resample24to16(mono_buf, XZ_INPUT_FRAME_SAMPLES,
                                              enc_item.pcm, XZ_OPUS_FRAME_SAMPLES);

        // Pad if needed
        if (resampled < XZ_OPUS_FRAME_SAMPLES) {
            memset(&enc_item.pcm[resampled], 0,
                   (XZ_OPUS_FRAME_SAMPLES - resampled) * 2);
        }

        // Send to encode queue (don't block, drop if full)
        xQueueSendToBack(self->encode_queue_, &enc_item, 0);
    }

    free(read_buf);
    free(mono_buf);
    self->input_task_ = nullptr;
    vTaskDelete(NULL);
}

void AudioPipeline::OpusCodecTask(void *arg) {
    AudioPipeline *self = (AudioPipeline *)arg;
    ESP_LOGI(TAG, "Opus codec task started");

    EncodeItem enc_item;
    DecodeItem dec_item;
    uint32_t send_timestamp = 0;
    std::vector<uint8_t> opus_buf;
    std::vector<int16_t> pcm_buf;

    while (self->codec_running_) {
        bool did_work = false;

        // Process decode FIRST for lower playback latency
        if (xQueueReceive(self->decode_queue_, &dec_item, 0)) {
            pcm_buf.clear();
            if (self->opus_->Decode(dec_item.opus, dec_item.opus_len, pcm_buf)
                && !pcm_buf.empty()) {
                PlaybackItem play_item;
                int samples = pcm_buf.size();
                if (samples > XZ_INPUT_FRAME_SAMPLES) samples = XZ_INPUT_FRAME_SAMPLES;
                memcpy(play_item.pcm, pcm_buf.data(), samples * 2);
                play_item.pcm_len = (uint16_t)samples;
                xQueueSendToBack(self->playback_queue_, &play_item, pdMS_TO_TICKS(100));
            }
            did_work = true;
        }

        // Process encode (after decode for lower audio playback latency)
        if (xQueueReceive(self->encode_queue_, &enc_item, did_work ? 0 : pdMS_TO_TICKS(10))) {
            opus_buf.clear();
            if (self->opus_->Encode(enc_item.pcm, XZ_OPUS_FRAME_SAMPLES, opus_buf)
                && !opus_buf.empty()) {
                SendItem send_item;
                send_item.opus_len = (uint16_t)opus_buf.size();
                if (send_item.opus_len > XZ_OPUS_MAX_PACKET) {
                    send_item.opus_len = XZ_OPUS_MAX_PACKET;
                }
                memcpy(send_item.opus, opus_buf.data(), send_item.opus_len);
                send_item.timestamp = send_timestamp;
                send_timestamp += XZ_OPUS_FRAME_MS;
                xQueueSendToBack(self->send_queue_, &send_item, 0);

                // Signal bridge that audio is ready to send
                if (self->bridge_events_ && self->bridge_audio_bit_) {
                    xEventGroupSetBits(self->bridge_events_, self->bridge_audio_bit_);
                }
            }
            did_work = true;
        }

        if (!did_work) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    self->codec_task_ = nullptr;
    vTaskDelete(NULL);
}

void AudioPipeline::AudioOutputTask(void *arg) {
    AudioPipeline *self = (AudioPipeline *)arg;
    ESP_LOGI(TAG, "Audio output task started");

    PlaybackItem play_item;

    while (self->output_running_) {
        if (xQueueReceive(self->playback_queue_, &play_item, pdMS_TO_TICKS(100))) {
            esp_err_t ret = esp_codec_dev_write(self->output_dev_,
                                                 play_item.pcm, play_item.pcm_len * 2);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(ret));
            }
        }
    }

    self->output_task_ = nullptr;
    vTaskDelete(NULL);
}

esp_err_t AudioPipeline::Start(esp_codec_dev_handle_t input_dev, esp_codec_dev_handle_t output_dev) {
    input_dev_ = input_dev;
    output_dev_ = output_dev;

    // Start output task (always running to drain any received audio)
    output_running_ = true;
    xTaskCreatePinnedToCore(AudioOutputTask, "audio_out",
                             XZ_AUDIO_OUTPUT_STACK, this,
                             XZ_AUDIO_OUTPUT_PRIO, &output_task_, 1);

    // Start codec task
    codec_running_ = true;
    xTaskCreatePinnedToCore(OpusCodecTask, "opus_codec",
                             XZ_OPUS_CODEC_STACK, this,
                             XZ_OPUS_CODEC_PRIO, &codec_task_, 1);

    // Start input task on Core 0 to leave Core 1 free for codec + main loop
    input_running_ = true;
    xTaskCreatePinnedToCore(AudioInputTask, "audio_in",
                             XZ_AUDIO_INPUT_STACK, this,
                             XZ_AUDIO_INPUT_PRIO, &input_task_, 0);

    ESP_LOGI(TAG, "Audio pipeline tasks started");
    return ESP_OK;
}

void AudioPipeline::Stop() {
    input_running_ = false;
    output_running_ = false;
    codec_running_ = false;

    vTaskDelay(pdMS_TO_TICKS(200));

    if (input_task_) {
        vTaskDelete(input_task_);
        input_task_ = nullptr;
    }
    if (codec_task_) {
        vTaskDelete(codec_task_);
        codec_task_ = nullptr;
    }
    if (output_task_) {
        vTaskDelete(output_task_);
        output_task_ = nullptr;
    }

    if (encode_queue_) xQueueReset(encode_queue_);
    if (send_queue_) xQueueReset(send_queue_);
    if (decode_queue_) xQueueReset(decode_queue_);
    if (playback_queue_) xQueueReset(playback_queue_);
}

void AudioPipeline::EnqueueReceivedAudio(const uint8_t *opus, int len) {
    DecodeItem item;
    if (len > XZ_OPUS_MAX_PACKET) len = XZ_OPUS_MAX_PACKET;
    memcpy(item.opus, opus, len);
    item.opus_len = (uint16_t)len;
    if (xQueueSendToBack(decode_queue_, &item, 0) != pdPASS) {
        DecodeItem discard;
        xQueueReceive(decode_queue_, &discard, 0);
        xQueueSendToBack(decode_queue_, &item, 0);
    }
}

bool AudioPipeline::DequeueSendAudio(SendItem &item) {
    return xQueueReceive(send_queue_, &item, 0) == pdPASS;
}
