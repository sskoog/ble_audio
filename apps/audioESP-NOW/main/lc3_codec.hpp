#pragma once

#ifndef AUDIO_SAMPLE_RATE_HZ
#define AUDIO_SAMPLE_RATE_HZ 32000
#endif

#ifndef AUDIO_CHANNELS_NUM
#define AUDIO_CHANNELS_NUM 1
#endif

#ifndef AUDIO_FRAME_DURATION_US
#define AUDIO_FRAME_DURATION_US 10000
#endif

#ifndef AUDIO_LC3_OCTETS_PER_FRAME
#define AUDIO_LC3_OCTETS_PER_FRAME 80
#endif

#include "esp_audio_enc.h"
#include "esp_audio_dec.h"
#include "esp_lc3_enc.h"
#include "esp_lc3_dec.h"
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <atomic>

namespace Codec {

class Lc3CodecEngine {
public:
    Lc3CodecEngine();
    ~Lc3CodecEngine();

    esp_err_t initEncoder(uint32_t sample_rate_hz = AUDIO_SAMPLE_RATE_HZ, uint8_t channels = AUDIO_CHANNELS_NUM, uint32_t frame_duration_us = AUDIO_FRAME_DURATION_US, uint16_t octets_per_frame = AUDIO_LC3_OCTETS_PER_FRAME);
    esp_err_t initDecoder(uint32_t sample_rate_hz = AUDIO_SAMPLE_RATE_HZ, uint8_t channels = AUDIO_CHANNELS_NUM, uint32_t frame_duration_us = AUDIO_FRAME_DURATION_US, uint16_t octets_per_frame = AUDIO_LC3_OCTETS_PER_FRAME);

    esp_err_t reconfigureEncoder(uint32_t sample_rate_hz, uint16_t octets_per_frame);
    esp_err_t reconfigureDecoder(uint32_t sample_rate_hz, uint16_t octets_per_frame);

    esp_err_t encodeFrame(const int16_t* pcm_in, size_t pcm_samples, uint8_t* out_lc3_buf, size_t max_out_bytes, size_t* actual_out_bytes);
    esp_err_t decodeFrame(const uint8_t* in_lc3_buf, size_t in_bytes, int16_t* pcm_out, size_t max_pcm_samples, size_t* actual_pcm_samples, uint32_t stream_sample_rate = 0);

    uint32_t getSampleRate() const { return m_sample_rate; }
    uint16_t getOctetsPerFrame() const { return m_octets_per_frame; }
    uint32_t getBitrateKbps() const;
    void incrementPlcCount() { m_plc_count.fetch_add(1, std::memory_order_relaxed); }
    uint32_t getAndResetPlcCount() { return m_plc_count.exchange(0, std::memory_order_relaxed); }
    uint32_t getPlcCount() const { return m_plc_count.load(std::memory_order_relaxed); }

private:
    void* m_enc_handle = nullptr;
    void* m_dec_handle = nullptr;
    uint32_t m_sample_rate = AUDIO_SAMPLE_RATE_HZ;
    uint8_t  m_channels = AUDIO_CHANNELS_NUM;
    uint32_t m_frame_duration_us = AUDIO_FRAME_DURATION_US;
    uint16_t m_octets_per_frame = AUDIO_LC3_OCTETS_PER_FRAME;
    bool m_encoder_ready = false;
    bool m_decoder_ready = false;
    std::atomic<uint32_t> m_plc_count{0};
};

} // namespace Codec
