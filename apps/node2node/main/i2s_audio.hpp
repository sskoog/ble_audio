#pragma once

#include <cstdint>
#include <cstddef>
#include "esp_err.h"
#include "driver/gpio.h"

namespace Hardware {

enum Max98357Gain {
    GAIN_6DB,   // GPIO Output LOW
    GAIN_9DB,   // GPIO Input (High-Z Float)
    GAIN_12DB   // GPIO Output HIGH
};

class I2sAudioDriver {
public:
    I2sAudioDriver();
    ~I2sAudioDriver();

    esp_err_t init(uint32_t sample_rate_hz, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, int gain_pin = -1);
    void deinit();

    // Sets hardware gain on MAX98357A via tri-state GPIO
    void setGain(Max98357Gain gain);

    // Writes 16-bit PCM frames to I2S DMA
    esp_err_t write(const int16_t* pcm_data, size_t samples_count, size_t* bytes_written, uint32_t timeout_ms = 100);

    bool isInitialized() const { return m_initialized; }

private:
    void* m_tx_chan = nullptr;
    bool m_initialized = false;
    uint32_t m_sample_rate = 44100;
    gpio_num_t m_gain_pin = GPIO_NUM_NC;
};

} // namespace Hardware
