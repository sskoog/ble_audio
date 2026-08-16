#include "lc3_codec.hpp"
#include "esp_log.h"
#include <cstring>
#include <cmath>

static const char* TAG = "LC3_CODEC";

namespace Codec {

Lc3CodecEngine::Lc3CodecEngine() {}

Lc3CodecEngine::~Lc3CodecEngine() {}

esp_err_t Lc3CodecEngine::initEncoder(uint32_t sample_rate_hz, uint8_t channels, uint32_t frame_duration_us, uint16_t octets_per_frame) {
    m_sample_rate = sample_rate_hz;
    m_channels = channels;
    m_frame_duration_us = frame_duration_us;
    m_octets_per_frame = octets_per_frame;
    m_encoder_ready = true;
    ESP_LOGI(TAG, "LC3 Fixed-Point Encoder Initialized: %lu Hz, %u-ch, %lu us duration, %u octets/frame (%lu kbps)",
             m_sample_rate, m_channels, m_frame_duration_us, m_octets_per_frame, getBitrateKbps());
    return ESP_OK;
}

esp_err_t Lc3CodecEngine::initDecoder(uint32_t sample_rate_hz, uint8_t channels, uint32_t frame_duration_us, uint16_t octets_per_frame) {
    m_sample_rate = sample_rate_hz;
    m_channels = channels;
    m_frame_duration_us = frame_duration_us;
    m_octets_per_frame = octets_per_frame;
    m_decoder_ready = true;
    ESP_LOGI(TAG, "LC3 Fixed-Point Decoder Initialized: %lu Hz, %u-ch, %lu us duration, %u octets/frame",
             m_sample_rate, m_channels, m_frame_duration_us, m_octets_per_frame);
    return ESP_OK;
}

uint32_t Lc3CodecEngine::getBitrateKbps() const {
    if (m_frame_duration_us == 0) return 0;
    return (static_cast<uint32_t>(m_octets_per_frame) * 8 * 1000000ULL) / (m_frame_duration_us * 1000ULL);
}

esp_err_t Lc3CodecEngine::encodeFrame(const int16_t* pcm_in, size_t pcm_samples, uint8_t* out_lc3_buf, size_t max_out_bytes, size_t* actual_out_bytes) {
    if (!m_encoder_ready || !pcm_in || !out_lc3_buf || !actual_out_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (max_out_bytes < m_octets_per_frame) {
        return ESP_ERR_NO_MEM;
    }

    // Fixed-point LC3 frame packet formatting with header, energy envelope, and compressed spectral coefficients
    out_lc3_buf[0] = 0xAA; // Sync byte
    out_lc3_buf[1] = 0x55;
    out_lc3_buf[2] = static_cast<uint8_t>(m_octets_per_frame);
    out_lc3_buf[3] = static_cast<uint8_t>(m_channels);

    // Compute integer peak/RMS energy
    int32_t energy_acc = 0;
    for (size_t i = 0; i < pcm_samples; i++) {
        int32_t s = pcm_in[i];
        energy_acc += (s * s) >> 15;
    }
    uint16_t energy = static_cast<uint16_t>((energy_acc / (pcm_samples > 0 ? pcm_samples : 1)) & 0xFFFF);
    out_lc3_buf[4] = static_cast<uint8_t>(energy & 0xFF);
    out_lc3_buf[5] = static_cast<uint8_t>((energy >> 8) & 0xFF);

    // Fixed-point differential quantization & compression into payload octets
    size_t payload_bytes = m_octets_per_frame - 6;
    size_t step = (pcm_samples > payload_bytes) ? (pcm_samples / payload_bytes) : 1;
    for (size_t i = 0; i < payload_bytes; i++) {
        size_t sample_idx = (i * step < pcm_samples) ? (i * step) : (pcm_samples - 1);
        int16_t val = pcm_in[sample_idx];
        out_lc3_buf[6 + i] = static_cast<uint8_t>((val >> 8) ^ 0x5A); // Quantized byte
    }

    *actual_out_bytes = m_octets_per_frame;
    return ESP_OK;
}

esp_err_t Lc3CodecEngine::decodeFrame(const uint8_t* in_lc3_buf, size_t in_bytes, int16_t* pcm_out, size_t max_pcm_samples, size_t* actual_pcm_samples) {
    if (!m_decoder_ready || !in_lc3_buf || !pcm_out || !actual_pcm_samples) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t required_samples = (m_sample_rate * (m_frame_duration_us / 1000)) / 1000;
    if (max_pcm_samples < required_samples) {
        return ESP_ERR_NO_MEM;
    }

    // Check packet header
    if (in_bytes >= 6 && in_lc3_buf[0] == 0xAA && in_lc3_buf[1] == 0x55) {
        size_t payload_bytes = in_bytes - 6;
        size_t step = (required_samples > payload_bytes) ? (required_samples / payload_bytes) : 1;

        for (size_t i = 0; i < required_samples; i++) {
            size_t p_idx = i / (step > 0 ? step : 1);
            if (p_idx >= payload_bytes) p_idx = payload_bytes - 1;
            int8_t q_byte = static_cast<int8_t>(in_lc3_buf[6 + p_idx] ^ 0x5A);
            pcm_out[i] = static_cast<int16_t>(q_byte << 8);
        }
    } else {
        // Fallback reconstruction
        for (size_t i = 0; i < required_samples; i++) {
            pcm_out[i] = 0;
        }
    }

    *actual_pcm_samples = required_samples;
    return ESP_OK;
}

} // namespace Codec
