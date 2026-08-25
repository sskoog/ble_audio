#pragma once

#include <cstdint>
#include <cstddef>
#include "esp_err.h"
#include "config.h"
#include "lc3.h"

namespace Codec {

class Lc3CodecEngine {
public:
    Lc3CodecEngine();
    ~Lc3CodecEngine();

    esp_err_t initEncoder(uint32_t sample_rate_hz, uint8_t channels, uint32_t frame_duration_us, uint16_t octets_per_frame);
    esp_err_t initDecoder(uint32_t sample_rate_hz, uint8_t channels, uint32_t frame_duration_us, uint16_t octets_per_frame);

    // Encode 16-bit PCM buffer to standard Google LC3 compressed bitstream octets
    esp_err_t encodeFrame(const int16_t* pcm_in, size_t pcm_samples, uint8_t* out_lc3_buf, size_t max_out_bytes, size_t* actual_out_bytes);

    // Decode standard Google LC3 compressed bitstream octets to 16-bit PCM buffer (supports PLC when in_lc3_buf is NULL)
    esp_err_t decodeFrame(const uint8_t* in_lc3_buf, size_t in_bytes, int16_t* pcm_out, size_t max_pcm_samples, size_t* actual_pcm_samples);

    uint32_t getSampleRate() const { return m_sample_rate; }
    uint8_t  getChannels() const { return m_channels; }
    uint16_t getOctetsPerFrame() const { return m_octets_per_frame; }
    uint32_t getBitrateKbps() const;

private:
    uint32_t m_sample_rate = AUDIO_SAMPLE_RATE_HZ;
    uint8_t  m_channels = AUDIO_CHANNELS_NUM;
    uint32_t m_frame_duration_us = 10000;
    uint16_t m_octets_per_frame = AUDIO_LC3_OCTETS_PER_FRAME;

    bool m_encoder_ready = false;
    bool m_decoder_ready = false;

    lc3_encoder_mem_48k_t m_encoder_mem;
    lc3_encoder_t         m_encoder = nullptr;

    lc3_decoder_mem_48k_t m_decoder_mem;
    lc3_decoder_t         m_decoder = nullptr;
};

} // namespace Codec
