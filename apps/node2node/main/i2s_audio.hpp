#pragma once

#include <cstdint>
#include <cstddef>
#include "esp_err.h"
#include "driver/gpio.h"

namespace Hardware {

class I2sAudioDriver {
public:
    I2sAudioDriver();
    ~I2sAudioDriver();

    esp_err_t init(uint32_t sample_rate_hz, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout);
    void deinit();

    // Writes 16-bit PCM frames to I2S DMA
    esp_err_t write(const int16_t* pcm_data, size_t samples_count, size_t* bytes_written, uint32_t timeout_ms = 100);

    bool isInitialized() const { return m_initialized; }

private:
    void* m_tx_chan = nullptr;
    bool m_initialized = false;
    uint32_t m_sample_rate = 44100;
};

} // namespace Hardware
