#include "i2s_audio.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"

static const char* TAG = "I2S_AUDIO";

namespace Hardware {

I2sAudioDriver::I2sAudioDriver() {}

I2sAudioDriver::~I2sAudioDriver() {
    deinit();
}

esp_err_t I2sAudioDriver::init(uint32_t sample_rate_hz, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout) {
    if (m_initialized) {
        deinit();
    }

    m_sample_rate = sample_rate_hz;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear = true;

    i2s_chan_handle_t tx_handle = nullptr;
    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }
    m_tx_chan = tx_handle;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S standard mode: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        m_tx_chan = nullptr;
        return ret;
    }

    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        m_tx_chan = nullptr;
        return ret;
    }

    m_initialized = true;
    ESP_LOGI(TAG, "I2S DAC Driver Initialized: Fs=%lu Hz, Mono 16-bit, BCLK=%d, WS=%d, DOUT=%d",
             m_sample_rate, static_cast<int>(bclk), static_cast<int>(ws), static_cast<int>(dout));
    return ESP_OK;
}

void I2sAudioDriver::deinit() {
    if (m_initialized && m_tx_chan) {
        auto tx_handle = static_cast<i2s_chan_handle_t>(m_tx_chan);
        i2s_channel_disable(tx_handle);
        i2s_del_channel(tx_handle);
        m_tx_chan = nullptr;
        m_initialized = false;
        ESP_LOGI(TAG, "I2S DAC Driver Deinitialized.");
    }
}

esp_err_t I2sAudioDriver::write(const int16_t* pcm_data, size_t samples_count, size_t* bytes_written, uint32_t timeout_ms) {
    if (!m_initialized || !m_tx_chan || !pcm_data) {
        return ESP_ERR_INVALID_STATE;
    }
    auto tx_handle = static_cast<i2s_chan_handle_t>(m_tx_chan);
    size_t bytes_to_write = samples_count * sizeof(int16_t);
    return i2s_channel_write(tx_handle, pcm_data, bytes_to_write, bytes_written, pdMS_TO_TICKS(timeout_ms));
}

} // namespace Hardware
