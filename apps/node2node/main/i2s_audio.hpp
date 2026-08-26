#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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

    // Initializes dual-descriptor DMA (2 x 10ms = 20ms capacity) for any standard sample rate
    esp_err_t init(uint32_t sample_rate_hz, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, int gain_pin = -1);
    void deinit();

    // Starts I2S hardware clock (called when first 10ms frame is buffered)
    esp_err_t start();

    // Stops I2S hardware clock (puts peripheral into idle/standby)
    esp_err_t stop();

    // Reconfigures hardware sample rate on the fly (48k, 44.1k, 32k, 24k, 22.05k, 16k, 8k)
    esp_err_t setSampleRate(uint32_t sample_rate_hz);

    // Sets hardware gain on MAX98357A via tri-state GPIO
    void setGain(Max98357Gain gain);

    // Writes 16-bit PCM frames to I2S DMA
    esp_err_t write(const int16_t* pcm_data, size_t samples_count, size_t* bytes_written, uint32_t timeout_ms = 100);

    // Interrupt-driven synchronization: waits until a DMA descriptor is empty
    bool waitForDmaSlot(uint32_t timeout_ms = 30);

    // Called from DMA on_sent ISR
    void notifyDmaSlotFreeFromISR(BaseType_t* high_task_wakeup);

    bool isInitialized() const { return m_initialized; }
    bool isRunning() const { return m_running; }
    uint32_t getSampleRate() const { return m_sample_rate; }
    uint32_t getSamplesPerFrame() const { return (m_sample_rate * 10) / 1000; }

    void incrementUnderrunCount();

    inline uint32_t getAndResetUnderrunCount() {
        return m_dma_underrun_count.exchange(0, std::memory_order_relaxed);
    }

    inline uint32_t getUnderrunCount() const {
        return m_dma_underrun_count.load(std::memory_order_relaxed);
    }

private:
    void* m_tx_chan = nullptr;
    bool m_initialized = false;
    bool m_running = false;
    uint32_t m_sample_rate = 48000;
    gpio_num_t m_bclk = GPIO_NUM_NC;
    gpio_num_t m_ws = GPIO_NUM_NC;
    gpio_num_t m_dout = GPIO_NUM_NC;
    gpio_num_t m_gain_pin = GPIO_NUM_NC;
    SemaphoreHandle_t m_dma_free_sem = nullptr;
    std::atomic<uint32_t> m_dma_underrun_count{0};
};

} // namespace Hardware
