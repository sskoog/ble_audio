#pragma once

#include "sdkconfig.h"
#include "esp_now.h"
#include "esp_wifi_types.h"
#include "lc3_codec.hpp"
#include "tone_generator.hpp"
#include "i2s_audio.hpp"
#include "audio_metering.hpp"
#include "audio_filters.hpp"
#include "config.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cmath>

#define MAX_UNICAST_SINKS 6

namespace AudioNet {

enum class NetworkState {
    OFF,
    IDLE,
    SCANNING,
    PREFILL,
    STREAM,
    CAST // SOURCE actively multicasting/unicasting to peers
};

enum class PeerStatus : uint8_t {
    DISABLED = 0,
    OFFLINE  = 1,
    ONLINE   = 2
};

enum class ControlOpcode : uint8_t {
    NONE        = 0x00,
    SINK_HELLO  = 0x01, // SINK -> SOURCE: Announce presence, target channel & request stream
    SINK_BYE    = 0x02, // SINK -> SOURCE: Graceful detach / power off
    SOURCE_ACK  = 0x03, // SOURCE -> SINK: Handshake ACK
    VOLUME_SET  = 0x04  // SOURCE -> SINK: Volume / gain control (uint8_t 0..255 dB scaled)
};

struct SinkPeerConfig {
    uint8_t    mac[6];
    uint8_t    channel_id;             // 0: Left, 1: Right, 2: Center, 3: L-Surround, 4: R-Surround, 5: Sub
    char       name[24];
    bool       is_enabled;
    PeerStatus status;                 // ONLINE, OFFLINE, DISABLED
    int64_t    session_start_time_us;  // Timestamp (us) when latest streaming session started for this node
    uint32_t   packets_sent;
    uint32_t   acks_received;
    uint32_t   ack_failures;
    uint32_t   consecutive_ack_fails;  // Circuit breaker: track consecutive missing ACKs
    int8_t     last_rssi;
};

#pragma pack(push, 1)
typedef struct {
    uint8_t  seq;        // Byte 0: Rolling sequence number (0..255)
    uint8_t  octets;     // Byte 1: 0 = Control / Handshake, 20..200 = LC3 Audio payload length
    union {
        struct {
            uint16_t flags;      // Bytes 2..3: Bit 0..2: SR (8k=0, 16k=1, 24k=2, 32k=3, 48k=4, 96k=5), Bit 3..4: Dur (10ms)
            uint32_t pts_us;     // Bytes 4..7: Presentation Time Stamp (us timeline, 32-bit aligned)
        } audio;
        struct {
            uint8_t  opcode;     // Byte 2: ControlOpcode (e.g. VOLUME_SET = 0x04)
            uint8_t  channel_id; // Byte 3: Target Audio Channel (0=Left, 1=Right, 5=Sub, 0xFF=All SINKs)
            uint8_t  volume_u8;  // Byte 4: Volume scaled (0=Mute, 1=-96.0dB, 255=0.0dB)
            uint8_t  flags;      // Byte 5: Bit 0: 0=Smooth Slew (96dB/s), 1=Instant
            uint16_t reserved;   // Bytes 6..7: Word padding
        } ctrl;
    };
} vsaf_packet_t;
typedef vsaf_packet_t vsaf_unicast_header_t;
#pragma pack(pop)

struct StreamTelemetry {
    uint32_t sample_rate = CONFIG_ESPNOW_SAMPLE_RATE_HZ;
    uint16_t frame_len = CONFIG_ESPNOW_FRAME_LEN_OCTETS;
    uint32_t frame_duration_us = 10000;
    uint8_t  bit_depth = 16;
    uint8_t  active_peers_count = 0;
};

// SPSC Duration Ring Buffer for Codec Performance Tracking
template <typename T, size_t N>
class SpscDurationRingBuffer {
public:
    void push(T val) {
        size_t next = (m_head + 1) % N;
        m_buffer[m_head] = val;
        m_head = next;
        if (m_count < N) m_count++;
    }
    void getStats(float& out_avg, float& out_max, bool& out_has_data) const {
        if (m_count == 0) {
            out_avg = 0.0f;
            out_max = 0.0f;
            out_has_data = false;
            return;
        }
        float sum = 0.0f;
        float max_v = 0.0f;
        for (size_t i = 0; i < m_count; i++) {
            float v = static_cast<float>(m_buffer[i]);
            sum += v;
            if (v > max_v) max_v = v;
        }
        out_avg = sum / static_cast<float>(m_count);
        out_max = max_v;
        out_has_data = true;
    }
private:
    T m_buffer[N];
    size_t m_head = 0;
    size_t m_count = 0;
};

// Time Offset Ring Buffer for Microsecond Clock Sync Telemetry
class TimeOffsetRingBuffer {
public:
    void push(float offset_ms) {
        m_buffer[m_head] = offset_ms;
        m_head = (m_head + 1) % CAPACITY;
        if (m_count < CAPACITY) m_count++;
    }
    void computeStats(float& out_median, float& out_range, bool& out_valid) const {
        if (m_count == 0) {
            out_median = 0.0f;
            out_range = 0.0f;
            out_valid = false;
            return;
        }
        float sorted[CAPACITY];
        for (size_t i = 0; i < m_count; i++) {
            sorted[i] = m_buffer[i];
        }
        for (size_t i = 0; i < m_count - 1; i++) {
            for (size_t j = i + 1; j < m_count; j++) {
                if (sorted[i] > sorted[j]) {
                    float tmp = sorted[i];
                    sorted[i] = sorted[j];
                    sorted[j] = tmp;
                }
            }
        }
        out_median = sorted[m_count / 2];
        out_range = sorted[m_count - 1] - sorted[0];
        out_valid = true;
    }
private:
    static constexpr size_t CAPACITY = 10;
    float m_buffer[CAPACITY] = {0.0f};
    size_t m_head = 0;
    size_t m_count = 0;
};

class EspNowUnicastEngine {
public:
    EspNowUnicastEngine(Codec::Lc3CodecEngine& lc3_codec,
                        Audio::ToneGenerator* tone_gen,
                        Hardware::I2sAudioDriver* i2s_dac);
    ~EspNowUnicastEngine();

    esp_err_t init(uint8_t role, uint8_t node_id, uint8_t wifi_channel = 1);
    esp_err_t start();
    esp_err_t stop();

    // Peer Management & Dynamic Handshake
    bool addPeer(const uint8_t* mac, uint8_t channel_id, const char* name);
    bool removePeer(const uint8_t* mac);
    bool setPeerEnabled(const uint8_t* mac, bool enabled);
    bool addOrUpdatePeerFromHello(const uint8_t* mac, uint8_t channel_id, const char* name = nullptr);
    int  getPeerCount() const { return m_peer_count; }
    const SinkPeerConfig* getPeer(int index) const;
    const SinkPeerConfig* getPeerByMac(const uint8_t* mac) const;
    void resetPeerStats();

    // Volume Control & Slew Limiter
    void setVolume(uint8_t vol_u8, bool instant = false);
    uint8_t getVolume() const { return m_target_volume_u8.load(std::memory_order_relaxed); }
    float getTargetVolumeDb() const;
    float getCurrentSlewDb() const { return m_current_gain_db; }
    void sendVolumeCommand(uint8_t channel_id, uint8_t vol_u8, bool instant = false);

    // Subwoofer DSP Control
    void setSubwooferCutoff(float cutoff_hz) {
        m_sub_cutoff_hz = cutoff_hz;
        m_sub_lr4_filter.setCutoff(cutoff_hz);
    }
    float getSubwooferCutoff() const { return m_sub_cutoff_hz; }

    // Dynamic Stream Configuration
    esp_err_t setSampleRate(uint32_t sample_rate_hz);
    esp_err_t setBitDepth(uint8_t bit_depth);
    esp_err_t setFrameLen(uint16_t frame_len_octets);
    esp_err_t setFrameDuration(uint32_t frame_duration_us);
    esp_err_t setWifiPhyRate(wifi_phy_mode_t phymode, wifi_phy_rate_t rate);

    wifi_phy_rate_t getWifiPhyRate() const { return m_tx_phy_rate; }
    uint32_t getSampleRate() const { return m_telemetry.sample_rate; }
    uint32_t getFrameDurationUs() const { return m_frame_duration_us; }
    uint16_t getFrameLen() const { return m_octets_per_frame; }
    uint8_t  getBitDepth() const { return m_telemetry.bit_depth; }

    // Multi-Channel Target Selection (SINK node: 0: Left, 1: Right, 5: Subwoofer)
    void setTargetChannel(uint8_t channel_id) {
        m_target_channel = channel_id & 0x07;
        if (m_node_role == NODE_ROLE_SINK) {
            uint32_t expected_sr = (m_target_channel == SUB_CHANNEL_ID) ? CONFIG_ESPNOW_SUB_SAMPLE_RATE_HZ : CONFIG_ESPNOW_SAMPLE_RATE_HZ;
            uint16_t expected_octets = (m_target_channel == SUB_CHANNEL_ID) ? CONFIG_ESPNOW_SUB_FRAME_LEN_OCTETS : m_octets_per_frame;
            if (expected_sr != m_telemetry.sample_rate) {
                m_telemetry.sample_rate = expected_sr;
                m_lc3_codec.reconfigureDecoder(expected_sr, expected_octets, m_frame_duration_us);
                if (m_i2s_dac) {
                    m_i2s_dac->reconfigureSampleRate(expected_sr, m_frame_duration_us);
                }
            }
        }
    }
    uint8_t getTargetChannel() const { return m_target_channel; }

    // Real-Time USB Audio Stream Ingestion (SOURCE node)
    void processUsbVsafPacket(const uint8_t* data, size_t len);
    bool isUsbStreamActive() const { return m_usb_stream_active.load(std::memory_order_relaxed); }

    // Packet Callbacks
    void onPacketSent(const uint8_t* mac_addr, esp_now_send_status_t status);
    void onPacketReceived(const uint8_t* mac_addr, const uint8_t* data, int data_len, int8_t rssi = -127, uint8_t rate = 0);
    void transitionTo(NetworkState new_state);

    NetworkState getState() const { return m_state; }
    const char* getStateString() const;
    const char* getActiveCodecName() const;
    const char* getWifiPhyRateString() const;
    int8_t getLastRssi() const { return m_last_rx_rssi.load(std::memory_order_relaxed); }
    bool hasLocalAudioOutput() const { return (m_i2s_dac != nullptr && m_node_role == NODE_ROLE_SINK); }
    const StreamTelemetry& getStreamTelemetry() const { return m_telemetry; }

    // Audio Metering
    int16_t getAudioFrameRMS_int16() { return m_audio_meter.getAudioFrameRMS_int16(); }
    int16_t getAudioPeak_int16() { return m_audio_meter.getAudioFramePeak_int16(); }
    float getAudioFrameRMS_dBFS() { return m_audio_meter.getAudioFrameRMS_dBFS(); }
    float getAudioPeak_dBFS() { return m_audio_meter.getAudioFramePeak_dBFS(); }

    // Statistics Getters
    uint32_t getTxPacketsTotal() const { return m_tx_packets_total.load(std::memory_order_relaxed); }
    uint32_t getAndResetTxPacketsSec() { return m_tx_packets_sec.exchange(0, std::memory_order_relaxed); }
    uint32_t getTxAcksTotal() const { return m_tx_acks_total.load(std::memory_order_relaxed); }
    uint32_t getTxAckFailsTotal() const { return m_tx_ack_fails_total.load(std::memory_order_relaxed); }

    uint32_t getRxPacketsTotal() const { return m_rx_packets_total.load(std::memory_order_relaxed); }
    uint32_t getAndResetRxPacketsSec() { return m_rx_packets_sec.exchange(0, std::memory_order_relaxed); }

    uint32_t getFifoOverflowCount() const { return m_fifo_overflow.load(std::memory_order_relaxed); }
    uint32_t getFifoUnderrunCount() const { return m_fifo_underrun.load(std::memory_order_relaxed); }
    uint32_t getAndResetFifoOverflowCount() { return m_fifo_overflow.exchange(0, std::memory_order_relaxed); }
    uint32_t getAndResetFifoUnderrunCount() { return m_fifo_underrun.exchange(0, std::memory_order_relaxed); }

    uint32_t getAndResetDmaUnderrunCount() { return m_i2s_dac ? m_i2s_dac->getAndResetUnderrunCount() : 0; }
    uint32_t getDmaUnderrunCount() const { return m_i2s_dac ? m_i2s_dac->getUnderrunCount() : 0; }
    uint8_t getHardwareGainDb() const { return m_i2s_dac ? m_i2s_dac->getHardwareGainDb() : 0; }
    void setHardwareGain(Hardware::Max98357Gain gain) { if (m_i2s_dac) m_i2s_dac->setHardwareGain(gain); }
    uint32_t getPlcCount() const { return m_lc3_codec.getPlcCount(); }
    uint32_t getAndResetPlcCount() { return m_lc3_codec.getAndResetPlcCount(); }

    void resetStreamingCounters();
    void resetErrorCounters();

    void setPrefillThresholdFrames(uint32_t frames) { m_prefill_threshold_frames = frames; }
    uint32_t getPrefillThresholdFrames() const { return m_prefill_threshold_frames; }
    void setWatchdogTimeoutFrames(uint32_t frames) { m_watchdog_timeout_frames = frames; }
    uint32_t getWatchdogTimeoutFrames() const { return m_watchdog_timeout_frames; }

    // Time Synchronization & Diagnostics Getters
    uint64_t getMasterTimeMs() const;
    bool isMasterTimeValid() const;
    uint32_t getClockSyncAdjustCount() const { return m_clock_sync_micro_adjust_count.load(std::memory_order_relaxed); }
    int64_t  getSessionStartTimeUs() const { return m_session_start_time_us.load(std::memory_order_relaxed); }

    void update10HzTimeOffsetStats();
    void getTimeOffsetStats(float& out_ema_ms, float& out_rb_med_ms, float& out_rb_rng_ms, bool& out_has_stats) const;

    void getCodecDurationStats(float& out_avg_ms, float& out_peak_ms, bool& out_has_data) const {
        m_codec_duration_ring_buffer.getStats(out_avg_ms, out_peak_ms, out_has_data);
        out_avg_ms /= 1000.0f;
        out_peak_ms /= 1000.0f;
    }

    // Stereo / Surround Mode (SOURCE node)
    void setStereo(bool stereo) { m_is_stereo.store(stereo, std::memory_order_relaxed); }
    bool isStereo() const { return m_is_stereo.load(std::memory_order_relaxed); }

    // Test Hooks
    void triggerSimulatedPacketDrop() { m_simulated_drop_count = 1; }

private:
    static void audioTaskRoutine(void* pvParameters);
    void runSourceLoop();
    void runSinkLoop();

    Codec::Lc3CodecEngine&     m_lc3_codec;     // Primary encoder (Left / Mono) or SINK decoder
    Codec::Lc3CodecEngine      m_lc3_codec_r;   // Secondary encoder (Right Channel for Stereo)
    Codec::Lc3CodecEngine      m_lc3_codec_sub; // Subwoofer encoder (8 kHz mono, 80 octets)
    Dsp::LinkwitzRiley4thOrderLowPass m_sub_lr4_filter; // 4th-order LR Low-Pass Filter @ 100 Hz
    float                      m_sub_cutoff_hz = CONFIG_ESPNOW_SUB_LP_HZ;

    Audio::ToneGenerator*      m_tone_gen;     // Primary tone generator (Left / Mono)
    Audio::ToneGenerator       m_tone_gen_r;   // Secondary tone generator (Right Channel for Stereo)
    Hardware::I2sAudioDriver*  m_i2s_dac;

    uint8_t                    m_node_role;
    uint8_t                    m_node_id;
    uint8_t                    m_target_channel = 0; // SINK listens to this channel (0:Left, 1:Right, 5:Sub)
    uint16_t                   m_octets_per_frame = CONFIG_ESPNOW_FRAME_LEN_OCTETS;
    uint32_t                   m_frame_duration_us = 10000;
    NetworkState               m_state = NetworkState::OFF;
    StreamTelemetry            m_telemetry;
    AudioMetering::AudioSignalMeter m_audio_meter;
    SpscDurationRingBuffer<uint32_t, 10> m_codec_duration_ring_buffer;
    TimeOffsetRingBuffer       m_time_offset_ring_buffer;
    std::atomic<float>         m_cached_rb_median_ms{0.0f};
    std::atomic<float>         m_cached_rb_range_ms{0.0f};
    std::atomic<bool>          m_has_cached_offset_stats{false};

    // SINK Volume & Slew Limiter State
    std::atomic<uint8_t>       m_target_volume_u8{CONFIG_VOLUME_DEFAULT_U8};
    std::atomic<bool>          m_instant_volume_requested{false};
    float                      m_current_gain_db = 0.0f;
    float                      m_current_linear_gain = 1.0f;

    bool                       m_wifi_initialized = false;
    bool                       m_audio_task_running = false;
    uint8_t                    m_last_rx_seq = 0;
    bool                       m_has_last_rx_seq = false;

    uint32_t                   m_prefill_threshold_frames = CONFIG_ESPNOW_PREFILL_THRESHOLD_FRAMES;
    uint32_t                   m_watchdog_timeout_frames  = CONFIG_ESPNOW_WATCHDOG_TIMEOUT_FRAMES;

    int64_t                    m_master_time_offset_us = 0;
    std::atomic<int64_t>       m_last_sync_time_us{0};
    std::atomic<int64_t>       m_session_start_time_us{0};
    std::atomic<uint32_t>      m_clock_sync_micro_adjust_count{0};
    std::atomic<uint32_t>      m_simulated_drop_count{0};

    // Unicast Peer Table
    SinkPeerConfig             m_peers[MAX_UNICAST_SINKS];
    int                        m_peer_count = 0;
    portMUX_TYPE               m_peer_mux = portMUX_INITIALIZER_UNLOCKED;

    // Ingest State (SOURCE node)
    std::atomic<bool>          m_is_stereo{true};
    std::atomic<bool>          m_usb_stream_active{false};
    std::atomic<int64_t>       m_last_usb_packet_time_us{0};

    std::atomic<uint32_t>      m_tx_packets_total{0};
    std::atomic<uint32_t>      m_tx_packets_sec{0};
    std::atomic<uint32_t>      m_tx_acks_total{0};
    std::atomic<uint32_t>      m_tx_ack_fails_total{0};

    std::atomic<uint32_t>      m_rx_packets_total{0};
    std::atomic<uint32_t>      m_rx_packets_sec{0};
    std::atomic<uint32_t>      m_fifo_overflow{0};
    std::atomic<uint32_t>      m_fifo_underrun{0};
    std::atomic<int8_t>        m_last_rx_rssi{-127};
    std::atomic<uint8_t>       m_last_rx_rate{0};
    wifi_phy_mode_t            m_tx_phy_mode = WIFI_PHY_MODE_HT20;
    wifi_phy_rate_t            m_tx_phy_rate = WIFI_PHY_RATE_MCS1_LGI; // 13.0 Mbps default
};

} // namespace AudioNet
