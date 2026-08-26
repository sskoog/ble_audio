#include "status_led.hpp"
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

esp_err_t I2sAudioDriver::init(uint32_t sample_rate_hz, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, int gain_pin) {
    if (m_initialized) {
        deinit();
    }

    m_sample_rate = sample_rate_hz;

    // Configure Hardware Gain pin if assigned (e.g. GP0 on Waveshare Zero)
    if (gain_pin >= 0) {
        m_gain_pin = static_cast<gpio_num_t>(gain_pin);
        setGain(GAIN_12DB); // Default to 12 dB gain for speaker
        ESP_LOGI(TAG, "Configured MAX98357A GAIN pin on GPIO %d (Set to 12 dB)", gain_pin);
    } else {
        m_gain_pin = GPIO_NUM_NC;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = (sample_rate_hz * 10) / 1000; // Exactly 10 ms (480 samples @ 48kHz)
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
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
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

void I2sAudioDriver::setGain(Max98357Gain gain) {
    if (m_gain_pin == GPIO_NUM_NC) return;

    if (gain == GAIN_9DB) {
        // High-Z Float -> 9 dB
        gpio_set_direction(m_gain_pin, GPIO_MODE_INPUT);
        gpio_pullup_dis(m_gain_pin);
        gpio_pulldown_dis(m_gain_pin);
        ESP_LOGI(TAG, "MAX98357A Hardware Gain set to 9 dB (Floating)");
    } else if (gain == GAIN_6DB) {
        // Output LOW -> 6 dB
        gpio_set_direction(m_gain_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(m_gain_pin, 0);
        ESP_LOGI(TAG, "MAX98357A Hardware Gain set to 6 dB (GND)");
    } else if (gain == GAIN_12DB) {
        // Output HIGH -> 12 dB
        gpio_set_direction(m_gain_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(m_gain_pin, 1);
        ESP_LOGI(TAG, "MAX98357A Hardware Gain set to 12 dB (VDD)");
    }
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
    esp_err_t ret = i2s_channel_write(tx_handle, pcm_data, bytes_to_write, bytes_written, pdMS_TO_TICKS(timeout_ms));
    if (ret != ESP_OK) {
        incrementUnderrunCount();
    }
    return ret;
}

void I2sAudioDriver::incrementUnderrunCount() {
    m_dma_underrun_count.fetch_add(1, std::memory_order_relaxed);
    getStatusLed().triggerUnderrunFlash(200);
}

} // namespace Hardware
