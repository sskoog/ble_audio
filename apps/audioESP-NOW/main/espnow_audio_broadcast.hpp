#pragma once

#include "lc3_codec.hpp"
#include "tone_generator.hpp"
#include "i2s_audio.hpp"
#include "audio_metering.hpp"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/portmacro.h"
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <atomic>

namespace AudioNet {

// VSAF Protocol Packet Definition (Dual-Frame Redundant Payload with Recipient Tag)
struct EspNowAudioPacket {
    uint16_t magic;          // 0xE501 (VSAF Magic)
    uint8_t  target_node_id; // Recipient Node ID (0xFF = broadcast to all SINKs, or specific ID e.g. 23)
    uint8_t  source_node_id; // Sender Node ID (e.g. 21)
    uint8_t  seq;            // Sequence number (0-255)
    uint8_t  flags;          // Bit 0: Muted, Bit 1: Stereo/Mono
    uint16_t frame_len;      // Octets per LC3 frame (e.g. 80)
    uint8_t  prev_frame[80]; // Redundant Frame N-1 (Dual-frame redundancy)
    uint8_t  curr_frame[80]; // Frame N
} __attribute__((packed));

enum class NetworkState {
    OFF,
    SCANNING,
    CONNECTED,
    BROADCASTING,
    STREAMING
};

struct StreamTelemetry {
    uint32_t sample_rate = 32000;
    uint8_t  channels = 1;
    uint32_t bitrate_kbps = 64;
    int8_t   rssi_dbm = -26;
    bool     is_muted = false;
    uint8_t  volume_percent = 5; // 5% whisper-quiet volume

    std::string getStatusString() const {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s 16-bit %.1f kHz",
                 (channels == 1) ? "Mono" : "Stereo", sample_rate / 1000.0f);
        return std::string(buf);
    }

    std::string getCodecString() const {
        char buf[64];
        snprintf(buf, sizeof(buf), "LC3 fixp @ %lu kbps", (unsigned long)bitrate_kbps);
        return std::string(buf);
    }
};

class EspNowAudioBroadcast {
public:
    EspNowAudioBroadcast(Codec::Lc3CodecEngine& lc3_codec,
                         Audio::ToneGenerator* tone_gen = nullptr,
                         Hardware::I2sAudioDriver* i2s_dac = nullptr);
    ~EspNowAudioBroadcast();

    esp_err_t init(uint8_t node_role);
    esp_err_t enableWifiEspNow();
    esp_err_t startAudioTask();

    void onPacketReceived(const uint8_t* mac_addr, const uint8_t* data, int data_len);
    void transitionTo(NetworkState new_state);

    NetworkState getState() const { return m_state; }
    const char* getStateString() const;
    const StreamTelemetry& getStreamTelemetry() const { return m_telemetry; }

    int16_t getAudioFrameRMS_int16() { return m_audio_meter.getAudioFrameRMS_int16(); }
    int16_t getAudioPeak_int16() { return m_audio_meter.getAudioFramePeak_int16(); }
    int16_t getAudioFramePeak_int16() { return m_audio_meter.getAudioFramePeak_int16(); }
    float getAudioFrameRMS_dBFS() { return m_audio_meter.getAudioFrameRMS_dBFS(); }
    float getAudioPeak_dBFS() { return m_audio_meter.getAudioFramePeak_dBFS(); }
    float getAudioFramePeak_dBFS() { return m_audio_meter.getAudioFramePeak_dBFS(); }

    uint32_t getTxPacketsTotal() const { return m_tx_packets_total.load(std::memory_order_relaxed); }
    uint32_t getAndResetTxPacketsSec() { return m_tx_packets_sec.exchange(0, std::memory_order_relaxed); }

    uint32_t getRxPacketsTotal() const { return m_rx_packets_total.load(std::memory_order_relaxed); }
    uint32_t getAndResetRxPacketsSec() { return m_rx_packets_sec.exchange(0, std::memory_order_relaxed); }

    uint32_t getFifoOverflowCount() const { return m_fifo_overflow.load(std::memory_order_relaxed); }
    uint32_t getFifoUnderrunCount() const { return m_fifo_underrun.load(std::memory_order_relaxed); }
    uint32_t getAndResetFifoOverflowCount() { return m_fifo_overflow.exchange(0, std::memory_order_relaxed); }
    uint32_t getAndResetFifoUnderrunCount() { return m_fifo_underrun.exchange(0, std::memory_order_relaxed); }

    uint32_t getAndResetDmaUnderrunCount() { return m_i2s_dac ? m_i2s_dac->getAndResetUnderrunCount() : 0; }
    uint8_t getHardwareGainDb() const { return m_i2s_dac ? m_i2s_dac->getHardwareGainDb() : 0; }
    void setHardwareGain(Hardware::Max98357Gain gain) { if (m_i2s_dac) m_i2s_dac->setHardwareGain(gain); }
    uint32_t getAndResetPlcCount() { return m_lc3_codec.getAndResetPlcCount(); }

private:
    static void audioTaskRoutine(void* pvParameters);
    void runSourceLoop();
    void runSinkLoop();

    Codec::Lc3CodecEngine& m_lc3_codec;
    Audio::ToneGenerator* m_tone_gen;
    Hardware::I2sAudioDriver* m_i2s_dac;
    AudioMetering::AudioSignalMeter m_audio_meter;

    uint8_t m_node_role = 0;
    uint8_t m_node_id = 0;
    NetworkState m_state = NetworkState::OFF;
    StreamTelemetry m_telemetry;
    bool m_audio_task_running = false;
    bool m_wifi_initialized = false;

    // Sequence tracking & deduplication
    uint8_t m_last_rx_seq = 0;
    bool m_has_last_rx_seq = false;

    // Real-time packet throughput counters
    std::atomic<uint32_t> m_tx_packets_total{0};
    std::atomic<uint32_t> m_tx_packets_sec{0};
    std::atomic<uint32_t> m_rx_packets_total{0};
    std::atomic<uint32_t> m_rx_packets_sec{0};
    std::atomic<uint32_t> m_fifo_overflow{0};
    std::atomic<uint32_t> m_fifo_underrun{0};
};

} // namespace AudioNet
