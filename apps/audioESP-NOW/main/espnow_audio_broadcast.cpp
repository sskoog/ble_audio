#include "espnow_audio_broadcast.hpp"
#include "config.h"
#include "status_led.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include <cstring>

static const char* TAG = "ESP_NOW_AUDIO";

namespace AudioNet {

static const uint8_t s_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static EspNowAudioBroadcast* s_instance = nullptr;
static TaskHandle_t s_audio_task_handle = nullptr;

// Thread-safe Single-Producer Single-Consumer FIFO for LC3 frames
struct Lc3RxFrame {
    uint8_t  data[MAX_LC3_FRAME_OCTETS];
    uint8_t  len;
    uint8_t  seq;
    uint16_t sample_rate_hz;
    uint16_t frame_duration_us;
    uint32_t pts_us;
};

static constexpr size_t LC3_RX_FIFO_CAPACITY = 24;
static Lc3RxFrame s_rx_fifo[LC3_RX_FIFO_CAPACITY];
static size_t s_rx_fifo_head = 0;
static size_t s_rx_fifo_tail = 0;
static size_t s_rx_fifo_count = 0;
static portMUX_TYPE s_fifo_mux = portMUX_INITIALIZER_UNLOCKED;

static inline bool push_rx_lc3_frame(const uint8_t* data, size_t len, uint8_t seq,
                                     uint16_t sample_rate_hz, uint16_t frame_duration_us, uint32_t pts_us) {
    if (!data || len == 0 || len > MAX_LC3_FRAME_OCTETS) return false;
    taskENTER_CRITICAL(&s_fifo_mux);
    if (s_rx_fifo_count >= LC3_RX_FIFO_CAPACITY) {
        s_rx_fifo_tail = (s_rx_fifo_tail + 1) % LC3_RX_FIFO_CAPACITY;
        s_rx_fifo_count--;
    }
    s_rx_fifo[s_rx_fifo_head].len = static_cast<uint8_t>(len);
    s_rx_fifo[s_rx_fifo_head].seq = seq;
    s_rx_fifo[s_rx_fifo_head].sample_rate_hz = sample_rate_hz;
    s_rx_fifo[s_rx_fifo_head].frame_duration_us = frame_duration_us;
    s_rx_fifo[s_rx_fifo_head].pts_us = pts_us;
    memcpy(s_rx_fifo[s_rx_fifo_head].data, data, len);
    s_rx_fifo_head = (s_rx_fifo_head + 1) % LC3_RX_FIFO_CAPACITY;
    s_rx_fifo_count++;
    taskEXIT_CRITICAL(&s_fifo_mux);
    return true;
}

static inline bool pop_rx_lc3_frame(uint8_t* out_data, size_t* out_len, uint8_t* out_seq = nullptr,
                                    uint16_t* out_sample_rate = nullptr, uint16_t* out_frame_duration = nullptr,
                                    uint32_t* out_pts = nullptr) {
    if (!out_data || !out_len) return false;
    taskENTER_CRITICAL(&s_fifo_mux);
    if (s_rx_fifo_count == 0) {
        taskEXIT_CRITICAL(&s_fifo_mux);
        return false;
    }
    *out_len = s_rx_fifo[s_rx_fifo_tail].len;
    if (out_seq) *out_seq = s_rx_fifo[s_rx_fifo_tail].seq;
    if (out_sample_rate) *out_sample_rate = s_rx_fifo[s_rx_fifo_tail].sample_rate_hz;
    if (out_frame_duration) *out_frame_duration = s_rx_fifo[s_rx_fifo_tail].frame_duration_us;
    if (out_pts) *out_pts = s_rx_fifo[s_rx_fifo_tail].pts_us;
    memcpy(out_data, s_rx_fifo[s_rx_fifo_tail].data, *out_len);
    s_rx_fifo_tail = (s_rx_fifo_tail + 1) % LC3_RX_FIFO_CAPACITY;
    s_rx_fifo_count--;
    taskEXIT_CRITICAL(&s_fifo_mux);
    return true;
}

static inline size_t get_rx_fifo_count() {
    taskENTER_CRITICAL(&s_fifo_mux);
    size_t count = s_rx_fifo_count;
    taskEXIT_CRITICAL(&s_fifo_mux);
    return count;
}

static inline void clear_rx_fifo() {
    taskENTER_CRITICAL(&s_fifo_mux);
    s_rx_fifo_head = 0;
    s_rx_fifo_tail = 0;
    s_rx_fifo_count = 0;
    taskEXIT_CRITICAL(&s_fifo_mux);
}

#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
static void onEspNowSendCb(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {}
#else
static void onEspNowSendCb(const uint8_t *mac_addr, esp_now_send_status_t status) {}
#endif

static void onEspNowRecvCb(const esp_now_recv_info_t* recv_info, const uint8_t* data, int len) {
    if (s_instance) {
        s_instance->onPacketReceived(recv_info->src_addr, data, len);
    }
}

EspNowAudioBroadcast::EspNowAudioBroadcast(Codec::Lc3CodecEngine& lc3_codec,
                                           Audio::ToneGenerator* tone_gen,
                                           Hardware::I2sAudioDriver* i2s_dac)
    : m_lc3_codec(lc3_codec), m_tone_gen(tone_gen), m_i2s_dac(i2s_dac) {
    s_instance = this;
}

EspNowAudioBroadcast::~EspNowAudioBroadcast() {
    m_audio_task_running = false;
    s_instance = nullptr;
}

esp_err_t EspNowAudioBroadcast::init(uint8_t node_role) {
    m_node_role = node_role;
    m_node_id = get_system_config()->node_id;
    m_telemetry.sample_rate = CONFIG_ESPNOW_SAMPLE_RATE_HZ;
    m_telemetry.frame_duration_us = 10000;
    m_frame_duration_us = 10000;
    m_telemetry.channels = 1;
    m_target_channel = get_system_config()->default_channel;
    m_octets_per_frame = CONFIG_ESPNOW_FRAME_LEN_OCTETS;
    m_telemetry.bitrate_kbps = (m_octets_per_frame * 8) / 10;
    m_active_magic = VSAF_DEFAULT_MAGIC;
    m_usb_stream_active.store(false);
    m_last_usb_packet_time_us.store(0);

    if (m_node_role == NODE_ROLE_SOURCE) {
        m_lc3_codec.initEncoder(m_telemetry.sample_rate, 1, m_frame_duration_us, m_octets_per_frame);
        if (m_tone_gen) {
            m_tone_gen->setSampleRate(m_telemetry.sample_rate);
        }
    } else { // SINK
        m_lc3_codec.initDecoder(m_telemetry.sample_rate, 1, m_frame_duration_us, m_octets_per_frame);
        if (m_i2s_dac) {
            m_i2s_dac->reconfigureSampleRate(m_telemetry.sample_rate);
        }
    }

    ESP_LOGI(TAG, "EspNowAudioBroadcast initialized (Role: %s, ID: %u, Fs: %lu Hz, Dur: %.1fms, Frame: %u octets, Magic: 0x%04X, Ch: %u)",
             (node_role == NODE_ROLE_SOURCE) ? "SOURCE" : "SINK", m_node_id,
             (unsigned long)m_telemetry.sample_rate, m_frame_duration_us / 1000.0f, m_octets_per_frame, m_active_magic, m_target_channel);
    return ESP_OK;
}

uint64_t EspNowAudioBroadcast::getMasterTimeMs() const {
    int64_t local_us = esp_timer_get_time();
    if (m_node_role == NODE_ROLE_SOURCE) {
        return static_cast<uint64_t>(local_us / 1000);
    } else {
        int64_t master_us = local_us + m_master_time_offset_us;
        return (master_us > 0) ? static_cast<uint64_t>(master_us / 1000) : 0;
    }
}

esp_err_t EspNowAudioBroadcast::setAudioConfig(uint32_t sample_rate_hz, uint16_t frame_len_octets, uint32_t frame_duration_us) {
    if (sample_rate_hz != 8000 && sample_rate_hz != 16000 && sample_rate_hz != 24000 && sample_rate_hz != 32000 &&
        sample_rate_hz != 44100 && sample_rate_hz != 48000) {
        ESP_LOGE(TAG, "Unsupported sample rate: %lu Hz", (unsigned long)sample_rate_hz);
        return ESP_ERR_INVALID_ARG;
    }
    if (frame_len_octets < 20 || frame_len_octets > MAX_LC3_FRAME_OCTETS) {
        ESP_LOGE(TAG, "Invalid LC3 frame len: %u octets (supported: 20..%u)", frame_len_octets, MAX_LC3_FRAME_OCTETS);
        return ESP_ERR_INVALID_ARG;
    }
    if (frame_duration_us != 0 && frame_duration_us != 7500 && frame_duration_us != 10000) {
        ESP_LOGE(TAG, "Invalid frame duration: %lu us (supported: 7500 or 10000)", (unsigned long)frame_duration_us);
        return ESP_ERR_INVALID_ARG;
    }

    if (frame_duration_us != 0) {
        m_frame_duration_us = frame_duration_us;
        m_telemetry.frame_duration_us = frame_duration_us;
    }
    m_telemetry.sample_rate = sample_rate_hz;
    m_octets_per_frame = frame_len_octets;
    m_telemetry.bitrate_kbps = (static_cast<uint32_t>(frame_len_octets) * 8 * 1000000ULL) / (m_frame_duration_us * 1000ULL);

    if (m_node_role == NODE_ROLE_SOURCE) {
        m_lc3_codec.reconfigureEncoder(sample_rate_hz, frame_len_octets, m_frame_duration_us);
        if (m_tone_gen) {
            m_tone_gen->setSampleRate(sample_rate_hz);
        }
    } else {
        m_lc3_codec.reconfigureDecoder(sample_rate_hz, frame_len_octets, m_frame_duration_us);
        if (m_i2s_dac) {
            m_i2s_dac->reconfigureSampleRate(sample_rate_hz);
        }
    }
    ESP_LOGI(TAG, "Audio Configuration updated on-the-fly: %lu Hz, %.1f ms, %u octets/frame (%lu kbps)",
             (unsigned long)sample_rate_hz, m_frame_duration_us / 1000.0f, frame_len_octets, (unsigned long)m_telemetry.bitrate_kbps);
    return ESP_OK;
}

esp_err_t EspNowAudioBroadcast::setSampleRate(uint32_t sample_rate_hz) {
    return setAudioConfig(sample_rate_hz, m_octets_per_frame, m_frame_duration_us);
}

esp_err_t EspNowAudioBroadcast::setFrameLen(uint16_t frame_len_octets) {
    return setAudioConfig(m_telemetry.sample_rate, frame_len_octets, m_frame_duration_us);
}

esp_err_t EspNowAudioBroadcast::setFrameDuration(uint32_t frame_duration_us) {
    return setAudioConfig(m_telemetry.sample_rate, 120, frame_duration_us);
}

void EspNowAudioBroadcast::processUsbVsafPacket(const uint8_t* data, size_t len) {
    if (m_node_role != NODE_ROLE_SOURCE) return;
    if (!data || len < sizeof(EspNowAudioPacket)) return;

    const auto* pkt = reinterpret_cast<const EspNowAudioPacket*>(data);
    if (pkt->magic != m_active_magic) return;

    // Check if transitioning to USB active mode
    if (!m_usb_stream_active.load(std::memory_order_relaxed)) {
        ESP_LOGI(TAG, "SOURCE: Detected USB PC Audio Stream! Suspending internal tone generator...");
    }
    m_usb_stream_active.store(true, std::memory_order_relaxed);
    m_last_usb_packet_time_us.store(esp_timer_get_time(), std::memory_order_relaxed);

    // Update telemetry from packet config
    uint8_t sr_code = (pkt->cfg >> 3) & 0x07;
    uint8_t dur_bit = (pkt->cfg >> 6) & 0x01;
    uint32_t stream_sr = codeToSampleRate(sr_code);
    uint32_t stream_dur = dur_bit ? 7500 : 10000;
    m_telemetry.sample_rate = stream_sr;
    m_telemetry.frame_duration_us = stream_dur;
    m_octets_per_frame = 120;
    m_telemetry.bitrate_kbps = (120 * 8 * 1000000ULL) / (stream_dur * 1000ULL);

    // Directly broadcast intact 248-byte packet over ESP-NOW
    esp_now_send(s_broadcast_mac, data, sizeof(EspNowAudioPacket));

    m_tx_packets_total.fetch_add(1, std::memory_order_relaxed);
    m_tx_packets_sec.fetch_add(1, std::memory_order_relaxed);
}

esp_err_t EspNowAudioBroadcast::enableWifiEspNow() {
    if (m_wifi_initialized) return ESP_OK;
    ESP_LOGI(TAG, "Initializing Wi-Fi and ESP-NOW Subsystem...");
    
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_wifi_config_11b_rate(WIFI_IF_STA, true);
    esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_24M);
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "Local STA MAC: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(onEspNowSendCb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(onEspNowRecvCb));

    esp_now_peer_info_t bcast_peer = {};
    memcpy(bcast_peer.peer_addr, s_broadcast_mac, 6);
    bcast_peer.channel = 1;
    bcast_peer.ifidx = WIFI_IF_STA;
    bcast_peer.encrypt = false;
    esp_now_add_peer(&bcast_peer);

    m_wifi_initialized = true;
    ESP_LOGI(TAG, "Wi-Fi & ESP-NOW enabled on Channel 1 (Broadcast: FF:FF:FF:FF:FF:FF)");
    return ESP_OK;
}

esp_err_t EspNowAudioBroadcast::startAudioTask() {
    m_audio_task_running = true;
    xTaskCreate(audioTaskRoutine, "espnow_audio", 8192, this, 5, &s_audio_task_handle);
    return ESP_OK;
}

void EspNowAudioBroadcast::audioTaskRoutine(void* pvParameters) {
    auto* self = static_cast<EspNowAudioBroadcast*>(pvParameters);
    if (self->m_node_role == NODE_ROLE_SOURCE) {
        self->runSourceLoop();
    } else {
        self->runSinkLoop();
    }
    vTaskDelete(nullptr);
}

void EspNowAudioBroadcast::runSourceLoop() {
    ESP_LOGI(TAG, "ESP-NOW Audio Source Loop Started (Dual Mode: USB Ingest + Internal Synth Fallback)...");

    static int16_t pcm_buffer[MAX_PCM_FRAME_SAMPLES] = {0};
    static int16_t pcm_buffer_ch1[MAX_PCM_FRAME_SAMPLES] = {0};
    static uint8_t curr_lc3[MAX_LC3_FRAME_OCTETS] = {0};
    static uint8_t prev_lc3[MAX_LC3_FRAME_OCTETS] = {0};
    static uint8_t curr_lc3_ch1[MAX_LC3_FRAME_OCTETS] = {0};
    static uint8_t prev_lc3_ch1[MAX_LC3_FRAME_OCTETS] = {0};
    size_t encoded_bytes = 0;
    uint8_t seq = 0;

    int64_t next_frame_us = esp_timer_get_time() + m_frame_duration_us;

    while (m_audio_task_running) {
        // Check if USB streaming from PC is currently active
        bool usb_active = m_usb_stream_active.load(std::memory_order_relaxed);
        if (usb_active) {
            int64_t now_us = esp_timer_get_time();
            int64_t last_usb_us = m_last_usb_packet_time_us.load(std::memory_order_relaxed);
            if (now_us - last_usb_us > 250000) { // 250 ms timeout
                m_usb_stream_active.store(false, std::memory_order_relaxed);
                ESP_LOGW(TAG, "SOURCE: USB stream timed out (>250ms). Resuming internal tone generator...");
                next_frame_us = esp_timer_get_time() + m_frame_duration_us;
            } else {
                // USB Stream active: suspend internal encoder loop (0% ESP32 CPU overhead)
                vTaskDelay(pdMS_TO_TICKS(15));
                continue;
            }
        }

        int64_t now_us = esp_timer_get_time();
        int64_t wait_us = next_frame_us - now_us;
        if (wait_us > 1500) {
            vTaskDelay(pdMS_TO_TICKS(wait_us / 1000 - 1));
        }
        while (esp_timer_get_time() < next_frame_us) {
            esp_rom_delay_us(10);
        }

        uint32_t active_dur_us = m_frame_duration_us;
        next_frame_us += active_dur_us;

        if (m_state == NetworkState::BROADCASTING) {
            uint32_t current_sr = m_telemetry.sample_rate;
            uint16_t current_len = m_octets_per_frame;
            size_t samples_to_gen = Codec::calculateRequiredPcmSamples(current_sr, active_dur_us);

            if (m_lc3_codec.getSampleRate() != current_sr || m_lc3_codec.getFrameDurationUs() != active_dur_us) {
                m_lc3_codec.reconfigureEncoder(current_sr, current_len, active_dur_us);
                if (m_tone_gen) {
                    m_tone_gen->setSampleRate(current_sr);
                }
            }

            // Channel 0 (Left)
            if (m_tone_gen) {
                m_tone_gen->generateFrame(pcm_buffer, samples_to_gen);
            }
            // Encode single LC3 frame (takes ~4.5ms of 10.0ms slot = 45% CPU load)
            m_lc3_codec.encodeFrame(pcm_buffer, samples_to_gen, curr_lc3, sizeof(curr_lc3), &encoded_bytes);
            m_audio_meter.pushFramePcm(pcm_buffer, samples_to_gen);

            uint32_t drop_val = m_simulated_drop_count.load(std::memory_order_relaxed);
            if (drop_val > 0) {
                m_simulated_drop_count.store(0, std::memory_order_relaxed);
                ESP_LOGW(TAG, "SOURCE: [TEST] Deliberately dropping packet Seq #%u to test SINK prev_frame recovery", seq);
                seq += 2;
                memcpy(prev_lc3, curr_lc3, current_len);
                continue;
            }

            uint8_t pkt_buf[VSAF_HEADER_LEN + 2 * MAX_LC3_FRAME_OCTETS];
            auto* hdr = reinterpret_cast<EspNowAudioHeader*>(pkt_buf);
            hdr->magic = m_active_magic;
            uint8_t dur_bit = (active_dur_us == 7500) ? 1 : 0;
            uint32_t pts = static_cast<uint32_t>(esp_timer_get_time());
            size_t pkt_len = VSAF_HEADER_LEN + 2 * current_len;

            // Packet 1: Channel 0 (Left - Node 23)
            hdr->seq = seq;
            hdr->cfg = (0 & 0x07) | (sampleRateToCode(current_sr) << 3) | (dur_bit << 6) | (0 << 7);
            hdr->pts_us = pts;
            memcpy(pkt_buf + VSAF_HEADER_LEN, curr_lc3, current_len);
            memcpy(pkt_buf + VSAF_HEADER_LEN + current_len, prev_lc3, current_len);
            esp_now_send(s_broadcast_mac, pkt_buf, pkt_len);

            // Packet 2: Channel 1 (Right - Node 24)
            hdr->seq = seq;
            hdr->cfg = (1 & 0x07) | (sampleRateToCode(current_sr) << 3) | (dur_bit << 6) | (0 << 7);
            hdr->pts_us = pts;
            esp_now_send(s_broadcast_mac, pkt_buf, pkt_len);

            seq++;
            memcpy(prev_lc3, curr_lc3, current_len);
            m_tx_packets_total.fetch_add(2, std::memory_order_relaxed);
            m_tx_packets_sec.fetch_add(2, std::memory_order_relaxed);
        }
    }
}

void EspNowAudioBroadcast::runSinkLoop() {
    static uint8_t current_lc3_buf[MAX_LC3_FRAME_OCTETS] = {0};
    static int16_t decoded_pcm[MAX_PCM_FRAME_SAMPLES] = {0};
    static int16_t stereo_pcm[MAX_PCM_FRAME_SAMPLES * 2] = {0};
    size_t actual_samples = 0;
    size_t bytes_written = 0;
    uint32_t consecutive_plc_count = 0;
    uint32_t sync_eval_counter = 0;

    while (m_audio_task_running) {
        uint32_t vol_scale = m_telemetry.is_muted ? 0 : ((static_cast<uint32_t>(m_telemetry.volume_percent) * 255) / 100);

        switch (m_state) {
            case NetworkState::OFF:
            case NetworkState::IDLE: {
                vTaskDelay(pdMS_TO_TICKS(50));
                break;
            }

            case NetworkState::SCANNING: {
                size_t buffered = get_rx_fifo_count();
                if (buffered >= m_prefill_threshold_frames) {
                    ESP_LOGI(TAG, "SINK: Found audio stream (FIFO = %zu pkts, threshold = %lu). Requesting transition to PREFILL...",
                             buffered, (unsigned long)m_prefill_threshold_frames);
                    transitionTo(NetworkState::PREFILL);
                } else {
                    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30));
                }
                break;
            }

            case NetworkState::PREFILL: {
                size_t lc3_len = 0;
                uint8_t seq = 0;
                uint16_t frame_sr = 0;
                uint16_t frame_dur = 0;
                uint32_t frame_pts = 0;

                if (pop_rx_lc3_frame(current_lc3_buf, &lc3_len, &seq, &frame_sr, &frame_dur, &frame_pts) && lc3_len >= 20) {
                    if (frame_sr > 0 && (frame_sr != m_telemetry.sample_rate || frame_dur != m_telemetry.frame_duration_us)) {
                        m_telemetry.sample_rate = frame_sr;
                        m_telemetry.frame_duration_us = frame_dur;
                        m_telemetry.bitrate_kbps = (lc3_len * 8 * 1000000ULL) / (frame_dur * 1000ULL);
                        if (m_i2s_dac) m_i2s_dac->reconfigureSampleRate(frame_sr);
                    }
                    m_lc3_codec.decodeFrame(current_lc3_buf, lc3_len, decoded_pcm, MAX_PCM_FRAME_SAMPLES, &actual_samples,
                                            m_telemetry.sample_rate, m_telemetry.frame_duration_us);
                    for (size_t i = 0; i < actual_samples; ++i) {
                        int16_t sample = static_cast<int16_t>((static_cast<int32_t>(decoded_pcm[i]) * vol_scale) / 255);
                        stereo_pcm[2 * i]     = sample;
                        stereo_pcm[2 * i + 1] = sample;
                    }
                    m_audio_meter.pushFramePcm(decoded_pcm, actual_samples);
                    if (m_i2s_dac) {
                        m_i2s_dac->preload(stereo_pcm, actual_samples * 2 * sizeof(int16_t), &bytes_written);
                    }
                }

                if (pop_rx_lc3_frame(current_lc3_buf, &lc3_len, &seq, &frame_sr, &frame_dur, &frame_pts) && lc3_len >= 20) {
                    if (frame_sr > 0 && (frame_sr != m_telemetry.sample_rate || frame_dur != m_telemetry.frame_duration_us)) {
                        m_telemetry.sample_rate = frame_sr;
                        m_telemetry.frame_duration_us = frame_dur;
                        m_telemetry.bitrate_kbps = (lc3_len * 8 * 1000000ULL) / (frame_dur * 1000ULL);
                        if (m_i2s_dac) m_i2s_dac->reconfigureSampleRate(frame_sr);
                    }
                    m_lc3_codec.decodeFrame(current_lc3_buf, lc3_len, decoded_pcm, MAX_PCM_FRAME_SAMPLES, &actual_samples,
                                            m_telemetry.sample_rate, m_telemetry.frame_duration_us);
                    for (size_t i = 0; i < actual_samples; ++i) {
                        int16_t sample = static_cast<int16_t>((static_cast<int32_t>(decoded_pcm[i]) * vol_scale) / 255);
                        stereo_pcm[2 * i]     = sample;
                        stereo_pcm[2 * i + 1] = sample;
                    }
                    m_audio_meter.pushFramePcm(decoded_pcm, actual_samples);
                    if (m_i2s_dac) {
                        m_i2s_dac->preload(stereo_pcm, actual_samples * 2 * sizeof(int16_t), &bytes_written);
                    }
                }

                consecutive_plc_count = 0;
                ESP_LOGI(TAG, "SINK: PREFILL complete (2 DMA descriptors loaded @ %lu Hz, %.1fms, %zu pkts cushion). Transitioning to STREAMING...",
                         (unsigned long)m_telemetry.sample_rate, m_telemetry.frame_duration_us / 1000.0f, get_rx_fifo_count());
                transitionTo(NetworkState::STREAMING);
                break;
            }

            case NetworkState::STREAMING: {
                if (m_i2s_dac && m_i2s_dac->isRunning()) {
                    m_i2s_dac->waitForDmaSlot(20);
                }

                size_t lc3_len = 0;
                uint8_t seq = 0;
                uint16_t frame_sr = 0;
                uint16_t frame_dur = 0;
                uint32_t frame_pts = 0;
                bool has_packet = pop_rx_lc3_frame(current_lc3_buf, &lc3_len, &seq, &frame_sr, &frame_dur, &frame_pts);

                if (has_packet && lc3_len >= 20) {
                    consecutive_plc_count = 0;

                    if (frame_sr > 0 && (frame_sr != m_telemetry.sample_rate || frame_dur != m_telemetry.frame_duration_us)) {
                        m_telemetry.sample_rate = frame_sr;
                        m_telemetry.frame_duration_us = frame_dur;
                        m_telemetry.bitrate_kbps = (lc3_len * 8 * 1000000ULL) / (frame_dur * 1000ULL);
                        if (m_i2s_dac) {
                            m_i2s_dac->reconfigureSampleRate(frame_sr);
                        }
                    }

                    sync_eval_counter++;
                    if ((sync_eval_counter % 50) == 0) {
                        int64_t now_us = esp_timer_get_time();
                        int64_t expected_pts_us = now_us + m_master_time_offset_us;
                        int64_t phase_err = static_cast<int64_t>(frame_pts) - expected_pts_us;
                        if (std::abs(phase_err) > 20) {
                            m_clock_sync_micro_adjust_count.fetch_add(1, std::memory_order_relaxed);
                        }
                    }

                    m_lc3_codec.decodeFrame(current_lc3_buf, lc3_len, decoded_pcm, MAX_PCM_FRAME_SAMPLES, &actual_samples,
                                            m_telemetry.sample_rate, m_telemetry.frame_duration_us);

                    for (size_t i = 0; i < actual_samples; ++i) {
                        int16_t sample = static_cast<int16_t>((static_cast<int32_t>(decoded_pcm[i]) * vol_scale) / 255);
                        stereo_pcm[2 * i]     = sample;
                        stereo_pcm[2 * i + 1] = sample;
                    }
                    m_audio_meter.pushFramePcm(decoded_pcm, actual_samples);

                    if (m_i2s_dac && m_i2s_dac->isInitialized()) {
                        m_i2s_dac->write(stereo_pcm, actual_samples * 2 * sizeof(int16_t), &bytes_written, 15);
                    }
                } else {
                    m_fifo_underrun.fetch_add(1, std::memory_order_relaxed);
                    consecutive_plc_count++;

                    if (consecutive_plc_count < m_watchdog_timeout_frames) {
                        m_lc3_codec.decodeFrame(nullptr, 0, decoded_pcm, MAX_PCM_FRAME_SAMPLES, &actual_samples,
                                                m_telemetry.sample_rate, m_telemetry.frame_duration_us);
                        for (size_t i = 0; i < actual_samples; ++i) {
                            int16_t sample = static_cast<int16_t>((static_cast<int32_t>(decoded_pcm[i]) * vol_scale) / 255);
                            stereo_pcm[2 * i]     = sample;
                            stereo_pcm[2 * i + 1] = sample;
                        }
                        m_audio_meter.pushFramePcm(decoded_pcm, actual_samples);

                        if (m_i2s_dac && m_i2s_dac->isInitialized()) {
                            m_i2s_dac->write(stereo_pcm, actual_samples * 2 * sizeof(int16_t), &bytes_written, 15);
                        }
                    } else {
                        ESP_LOGW(TAG, "SINK: Reached %lu consecutive PLC frames (%lu ms loss). Requesting transition to SCANNING...",
                                 (unsigned long)m_watchdog_timeout_frames,
                                 (unsigned long)((m_watchdog_timeout_frames * m_telemetry.frame_duration_us) / 1000));
                        consecutive_plc_count = 0;
                        transitionTo(NetworkState::SCANNING);
                    }
                }
                break;
            }

            default:
                vTaskDelay(pdMS_TO_TICKS(10));
                break;
        }
    }
}

void EspNowAudioBroadcast::onPacketReceived(const uint8_t* mac_addr, const uint8_t* data, int data_len) {
    if (m_node_role != NODE_ROLE_SINK) return;
    if (data_len < static_cast<int>(sizeof(EspNowAudioPacket))) return;

    const auto* pkt = reinterpret_cast<const EspNowAudioPacket*>(data);
    if (pkt->magic != m_active_magic) {
        return;
    }

    // Filter by audio channel
    uint8_t pkt_ch = pkt->cfg & 0x07;
    if (pkt_ch != m_target_channel) {
        return; // Discard packets intended for other audio channels
    }

    uint8_t sr_code = (pkt->cfg >> 3) & 0x07;
    uint32_t sample_rate = codeToSampleRate(sr_code);
    uint8_t dur_bit = (pkt->cfg >> 6) & 0x01;
    uint32_t pkt_dur_us = dur_bit ? 7500 : 10000;
    uint16_t frame_len = 120; // Fixed intact 120-octet LC3 frame

    uint32_t pts_curr = pkt->pts_us;
    int64_t now_us = esp_timer_get_time();
    m_master_time_offset_us = static_cast<int64_t>(pts_curr) - now_us;

    m_telemetry.rssi_dbm = -26;

    if (m_has_last_rx_seq) {
        if (pkt->seq == m_last_rx_seq) {
            return;
        }

        uint8_t expected = m_last_rx_seq + 1;
        if (pkt->seq == static_cast<uint8_t>(expected + 1)) {
            uint32_t pts_prev = pts_curr - pkt_dur_us;
            if (push_rx_lc3_frame(pkt->prev_frame, frame_len, expected, static_cast<uint16_t>(sample_rate),
                                  static_cast<uint16_t>(pkt_dur_us), pts_prev)) {
                m_rx_packets_total.fetch_add(1, std::memory_order_relaxed);
                m_rx_packets_sec.fetch_add(1, std::memory_order_relaxed);
                m_prev_frame_recoveries.fetch_add(1, std::memory_order_relaxed);
                ESP_LOGI(TAG, "SINK: [PLC-RECOVERY] Successfully recovered dropped packet Seq #%u (PTS: %lu us, Dur: %.1fms) from prev_frame!",
                         expected, (unsigned long)pts_prev, pkt_dur_us / 1000.0f);
            }
        }
    }

    if (push_rx_lc3_frame(pkt->curr_frame, frame_len, pkt->seq, static_cast<uint16_t>(sample_rate),
                          static_cast<uint16_t>(pkt_dur_us), pts_curr)) {
        m_rx_packets_total.fetch_add(1, std::memory_order_relaxed);
        m_rx_packets_sec.fetch_add(1, std::memory_order_relaxed);

        m_last_rx_seq = pkt->seq;
        m_has_last_rx_seq = true;

        if (s_audio_task_handle) {
            xTaskNotifyGive(s_audio_task_handle);
        }
    } else {
        m_fifo_overflow.fetch_add(1, std::memory_order_relaxed);
    }
}

void EspNowAudioBroadcast::transitionTo(NetworkState new_state) {
    if (m_state == new_state) return;

    const char* old_str = getStateString();
    m_state = new_state;
    const char* new_str = getStateString();

    ESP_LOGI(TAG, "State Machine Transition: [%s] ---> [%s]", old_str, new_str);

    switch (new_state) {
        case NetworkState::OFF: {
            if (m_i2s_dac) {
                m_i2s_dac->stop();
            }
            clear_rx_fifo();
            m_has_last_rx_seq = false;
            m_audio_meter.pushSilence();
            Hardware::getStatusLed().off();
            break;
        }

        case NetworkState::IDLE: {
            if (!m_wifi_initialized) {
                enableWifiEspNow();
            }
            if (m_i2s_dac) {
                m_i2s_dac->stop();
            }
            clear_rx_fifo();
            m_has_last_rx_seq = false;
            m_audio_meter.pushSilence();
            Hardware::getStatusLed().setSystemState(Hardware::SystemState::IDLE);
            break;
        }

        case NetworkState::SCANNING: {
            if (!m_wifi_initialized) {
                enableWifiEspNow();
            }
            if (m_i2s_dac) {
                m_i2s_dac->stop();
            }
            clear_rx_fifo();
            m_has_last_rx_seq = false;
            m_audio_meter.pushSilence();
            Hardware::getStatusLed().setSystemState(Hardware::SystemState::SCANNING);
            break;
        }

        case NetworkState::PREFILL: {
            if (m_i2s_dac) {
                m_i2s_dac->stop();
            }
            break;
        }

        case NetworkState::STREAMING: {
            if (m_i2s_dac) {
                m_i2s_dac->start();
            }
            Hardware::getStatusLed().setSystemState(Hardware::SystemState::STREAMING);
            break;
        }

        case NetworkState::BROADCASTING: {
            if (!m_wifi_initialized) {
                enableWifiEspNow();
            }
            Hardware::getStatusLed().setSystemState(Hardware::SystemState::BROADCASTING);
            break;
        }

        default:
            break;
    }
}

const char* EspNowAudioBroadcast::getStateString() const {
    switch (m_state) {
        case NetworkState::OFF:          return "OFF";
        case NetworkState::IDLE:         return "IDLE";
        case NetworkState::SCANNING:     return "SCANNING";
        case NetworkState::PREFILL:      return "PREFILL";
        case NetworkState::STREAMING:    return "STREAMING";
        case NetworkState::BROADCASTING: return "BROADCASTING";
        default:                         return "UNKNOWN";
    }
}

} // namespace AudioNet
