#include "diagnostics.hpp"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include <cstdio>
#include <cstring>
#include <cmath>

static const char* TAG = "DIAGNOSTICS";

namespace Diagnostics {

SystemDiagnostics::SystemDiagnostics(AudioNet::EspNowUnicastEngine& unicast_engine,
                                     Hardware::StatusLed& status_led)
    : m_unicast_engine(unicast_engine),
      m_status_led(status_led) {
}

SystemDiagnostics::~SystemDiagnostics() {
    if (m_temp_sensor) {
        temperature_sensor_disable(m_temp_sensor);
        temperature_sensor_uninstall(m_temp_sensor);
        m_temp_sensor = nullptr;
    }
}

void SystemDiagnostics::init() {
    temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&temp_sensor_config, &m_temp_sensor);
    if (err == ESP_OK) {
        temperature_sensor_enable(m_temp_sensor);
    } else {
        ESP_LOGW(TAG, "On-chip temperature sensor init failed: %s", esp_err_to_name(err));
        m_temp_sensor = nullptr;
    }
}

static inline void format_uptime(int64_t start_time_us, char* out_buf, size_t buf_size) {
    if (start_time_us <= 0) {
        snprintf(out_buf, buf_size, "00:00");
        return;
    }
    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_sec = (now_us - start_time_us) / 1000000;
    if (elapsed_sec < 0) elapsed_sec = 0;

    uint32_t mins = static_cast<uint32_t>(elapsed_sec / 60);
    uint32_t secs = static_cast<uint32_t>(elapsed_sec % 60);
    if (mins >= 60) {
        uint32_t hours = mins / 60;
        mins %= 60;
        snprintf(out_buf, buf_size, "%02lu:%02lu:%02lu", (unsigned long)hours, (unsigned long)mins, (unsigned long)secs);
    } else {
        snprintf(out_buf, buf_size, "%02lu:%02lu", (unsigned long)mins, (unsigned long)secs);
    }
}

void SystemDiagnostics::tick() {
    m_loop_count++;
    m_unicast_engine.update10HzTimeOffsetStats();

    const system_config_t* cfg = get_system_config();

    if ((m_loop_count % 10) == 0) { // 1 Hz periodic telemetry printout
        int64_t now_diag_us = esp_timer_get_time();
        int64_t elapsed_us = (m_last_print_time_us > 0) ? (now_diag_us - m_last_print_time_us) : 1000000;
        if (elapsed_us <= 0) elapsed_us = 1000000;
        m_last_print_time_us = now_diag_us;

        const auto& stream = m_unicast_engine.getStreamTelemetry();

        float temp_c = 0.0f;
        if (m_temp_sensor) {
            temperature_sensor_get_celsius(m_temp_sensor, &temp_c);
        }

        uint32_t cpu_freq_mhz = 160;
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32)
        cpu_freq_mhz = 240;
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
        cpu_freq_mhz = 160;
#endif
        uint32_t free_heap_kb = static_cast<uint32_t>(esp_get_free_heap_size() / 1024);

        if (cfg->node_role == NODE_ROLE_SOURCE) {
            // SOURCE Telemetry
            uint32_t tx_sec = m_unicast_engine.getAndResetTxPacketsSec();
            uint32_t tx_total = m_unicast_engine.getTxPacketsTotal();
            uint32_t acks_total = m_unicast_engine.getTxAcksTotal();
            uint32_t ack_fails = m_unicast_engine.getTxAckFailsTotal();
            int peer_count = m_unicast_engine.getPeerCount();

            float codec_avg_ms = 0.0f, codec_max_ms = 0.0f;
            bool codec_has_data = false;
            m_unicast_engine.getCodecDurationStats(codec_avg_ms, codec_max_ms, codec_has_data);

            printf("\n======================== SOURCE MULTI-UNICAST TELEMETRY ========================\n");
            printf(" State: %-10s | PHY: %-15s | CH: 1 | Pwr: +9.0dBm | Temp: %4.1fC\n",
                   m_unicast_engine.getStateString(),
                   m_unicast_engine.getWifiPhyRateString(),
                   temp_c);
            printf(" CPU: %luMHz (Dual Core) | Heap: %lukB | LC3: %luHz/%ums (%uB) | Enc: %4.2fms\n",
                   (unsigned long)cpu_freq_mhz,
                   (unsigned long)free_heap_kb,
                   (unsigned long)stream.sample_rate,
                   (unsigned int)(stream.frame_duration_us / 1000),
                   (unsigned int)stream.frame_len,
                   codec_avg_ms);
            uint32_t usb_underrun = m_unicast_engine.getUsbUnderrunCount();
            uint32_t usb_overrun = m_unicast_engine.getUsbOverrunCount();
            size_t usb_q_len = m_unicast_engine.getUsbQueueLength();
            printf(" Active SINKs: %d/%d | Total TX: %lu (%lu pkt/s) | ACKs: %lu | Fails: %lu\n",
                   peer_count, MAX_UNICAST_SINKS,
                   (unsigned long)tx_total, (unsigned long)tx_sec,
                   (unsigned long)acks_total, (unsigned long)ack_fails);
            printf(" USB Ingest FIFO: %u pkts | Underrun: %lu | Overrun: %lu\n",
                   (unsigned int)usb_q_len,
                   (unsigned long)usb_underrun,
                   (unsigned long)usb_overrun);
            printf("+---------+-------------------+----+-------+--------+----------+--------+--------+\n");
            printf("| NAME    | MAC               | CH | STATE | UPTIME | SENT/TX  | ACK %%  | RSSI   |\n");
            printf("+---------+-------------------+----+-------+--------+----------+--------+--------+\n");

            for (int i = 0; i < peer_count; i++) {
                const auto* p = m_unicast_engine.getPeer(i);
                if (!p) continue;

                char uptime_str[16];
                format_uptime(p->session_start_time_us, uptime_str, sizeof(uptime_str));

                float ack_pct = 100.0f;
                uint32_t total_attempts = p->acks_received + p->ack_failures;
                if (total_attempts > 0) {
                    ack_pct = (static_cast<float>(p->acks_received) * 100.0f) / static_cast<float>(total_attempts);
                }

                const char* state_str = "DIS";
                if (!p->is_enabled || p->status == AudioNet::PeerStatus::DISABLED) {
                    state_str = "DIS";
                } else if (p->status == AudioNet::PeerStatus::OFFLINE || p->consecutive_ack_fails >= 5) {
                    state_str = "OFFL";
                } else {
                    state_str = "EN";
                }

                if (p->last_rssi <= -100) {
                    printf("| %-9s | %02X:%02X:%02X:%02X:%02X:%02X | %2u | %-5s | %-6s | %8lu | %5.1f%% |   N/A  |\n",
                           p->name,
                           p->mac[0], p->mac[1], p->mac[2], p->mac[3], p->mac[4], p->mac[5],
                           p->channel_id,
                           state_str,
                           uptime_str,
                           (unsigned long)p->packets_sent,
                           ack_pct);
                } else {
                    printf("| %-9s | %02X:%02X:%02X:%02X:%02X:%02X | %2u | %-5s | %-6s | %8lu | %5.1f%% | %3ddBm |\n",
                           p->name,
                           p->mac[0], p->mac[1], p->mac[2], p->mac[3], p->mac[4], p->mac[5],
                           p->channel_id,
                           state_str,
                           uptime_str,
                           (unsigned long)p->packets_sent,
                           ack_pct,
                           p->last_rssi);
                }
            }
            printf("+---------+-------------------+----+-------+--------+----------+--------+--------+\n");
        } else {
            // SINK Telemetry
            uint32_t rx_sec = m_unicast_engine.getAndResetRxPacketsSec();
            uint32_t rx_total = m_unicast_engine.getRxPacketsTotal();
            uint32_t fifo_underrun = m_unicast_engine.getFifoUnderrunCount();
            uint32_t dma_underrun = m_unicast_engine.getDmaUnderrunCount();
            uint32_t plc_count = m_unicast_engine.getPlcCount();
            int8_t rssi = m_unicast_engine.getLastRssi();

            float ema_offs_ms = 0.0f, rb_med_ms = 0.0f, rb_rng_ms = 0.0f;
            bool has_offset_stats = false;
            m_unicast_engine.getTimeOffsetStats(ema_offs_ms, rb_med_ms, rb_rng_ms, has_offset_stats);

            char uptime_str[16];
            format_uptime(m_unicast_engine.getSessionStartTimeUs(), uptime_str, sizeof(uptime_str));

            float rms_dbfs = m_unicast_engine.getAudioFrameRMS_dBFS();
            float peak_dbfs = m_unicast_engine.getAudioPeak_dBFS();

            uint8_t vol_u8 = m_unicast_engine.getVolume();
            float vol_db = m_unicast_engine.getTargetVolumeDb();
            char vol_str[24];
            if (vol_u8 == 0) {
                snprintf(vol_str, sizeof(vol_str), "MUTE (0/255)");
            } else {
                snprintf(vol_str, sizeof(vol_str), "%u/255 (%+5.1fdB)", vol_u8, vol_db);
            }

            printf("\n========================= SINK MULTI-UNICAST TELEMETRY =========================\n");
            printf(" Role: SINK (Node %d) | Target CH: %d (%s) | State: %-9s | Uptime: %s\n",
                   cfg->node_id,
                   m_unicast_engine.getTargetChannel(),
                   (m_unicast_engine.getTargetChannel() == 0) ? "Left" : (m_unicast_engine.getTargetChannel() == 1) ? "Right" : (m_unicast_engine.getTargetChannel() == 5) ? "Subwoofer" : "Surround",
                   m_unicast_engine.getStateString(),
                   uptime_str);
            printf(" PHY: %-15s | RSSI: %3ddBm | Temp: %4.1fC | Heap: %lukB | Slew: %4.0fdB/s\n",
                   m_unicast_engine.getWifiPhyRateString(),
                   rssi, temp_c, (unsigned long)free_heap_kb,
                   CONFIG_VOLUME_SLEW_RATE_DB_PER_SEC);
            printf(" Volume: %-18s | MAX98357A Gain: +%ddB\n",
                   vol_str, m_unicast_engine.getHardwareGainDb());
            printf(" LC3: %luHz/%ums (%uB) | RMS: %5.1f dBFS | Peak: %5.1f dBFS\n",
                   (unsigned long)stream.sample_rate,
                   (unsigned int)(stream.frame_duration_us / 1000),
                   (unsigned int)stream.frame_len,
                   rms_dbfs, peak_dbfs);
            printf(" RX: %lu pkt (%lu/s) | FIFO Underrun: %lu | DMA Underrun: %lu | PLC: %lu\n",
                   (unsigned long)rx_total, (unsigned long)rx_sec,
                   (unsigned long)fifo_underrun, (unsigned long)dma_underrun, (unsigned long)plc_count);
            printf(" Clock Sync: EMA_offs: %+6.2fms | RB_med: %+6.2fms | Jitter_rng: %5.2fms\n",
                   ema_offs_ms, rb_med_ms, rb_rng_ms);
            printf("================================================================================\n");
        }
    }
}

} // namespace Diagnostics
