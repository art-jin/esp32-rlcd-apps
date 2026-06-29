#include "opus_codec.h"
#include "config.h"

#include <esp_log.h>
#include <cstring>

static const char *TAG = "XiaoZhi Opus";

OpusCodec::OpusCodec() {}

OpusCodec::~OpusCodec() {
    delete encoder_;
    delete decoder_;
}

esp_err_t OpusCodec::Init(int decoder_sample_rate) {
    // Create encoder: 16kHz mono, 60ms frames
    encoder_ = new OpusEncoderWrapper(XZ_OPUS_SAMPLE_RATE, 1, XZ_OPUS_FRAME_MS);
    encoder_->SetComplexity(0);

    // Create decoder: server sample rate, 60ms frames
    decoder_ = new OpusDecoderWrapper(decoder_sample_rate, 1, XZ_OPUS_FRAME_MS);

    initialized_ = true;
    ESP_LOGI(TAG, "Opus initialized: enc=%dHz, dec=%dHz, frame=%dms",
             XZ_OPUS_SAMPLE_RATE, decoder_sample_rate, XZ_OPUS_FRAME_MS);
    return ESP_OK;
}

bool OpusCodec::Encode(const int16_t *pcm, int pcm_samples, std::vector<uint8_t> &opus) {
    if (!initialized_ || !encoder_) return false;

    // Copy PCM into a vector for the wrapper
    std::vector<int16_t> pcm_vec(pcm, pcm + pcm_samples);
    return encoder_->Encode(std::move(pcm_vec), opus);
}

bool OpusCodec::Decode(const uint8_t *opus, int opus_len, std::vector<int16_t> &pcm) {
    if (!initialized_ || !decoder_) return false;

    std::vector<uint8_t> opus_vec(opus, opus + opus_len);
    return decoder_->Decode(std::move(opus_vec), pcm);
}
