#include "diagnostics.hpp"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_clk_tree.h"
#include <cmath>

static const char* TAG = "DIAGNOSTICS";

namespace Diagnostics {

SystemDiagnostics::SystemDiagnostics(AudioNet::EspNowAudioBroadcast& espnow_broadcast, Hardware::StatusLed& status_led)
    : m_espnow_broadcast(espnow_broadcast), m_status_led(status_led) {}

SystemDiagnostics::~SystemDiagnostics() {
    if (m_temp_sensor) {
        temperature_sensor_disable(m_temp_sensor);
        temperature_sensor_uninstall(m_temp_sensor);
        m_temp_sensor = nullptr;
    }
}

void SystemDiagnostics::init() {
    temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    esp_err_t ret = temperature_sensor_install(&temp_sensor_config, &m_temp_sensor);
    if (ret == ESP_OK) {
        temperature_sensor_enable(m_temp_sensor);
    }
}

void SystemDiagnostics::tick() {
    m_loop_count++;

    const system_config_t* cfg = get_system_config();

    if (cfg->node_role == NODE_ROLE_SOURCE) {
        const auto& stream = m_espnow_broadcast.getStreamTelemetry();
        bool is_broadcasting = (m_espnow_broadcast.getState() == AudioNet::NetworkState::BROADCASTING) && !stream.is_muted;

        if (is_broadcasting) {
            m_status_led.setPattern(Hardware::LED_COLOR_BLUE, Hardware::DEFAULT_LED_BRIGHTNESS, Hardware::BLINK_FAST);
        } else {
            m_status_led.setPattern(Hardware::LED_COLOR_BLUE, Hardware::DEFAULT_LED_BRIGHTNESS, Hardware::BLINK_SLOW);
        }
    }

    if ((m_loop_count % 10) == 0) { // 1 Hz periodic telemetry printout
        const auto& stream = m_espnow_broadcast.getStreamTelemetry();
        uint32_t free_heap = esp_get_free_heap_size();

        float temp_c = 0.0f;
        if (m_temp_sensor) {
            temperature_sensor_get_celsius(m_temp_sensor, &temp_c);
        }

        uint32_t cpu_freq_mhz = 160;
        esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_APPROX, (uint32_t*)&cpu_freq_mhz);
        cpu_freq_mhz /= 1000000;

        uint32_t dma_udr = m_espnow_broadcast.getAndResetDmaUnderrunCount();
        uint32_t plc_count = m_espnow_broadcast.getAndResetPlcCount();
        uint32_t fifo_ud = m_espnow_broadcast.getAndResetFifoUnderrunCount();
        uint32_t fifo_ov = m_espnow_broadcast.getAndResetFifoOverflowCount();

        // Only SINK triggers Red underrun flash
        if (cfg->node_role == NODE_ROLE_SINK && (dma_udr > 0 || plc_count > 0 || fifo_ud > 0)) {
            m_status_led.triggerUnderrunFlash(200);
        }

        float rms_db = (m_espnow_broadcast.getState() == AudioNet::NetworkState::STREAMING ||
                        m_espnow_broadcast.getState() == AudioNet::NetworkState::BROADCASTING)
                       ? m_espnow_broadcast.getAudioFrameRMS_dBFS() : -INFINITY;
        float peak_db = (m_espnow_broadcast.getState() == AudioNet::NetworkState::STREAMING ||
                         m_espnow_broadcast.getState() == AudioNet::NetworkState::BROADCASTING)
                        ? m_espnow_broadcast.getAudioFramePeak_dBFS() : -INFINITY;

        char rms_str[32], peak_str[32];
        if (std::isinf(rms_db) || rms_db <= -95.0f) snprintf(rms_str, sizeof(rms_str), "-inf");
        else snprintf(rms_str, sizeof(rms_str), "%.1f", rms_db);

        if (std::isinf(peak_db) || peak_db <= -95.0f) snprintf(peak_str, sizeof(peak_str), "-inf");
        else snprintf(peak_str, sizeof(peak_str), "%.1f", peak_db);

        uint32_t tx_pps = m_espnow_broadcast.getAndResetTxPacketsSec();
        uint32_t tx_tot = m_espnow_broadcast.getTxPacketsTotal();
        uint32_t rx_pps = m_espnow_broadcast.getAndResetRxPacketsSec();
        uint32_t rx_tot = m_espnow_broadcast.getRxPacketsTotal();

        ESP_LOGI("", "========== [%s] ==========", cfg->device_name);
        ESP_LOGI("[SYS]", "CPU %d-%d%% @ %lu MHz | Temp %d C | Heap %lu KB",
                 m_cpu_mean_pct, m_cpu_peak_pct, (unsigned long)cpu_freq_mhz, (int)(temp_c + 0.5f), (unsigned long)(free_heap / 1024));

        if (cfg->node_role == NODE_ROLE_SOURCE) {
            ESP_LOGI("[ESPNOW]", "SOURCE | %s CH 1 | %lu total pkts (%lu pkts/s) | Peer: FF:FF:FF:FF:FF:FF",
                     m_espnow_broadcast.getStateString(), (unsigned long)tx_tot, (unsigned long)tx_pps);
        } else {
            ESP_LOGI("[ESPNOW]", "SINK | %s CH 1 | %lu total pkts (%lu pkts/s) | RSSI %d dBm | Peer: FF:FF:FF:FF:FF:FF",
                     m_espnow_broadcast.getStateString(), (unsigned long)rx_tot, (unsigned long)rx_pps, stream.rssi_dbm);
        }

        ESP_LOGI("[AUDIO]", "RMS|Pk %s|%s dBFS | VCS %u%% | GAIN %udB | DMA_UDR %lu | FIFO_OV/UD %lu/%lu | PLC %lu | %s | %s",
                 rms_str, peak_str, stream.volume_percent, m_espnow_broadcast.getHardwareGainDb(), (unsigned long)dma_udr,
                 (unsigned long)fifo_ov, (unsigned long)fifo_ud, (unsigned long)plc_count,
                 stream.getStatusString().c_str(), stream.getCodecString().c_str());
    }
}

} // namespace Diagnostics
