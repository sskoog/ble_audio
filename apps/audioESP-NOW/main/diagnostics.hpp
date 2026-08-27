#pragma once

#include "espnow_audio_broadcast.hpp"
#include "status_led.hpp"
#include "esp_timer.h"
#include "driver/temperature_sensor.h"
#include <stdint.h>

namespace Diagnostics {

class SystemDiagnostics {
public:
    SystemDiagnostics(AudioNet::EspNowAudioBroadcast& espnow_broadcast, Hardware::StatusLed& status_led);
    ~SystemDiagnostics();

    void init();
    void tick();

private:
    AudioNet::EspNowAudioBroadcast& m_espnow_broadcast;
    Hardware::StatusLed& m_status_led;

    temperature_sensor_handle_t m_temp_sensor = nullptr;
    uint32_t m_loop_count = 0;
    int m_cpu_mean_pct = 2;
    int m_cpu_peak_pct = 4;
};

} // namespace Diagnostics
