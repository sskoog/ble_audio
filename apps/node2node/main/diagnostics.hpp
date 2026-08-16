#pragma once

#include <cstdint>
#include <cstddef>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ble_audio_broadcast.hpp"
#include "tone_generator.hpp"
#include "lcd_display.hpp"

namespace Diagnostics {

// CPU Frequency Management Helpers
float getCPUfreq_MHz();
esp_err_t setCPUfreq_MHz(int mhz);

class DiagnosticMonitor {
public:
    DiagnosticMonitor(Bluetooth::BleAudioBroadcast& ble_broadcast, 
                      Audio::ToneGenerator* tone_gen = nullptr, 
                      Hardware::LcdDisplay* lcd_display = nullptr);
    ~DiagnosticMonitor();

    esp_err_t start();
    void stop();

private:
    static void taskRoutine(void* pvParameters);
    void printDiagnostics();

    Bluetooth::BleAudioBroadcast& m_ble_broadcast;
    Audio::ToneGenerator* m_tone_gen = nullptr;
    Hardware::LcdDisplay* m_lcd_display = nullptr;

    TaskHandle_t m_task_handle = nullptr;
    bool m_is_running = false;
    uint32_t m_loop_count = 0;
};

} // namespace Diagnostics
