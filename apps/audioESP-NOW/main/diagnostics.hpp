#pragma once

#include "espnow_audio_broadcast.hpp"
#include "status_led.hpp"
#include "driver/temperature_sensor.h"
#include <cstdint>

namespace Diagnostics {

class SystemDiagnostics {
public:
    SystemDiagnostics(AudioNet::EspNowAudioBroadcast& espnow_broadcast, Hardware::StatusLed& status_led);
    ~SystemDiagnostics();

    void init();
    void tick();

private:
    AudioNet::EspNowAudioBroadcast& m_espnow_broadcast;
    Hardware::StatusLed&            m_status_led;
    temperature_sensor_handle_t     m_temp_sensor = nullptr;

    uint32_t                        m_loop_count = 0;
    int                             m_cpu_pct = 0;
};

} // namespace Diagnostics
