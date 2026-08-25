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

    // Initialize Google liblc3 encoder instance
    m_encoder = lc3_setup_encoder(m_frame_duration_us, m_sample_rate, 0, &m_encoder_mem);
    if (!m_encoder) {
        ESP_LOGE(TAG, "Failed to setup Google liblc3 encoder (%lu Hz, %lu us)!", m_sample_rate, m_frame_duration_us);
        return ESP_FAIL;
    }

    m_encoder_ready = true;
    ESP_LOGI(TAG, "Google liblc3 Encoder Initialized: %lu Hz, %u-ch, %lu us duration, %u octets/frame (%lu kbps)",
             m_sample_rate, m_channels, m_frame_duration_us, m_octets_per_frame, getBitrateKbps());
    return ESP_OK;
}

esp_err_t Lc3CodecEngine::initDecoder(uint32_t sample_rate_hz, uint8_t channels, uint32_t frame_duration_us, uint16_t octets_per_frame) {
    m_sample_rate = sample_rate_hz;
    m_channels = channels;
    m_frame_duration_us = frame_duration_us;
    m_octets_per_frame = octets_per_frame;

    // Initialize Google liblc3 decoder instance
    m_decoder = lc3_setup_decoder(m_frame_duration_us, m_sample_rate, 0, &m_decoder_mem);
    if (!m_decoder) {
        ESP_LOGE(TAG, "Failed to setup Google liblc3 decoder (%lu Hz, %lu us)!", m_sample_rate, m_frame_duration_us);
        return ESP_FAIL;
    }

    m_decoder_ready = true;
    ESP_LOGI(TAG, "Google liblc3 Decoder Initialized: %lu Hz, %u-ch, %lu us duration, %u octets/frame",
             m_sample_rate, m_channels, m_frame_duration_us, m_octets_per_frame);
    return ESP_OK;
}

uint32_t Lc3CodecEngine::getBitrateKbps() const {
    if (m_frame_duration_us == 0) return 0;
    return (static_cast<uint32_t>(m_octets_per_frame) * 8 * 1000000ULL) / (m_frame_duration_us * 1000ULL);
}

esp_err_t Lc3CodecEngine::encodeFrame(const int16_t* pcm_in, size_t pcm_samples, uint8_t* out_lc3_buf, size_t max_out_bytes, size_t* actual_out_bytes) {
    if (!m_encoder_ready || !m_encoder || !pcm_in || !out_lc3_buf || !actual_out_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (max_out_bytes < m_octets_per_frame) {
        return ESP_ERR_NO_MEM;
    }

    // Call Google liblc3 encoder: 16-bit PCM -> LC3 bitstream
    int res = lc3_encode(m_encoder, LC3_PCM_FORMAT_S16, pcm_in, 1, m_octets_per_frame, out_lc3_buf);
    if (res < 0) {
        ESP_LOGE(TAG, "lc3_encode error: %d", res);
        return ESP_FAIL;
    }

    *actual_out_bytes = m_octets_per_frame;
    return ESP_OK;
}

esp_err_t Lc3CodecEngine::decodeFrame(const uint8_t* in_lc3_buf, size_t in_bytes, int16_t* pcm_out, size_t max_pcm_samples, size_t* actual_pcm_samples) {
    if (!m_decoder_ready || !m_decoder || !pcm_out || !actual_pcm_samples) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t required_samples = lc3_frame_samples(m_frame_duration_us, m_sample_rate);
    if (required_samples <= 0) {
        required_samples = (m_sample_rate * (m_frame_duration_us / 1000)) / 1000;
    }
    if (max_pcm_samples < required_samples) {
        return ESP_ERR_NO_MEM;
    }

    // Call Google liblc3 decoder: LC3 bitstream -> 16-bit PCM (supports PLC when in_lc3_buf is NULL or in_bytes == 0)
    int res = lc3_decode(m_decoder, in_lc3_buf, (in_lc3_buf && in_bytes > 0) ? in_bytes : 0, LC3_PCM_FORMAT_S16, pcm_out, 1);
    if (res < 0) {
        // In case of corrupt frame, fallback to PLC
        lc3_decode(m_decoder, nullptr, 0, LC3_PCM_FORMAT_S16, pcm_out, 1);
    }

    *actual_pcm_samples = required_samples;
    return ESP_OK;
}

} // namespace Codec
