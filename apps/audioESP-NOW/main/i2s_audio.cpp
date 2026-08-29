#include "i2s_audio.hpp"
#include "esp_log.h"
#include "driver/gpio.h"
#include <cstring>

static const char* TAG = "I2S_AUDIO";

namespace Hardware {

static IRAM_ATTR bool i2s_dma_tx_done_cb(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx) {
    auto* driver = static_cast<I2sAudioDriver*>(user_ctx);
    BaseType_t high_task_wakeup = pdFALSE;
    if (driver) {
        driver->notifyDmaSlotFreeFromISR(&high_task_wakeup);
    }
    return high_task_wakeup == pdTRUE;
}

I2sAudioDriver::I2sAudioDriver(int bclk_pin, int ws_pin, int dout_pin, int din_pin, int gain_pin)
    : m_bclk_pin(bclk_pin), m_ws_pin(ws_pin), m_dout_pin(dout_pin), m_din_pin(din_pin), m_gain_pin(gain_pin) {
    m_dma_free_sem = xSemaphoreCreateBinary();
}

I2sAudioDriver::~I2sAudioDriver() {
    stop();
    if (m_tx_handle) {
        i2s_del_channel(m_tx_handle);
        m_tx_handle = nullptr;
    }
    if (m_dma_free_sem) {
        vSemaphoreDelete(m_dma_free_sem);
        m_dma_free_sem = nullptr;
    }
}

void I2sAudioDriver::setHardwareGain(Max98357Gain gain) {
    if (m_gain_pin < 0) return;
    gpio_num_t pin = static_cast<gpio_num_t>(m_gain_pin);

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pin_bit_mask = (1ULL << pin);

    switch (gain) {
        case Max98357Gain::GAIN_3DB:
            // Input with Pull-Up (~45k to VDD -> 3 dB)
            io_conf.mode = GPIO_MODE_INPUT;
            io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
            io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
            gpio_config(&io_conf);
            m_current_gain = Max98357Gain::GAIN_3DB;
            ESP_LOGI(TAG, "MAX98357A Hardware GAIN set to 3 dB (Input + Pull-Up on GPIO %d)", m_gain_pin);
            break;

        case Max98357Gain::GAIN_6DB:
            // Output HIGH (Direct VDD -> 6 dB)
            io_conf.mode = GPIO_MODE_OUTPUT;
            io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
            io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
            gpio_config(&io_conf);
            gpio_set_level(pin, 1);
            m_current_gain = Max98357Gain::GAIN_6DB;
            ESP_LOGI(TAG, "MAX98357A Hardware GAIN set to 6 dB (Output HIGH on GPIO %d)", m_gain_pin);
            break;

        case Max98357Gain::GAIN_9DB:
            // High-Z Input / Floating (No pulls -> 9 dB)
            io_conf.mode = GPIO_MODE_INPUT;
            io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
            io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
            gpio_config(&io_conf);
            m_current_gain = Max98357Gain::GAIN_9DB;
            ESP_LOGI(TAG, "MAX98357A Hardware GAIN set to 9 dB (High-Z Floating on GPIO %d)", m_gain_pin);
            break;

        case Max98357Gain::GAIN_12DB:
            // Output LOW (Direct GND -> 12 dB)
            io_conf.mode = GPIO_MODE_OUTPUT;
            io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
            io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
            gpio_config(&io_conf);
            gpio_set_level(pin, 0);
            m_current_gain = Max98357Gain::GAIN_12DB;
            ESP_LOGI(TAG, "MAX98357A Hardware GAIN set to 12 dB (Output LOW on GPIO %d)", m_gain_pin);
            break;
    }
}

esp_err_t I2sAudioDriver::init(uint32_t sample_rate, uint32_t frame_duration_us, i2s_data_bit_width_t bits_per_sample, i2s_slot_mode_t slot_mode) {
    // Configure MAX98357A GAIN pin (GP0 on Waveshare Zero): Default to 3 dB (Lowest Volume)
    setHardwareGain(Max98357Gain::GAIN_3DB);

    m_sample_rate = sample_rate;
    m_frame_duration_us = (frame_duration_us == 7500) ? 7500 : 10000;

    uint32_t frame_samples = (m_sample_rate * m_frame_duration_us) / 1000000;
    if (frame_samples < 60) frame_samples = 60;

    // Dual-Descriptor DMA Configuration: Exactly 1 frame per descriptor
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 2; // Exactly 2 descriptors (Dual-descriptor ping-pong)
    chan_cfg.dma_frame_num = frame_samples; // EXACTLY 1 frame per descriptor (80 @ 8k, 160 @ 16k, 240 @ 24k, 320 @ 32k, 480 @ 48k)
    chan_cfg.auto_clear = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &m_tx_handle, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S TX channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register DMA on_sent interrupt callback
    i2s_event_callbacks_t cbs = {
        .on_recv = nullptr,
        .on_recv_q_ovf = nullptr,
        .on_sent = i2s_dma_tx_done_cb,
        .on_send_q_ovf = nullptr,
    };
    i2s_channel_register_event_callback(m_tx_handle, &cbs, this);

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT, // Strictly 16-bit slot per channel
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = 16,
            .ws_pol = false,
            .bit_shift = true,      // Standard Philips 1-bit delay
            .left_align = true,     // MSB first aligned
            .big_endian = false,
            .bit_order_lsb = false, // MSB transmitted first on wire
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = static_cast<gpio_num_t>(m_bclk_pin),
            .ws = static_cast<gpio_num_t>(m_ws_pin),
            .dout = static_cast<gpio_num_t>(m_dout_pin),
            .din = static_cast<gpio_num_t>(m_din_pin),
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(m_tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S STD mode: %s", esp_err_to_name(ret));
        return ret;
    }

    m_is_running = false; // Gated / off until dual descriptors are pre-filled

    ESP_LOGI(TAG, "I2S Philips Master Initialized: Fs=%lu Hz, Frame=%lu samples (%.1f ms), GAIN=3dB",
             (unsigned long)sample_rate, (unsigned long)frame_samples, m_frame_duration_us / 1000.0f);
    return ESP_OK;
}

esp_err_t I2sAudioDriver::reconfigureSampleRate(uint32_t sample_rate, uint32_t frame_duration_us) {
    if (sample_rate == 0) return ESP_ERR_INVALID_ARG;
    if (frame_duration_us == 0) frame_duration_us = 10000;
    if (m_sample_rate == sample_rate && m_frame_duration_us == frame_duration_us && m_tx_handle) return ESP_OK;

    uint32_t frame_samples = (sample_rate * frame_duration_us) / 1000000;
    if (frame_samples < 60) frame_samples = 60;

    stop();

    if (m_tx_handle) {
        i2s_del_channel(m_tx_handle);
        m_tx_handle = nullptr;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 2;
    chan_cfg.dma_frame_num = frame_samples;
    chan_cfg.auto_clear = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &m_tx_handle, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to re-create I2S TX channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_event_callbacks_t cbs = {
        .on_recv = nullptr,
        .on_recv_q_ovf = nullptr,
        .on_sent = i2s_dma_tx_done_cb,
        .on_send_q_ovf = nullptr,
    };
    i2s_channel_register_event_callback(m_tx_handle, &cbs, this);

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = 16,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = static_cast<gpio_num_t>(m_bclk_pin),
            .ws = static_cast<gpio_num_t>(m_ws_pin),
            .dout = static_cast<gpio_num_t>(m_dout_pin),
            .din = static_cast<gpio_num_t>(m_din_pin),
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(m_tx_handle, &std_cfg);
    if (ret == ESP_OK) {
        m_sample_rate = sample_rate;
        m_frame_duration_us = frame_duration_us;
        ESP_LOGI(TAG, "I2S DMA & Clock reconfigured: Fs=%lu Hz, Frame=%lu samples (%.1f ms DMA buffer)",
                 (unsigned long)sample_rate, (unsigned long)frame_samples, frame_duration_us / 1000.0f);
    } else {
        ESP_LOGE(TAG, "Failed to re-init I2S std mode: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t I2sAudioDriver::start() {
    if (!m_tx_handle || m_is_running) return ESP_OK;
    if (m_dma_free_sem) {
        xSemaphoreTake(m_dma_free_sem, 0); // Clear any stale tokens
    }
    esp_err_t ret = i2s_channel_enable(m_tx_handle);
    if (ret == ESP_OK) {
        m_is_running = true;
        ESP_LOGI(TAG, "I2S Hardware Clock Started (Playing preloaded DMA descriptors).");
    }
    return ret;
}

esp_err_t I2sAudioDriver::stop() {
    if (!m_tx_handle || !m_is_running) return ESP_OK;
    esp_err_t ret = i2s_channel_disable(m_tx_handle);
    if (ret == ESP_OK) {
        m_is_running = false;
        if (m_dma_free_sem) {
            xSemaphoreTake(m_dma_free_sem, 0);
        }
        ESP_LOGI(TAG, "I2S Hardware Clock Stopped (Standby / Idle).");
    }
    return ret;
}

esp_err_t I2sAudioDriver::preload(const void* src, size_t size, size_t* bytes_written) {
    if (!m_tx_handle || !src) return ESP_ERR_INVALID_STATE;
    return i2s_channel_preload_data(m_tx_handle, src, size, bytes_written);
}

esp_err_t I2sAudioDriver::write(const void* src, size_t size, size_t* bytes_written, uint32_t timeout_ms) {
    if (!m_tx_handle || !src) return ESP_ERR_INVALID_STATE;
    if (!m_is_running) {
        start();
    }
    esp_err_t ret = i2s_channel_write(m_tx_handle, src, size, bytes_written, pdMS_TO_TICKS(timeout_ms));
    if (ret != ESP_OK) {
        m_underrun_count.fetch_add(1, std::memory_order_relaxed);
    }
    return ret;
}

bool I2sAudioDriver::waitForDmaSlot(uint32_t timeout_ms) {
    if (!m_dma_free_sem) return false;
    return xSemaphoreTake(m_dma_free_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void I2sAudioDriver::notifyDmaSlotFreeFromISR(BaseType_t* high_task_wakeup) {
    if (m_dma_free_sem) {
        xSemaphoreGiveFromISR(m_dma_free_sem, high_task_wakeup);
    }
}

} // namespace Hardware
