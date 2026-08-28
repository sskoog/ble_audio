#pragma once

#include "lc3_codec.hpp"
#include "tone_generator.hpp"
#include "i2s_audio.hpp"
#include "audio_metering.hpp"
#include "config.h"
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

#ifndef ESPNOW_PREFILL_THRESHOLD_FRAMES_DEFAULT
#ifdef CONFIG_ESPNOW_PREFILL_THRESHOLD_FRAMES
#define ESPNOW_PREFILL_THRESHOLD_FRAMES_DEFAULT CONFIG_ESPNOW_PREFILL_THRESHOLD_FRAMES
#else
#define ESPNOW_PREFILL_THRESHOLD_FRAMES_DEFAULT 5
#endif
#endif

#ifndef ESPNOW_WATCHDOG_TIMEOUT_FRAMES_DEFAULT
#ifdef CONFIG_ESPNOW_WATCHDOG_TIMEOUT_FRAMES
#define ESPNOW_WATCHDOG_TIMEOUT_FRAMES_DEFAULT CONFIG_ESPNOW_WATCHDOG_TIMEOUT_FRAMES
#else
#define ESPNOW_WATCHDOG_TIMEOUT_FRAMES_DEFAULT 5
#endif
#endif

namespace AudioNet {

// VSAF Protocol Packet Definition (Dual-Frame Redundant Payload with Dynamic Multi-Rate Header)
struct EspNowAudioPacket {
    uint16_t magic;          // 0xE501 (VSAF Magic)
    uint8_t  target_node_id; // Recipient Node ID (0xFF = broadcast to all SINKs, or specific ID e.g. 23)
    uint8_t  source_node_id; // Sender Node ID (e.g. 21)
    uint8_t  seq;            // Sequence number (0-255)
    uint8_t  flags;          // Bit 0: Muted, Bit 1: Stereo/Mono
    uint16_t frame_len;      // Octets per LC3 frame (e.g. 40..120)
    uint16_t sample_rate_hz; // Sample rate in Hz (16000, 24000, 32000, 44100, 48000)
    uint8_t  curr_frame[120];// Primary Frame N (Offsets 10..129)
    uint8_t  prev_frame[120];// Redundant Frame N-1 (Offsets 130..249)
} __attribute__((packed));

static constexpr size_t VSAF_HEADER_LEN = 10;

enum class NetworkState {
    OFF,           // WiFi disabled
    IDLE,          // WiFi on, passive, sound off/muted
    SCANNING,      // WiFi on, actively searching for audio stream
    PREFILL,       // Found audio stream, decoding the first LC3-frames
    STREAMING,     // Locked to WiFi audio stream, I2S clocks running, continously decoding LC3
    BROADCASTING   // Transmitting audio stream over ESP-NOW (SOURCE node)
};

struct StreamTelemetry {
    uint32_t sample_rate = CONFIG_ESPNOW_SAMPLE_RATE_HZ;
    uint8_t  channels = 1;
    uint32_t bitrate_kbps = (CONFIG_ESPNOW_FRAME_LEN_OCTETS * 8) / 10;
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

    // On-the-fly audio stream reconfiguration (SOURCE node)
    esp_err_t setAudioConfig(uint32_t sample_rate_hz, uint16_t frame_len_octets);
    esp_err_t setSampleRate(uint32_t sample_rate_hz);
    esp_err_t setFrameLen(uint16_t frame_len_octets);
    uint32_t getSampleRate() const { return m_telemetry.sample_rate; }
    uint16_t getFrameLen() const { return m_octets_per_frame; }

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

    void setPrefillThresholdFrames(uint32_t frames) { m_prefill_threshold_frames = frames; }
    uint32_t getPrefillThresholdFrames() const { return m_prefill_threshold_frames; }

    void setWatchdogTimeoutFrames(uint32_t frames) { m_watchdog_timeout_frames = frames; }
    uint32_t getWatchdogTimeoutFrames() const { return m_watchdog_timeout_frames; }

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
    uint16_t m_octets_per_frame = CONFIG_ESPNOW_FRAME_LEN_OCTETS;
    NetworkState m_state = NetworkState::OFF;
    StreamTelemetry m_telemetry;
    bool m_audio_task_running = false;
    bool m_wifi_initialized = false;

    // Prefill cushion & Watchdog loss thresholds
    uint32_t m_prefill_threshold_frames = ESPNOW_PREFILL_THRESHOLD_FRAMES_DEFAULT;
    uint32_t m_watchdog_timeout_frames = ESPNOW_WATCHDOG_TIMEOUT_FRAMES_DEFAULT;

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
