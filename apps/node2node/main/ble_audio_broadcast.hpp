#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include "esp_err.h"
#include "config.h"
#include "lc3_codec.hpp"
#include "tone_generator.hpp"
#include "i2s_audio.hpp"

namespace Bluetooth {

struct StreamTelemetry {
    std::string source_name = "N/A";
    std::string codec_name = "LC3 fixp";
    uint32_t sample_rate = AUDIO_SAMPLE_RATE_HZ;
    uint8_t  bit_depth = AUDIO_BIT_DEPTH;
    uint8_t  channels = AUDIO_CHANNELS_NUM;
    uint32_t bitrate_kbps = AUDIO_BITRATE_KBPS;
    uint8_t  bis_index = 1;
    int8_t   rssi_dbm = 0;
    uint32_t broadcast_id = 0x123456;
    uint32_t packets_count = 0;
    bool     is_synced = false;
};

class BleAudioBroadcast {
public:
    BleAudioBroadcast(Codec::Lc3CodecEngine& lc3_codec, 
                      Audio::ToneGenerator* tone_gen = nullptr, 
                      Hardware::I2sAudioDriver* i2s_dac = nullptr);
    ~BleAudioBroadcast();

    esp_err_t init(uint8_t node_role);
    void startAudioTask();

    const StreamTelemetry& getStreamTelemetry() const { return m_telemetry; }
    const char* getStateString() const;
    uint32_t getPacketsCount() const { return m_telemetry.packets_count; }

    void notifyAdvReceived(const char* name, int8_t rssi);
    void checkSyncState();
    void parseAdvReport(const uint8_t* data, uint8_t length_data, int8_t rssi);
    void startScanning();

private:
    static void hostTaskRoutine(void* param);
    static void audioTaskRoutine(void* param);
    static void onSyncCb(void);
    static void onResetCb(int reason);

    void runSourceLoop();
    void runSinkLoop();

    Codec::Lc3CodecEngine& m_lc3_codec;
    Audio::ToneGenerator* m_tone_gen = nullptr;
    Hardware::I2sAudioDriver* m_i2s_dac = nullptr;

    uint8_t m_node_role = CONFIG_ACTIVE_NODE_ROLE;
    bool m_initialized = false;
    bool m_audio_task_running = false;
    uint32_t m_last_adv_tick = 0;
    StreamTelemetry m_telemetry;
};

} // namespace Bluetooth
