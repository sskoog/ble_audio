#include "diagnostics.hpp"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_clk_tree.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>
#include <cstring>

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
    } else { // SINK
        AudioNet::NetworkState state = m_espnow_broadcast.getState();
        switch (state) {
            case AudioNet::NetworkState::OFF:
                m_status_led.off();
                break;
            case AudioNet::NetworkState::IDLE:
                m_status_led.setPattern(Hardware::LED_COLOR_TEAL, Hardware::DEFAULT_LED_BRIGHTNESS, Hardware::BLINK_SLOW);
                break;
            case AudioNet::NetworkState::SCANNING:
                m_status_led.setPattern(Hardware::LED_COLOR_BLUE, Hardware::DEFAULT_LED_BRIGHTNESS, Hardware::BLINK_SLOW);
                break;
            case AudioNet::NetworkState::PREFILL:
                m_status_led.setPattern(Hardware::LED_COLOR_TEAL, Hardware::DEFAULT_LED_BRIGHTNESS, Hardware::BLINK_FAST);
                break;
            case AudioNet::NetworkState::STREAMING:
                m_status_led.setPattern(Hardware::LED_COLOR_GREEN, Hardware::DEFAULT_LED_BRIGHTNESS, Hardware::BLINK_FAST);
                break;
            case AudioNet::NetworkState::BROADCASTING:
                m_status_led.setPattern(Hardware::LED_COLOR_BLUE, Hardware::DEFAULT_LED_BRIGHTNESS, Hardware::BLINK_FAST);
                break;
            default:
                break;
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

        // Approximate CPU load based on node role and active state
        int cpu_load_pct = 12;
        if (cfg->node_role == NODE_ROLE_SOURCE) {
            cpu_load_pct = m_espnow_broadcast.isUsbStreamActive() ? 3 : 18;
        }

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
        if (std::isinf(rms_db) || rms_db <= -95.0f) snprintf(rms_str, sizeof(rms_str), "-inf ");
        else snprintf(rms_str, sizeof(rms_str), "%.1f", rms_db);

        if (std::isinf(peak_db) || peak_db <= -95.0f) snprintf(peak_str, sizeof(peak_str), "-inf ");
        else snprintf(peak_str, sizeof(peak_str), "%.1f", peak_db);

        uint32_t tx_pps = m_espnow_broadcast.getAndResetTxPacketsSec();
        uint32_t tx_tot = m_espnow_broadcast.getTxPacketsTotal();
        uint32_t rx_pps = m_espnow_broadcast.getAndResetRxPacketsSec();
        uint32_t rx_tot = m_espnow_broadcast.getRxPacketsTotal();

        uint64_t master_time_ms = m_espnow_broadcast.getMasterTimeMs();

        ESP_LOGI("", "========== [%s] ==========", cfg->device_name);
        ESP_LOGI("[SYS]", "CPU %d%% @ %lu MHz | Temp %d C | Heap %lu KB | MasterTime %llu ms",
                 cpu_load_pct, (unsigned long)cpu_freq_mhz, (int)(temp_c + 0.5f), (unsigned long)(free_heap / 1024),
                 (unsigned long long)master_time_ms);

        if (cfg->node_role == NODE_ROLE_SOURCE) {
            ESP_LOGI("[ESPNOW]", "SOURCE | %s [%s] | %lu total pkts (%lu pkts/s) | Magic 0x%04X",
                     m_espnow_broadcast.getStateString(),
                     m_espnow_broadcast.isUsbStreamActive() ? "USB Stream" : "Internal Synth",
                     (unsigned long)tx_tot, (unsigned long)tx_pps,
                     m_espnow_broadcast.getTestMagicWord());
        } else {
            ESP_LOGI("[ESPNOW]", "SINK | %s [Ch %u] | %lu total pkts (%lu pkts/s) | RSSI %d dBm | SyncAdj %lu | Magic 0x%04X",
                     m_espnow_broadcast.getStateString(),
                     m_espnow_broadcast.getTargetChannel(),
                     (unsigned long)rx_tot, (unsigned long)rx_pps,
                     stream.rssi_dbm, (unsigned long)m_espnow_broadcast.getClockSyncAdjustCount(),
                     m_espnow_broadcast.getTestMagicWord());
        }

        ESP_LOGI("[AUDIO]", "RMS|Pk %s|%s dBFS | DMA_UDR %lu | FIFO_OV/UD %lu/%lu | PLC %lu | PREV_REC %lu | %s",
                 rms_str, peak_str, (unsigned long)dma_udr,
                 (unsigned long)fifo_ov, (unsigned long)fifo_ud, (unsigned long)plc_count,
                 (unsigned long)m_espnow_broadcast.getPrevFrameRecoveryCount(),
                 stream.getStatusString().c_str());
    }
}

} // namespace Diagnostics
