#ifndef OPUS_CODEC_H
#define OPUS_CODEC_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include "esp_err.h"
#include "opus_encoder.h"
#include "opus_decoder.h"

class OpusCodec {
public:
    OpusCodec();
    ~OpusCodec();

    /**
     * Initialize encoder (16kHz mono, 60ms frames) and decoder (configurable rate).
     */
    esp_err_t Init(int decoder_sample_rate = 24000);

    /**
     * Encode PCM to Opus.
     * pcm: 16-bit PCM at 16kHz mono, exactly XZ_OPUS_FRAME_SAMPLES samples.
     * opus: output vector (cleared and filled).
     * Returns true on success.
     */
    bool Encode(const int16_t *pcm, int pcm_samples, std::vector<uint8_t> &opus);

    /**
     * Decode Opus to PCM.
     * opus: Opus packet data.
     * opus_len: Opus packet length.
     * pcm: output vector (cleared and filled).
     * Returns true on success.
     */
    bool Decode(const uint8_t *opus, int opus_len, std::vector<int16_t> &pcm);

    int encoder_sample_rate() const { return encoder_ ? encoder_->sample_rate() : 16000; }
    int decoder_sample_rate() const { return decoder_ ? decoder_->sample_rate() : 24000; }

private:
    OpusEncoderWrapper *encoder_ = nullptr;
    OpusDecoderWrapper *decoder_ = nullptr;
    bool initialized_ = false;
};

#endif // OPUS_CODEC_H
