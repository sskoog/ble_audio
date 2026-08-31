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
        int8_t rssi = -127;
        uint8_t rate = 0;
        if (recv_info && recv_info->rx_ctrl) {
            rssi = recv_info->rx_ctrl->rssi;
            rate = recv_info->rx_ctrl->rate;
        }
        s_instance->onPacketReceived(recv_info ? recv_info->src_addr : nullptr, data, len, rssi, rate);
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
        m_lc3_codec_r.initEncoder(m_telemetry.sample_rate, 1, m_frame_duration_us, m_octets_per_frame);
        if (m_tone_gen) {
            m_tone_gen->setSampleRate(m_telemetry.sample_rate);
        }
        m_tone_gen_r.init(m_telemetry.sample_rate, 440.0f, 220.0f, 880.0f, 50.0f);
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

bool EspNowAudioBroadcast::isMasterTimeValid() const {
    if (m_node_role == NODE_ROLE_SOURCE) {
        return (m_state == NetworkState::BROADCASTING);
    }
    if (m_state != NetworkState::PREFILL && m_state != NetworkState::STREAMING) {
        return false;
    }
    int64_t last_sync = m_last_sync_time_us.load(std::memory_order_relaxed);
    if (last_sync <= 0) {
        return false;
    }
    int64_t now_us = esp_timer_get_time();
    return ((now_us - last_sync) <= 1000000); // Valid only if updated within 1 second
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
        // If audio task is not running, reconfigure immediately.
        // If running, runSourceLoop detects the change synchronously at the frame boundary.
        if (!m_audio_task_running) {
            m_lc3_codec.reconfigureEncoder(sample_rate_hz, frame_len_octets, m_frame_duration_us);
            m_lc3_codec_r.reconfigureEncoder(sample_rate_hz, frame_len_octets, m_frame_duration_us);
            if (m_tone_gen) {
                m_tone_gen->setSampleRate(sample_rate_hz);
            }
            m_tone_gen_r.setSampleRate(sample_rate_hz);
        }
    } else {
        m_lc3_codec.reconfigureDecoder(sample_rate_hz, frame_len_octets, m_frame_duration_us);
        if (m_i2s_dac) {
            m_i2s_dac->reconfigureSampleRate(sample_rate_hz, m_frame_duration_us);
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

    if (m_state != NetworkState::BROADCASTING) {
        transitionTo(NetworkState::BROADCASTING);
    }

    // Check if transitioning to USB active mode
    if (!m_usb_stream_active.load(std::memory_order_relaxed)) {
        ESP_LOGI(TAG, "SOURCE: Detected USB PC Audio Stream! Suspending internal tone generator...");
        Hardware::getStatusLed().setSystemState(Hardware::SystemState::BROADCASTING_STREAM);
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
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(36)); // +9 dBm (36 * 0.25 dBm) for ultra-low thermal dissipation
    int8_t actual_tx_power = 0;
    if (esp_wifi_get_max_tx_power(&actual_tx_power) == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi TX Power set to +%.2f dBm (raw: %d)", actual_tx_power * 0.25f, actual_tx_power);
    }
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
#if SOC_CPU_CORES_NUM > 1
    xTaskCreatePinnedToCore(audioTaskRoutine, "espnow_audio", 8192, this, 3, &s_audio_task_handle, 1);
#else
    xTaskCreate(audioTaskRoutine, "espnow_audio", 8192, this, 3, &s_audio_task_handle);
#endif
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
    static int16_t pcm_buffer_l[MAX_PCM_FRAME_SAMPLES] = {0};
    static int16_t pcm_buffer_r[MAX_PCM_FRAME_SAMPLES] = {0};
    static uint8_t curr_lc3_l[MAX_LC3_FRAME_OCTETS] = {0};
    static uint8_t prev_lc3_l[MAX_LC3_FRAME_OCTETS] = {0};
    static uint8_t curr_lc3_r[MAX_LC3_FRAME_OCTETS] = {0};
    static uint8_t prev_lc3_r[MAX_LC3_FRAME_OCTETS] = {0};
    size_t encoded_bytes_l = 0;
    size_t encoded_bytes_r = 0;
    uint8_t seq = 0;
    int64_t next_frame_target_us = esp_timer_get_time();

    while (m_audio_task_running) {
        if (m_state != NetworkState::BROADCASTING) {
            vTaskDelay(pdMS_TO_TICKS(50));
            next_frame_target_us = esp_timer_get_time();
            continue;
        }

        TickType_t start_tick = xTaskGetTickCount();
        uint32_t active_dur_us = m_frame_duration_us;
        if (active_dur_us == 0) active_dur_us = 10000;

        // Check if USB streaming from PC is currently active
        bool usb_active = m_usb_stream_active.load(std::memory_order_relaxed);
        if (usb_active) {
            int64_t now_usb_us = esp_timer_get_time();
            int64_t last_usb_us = m_last_usb_packet_time_us.load(std::memory_order_relaxed);
            if (now_usb_us - last_usb_us > 250000) { // 250 ms timeout
                m_usb_stream_active.store(false, std::memory_order_relaxed);
                ESP_LOGW(TAG, "SOURCE: USB stream timed out (>250ms). Resuming internal tone generator...");
                Hardware::getStatusLed().setSystemState(Hardware::SystemState::BROADCASTING_TONE);
            } else {
                // USB Stream active: suspend internal encoder loop
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        }

        bool is_stereo = m_is_stereo.load(std::memory_order_relaxed);
        uint32_t current_sr = m_telemetry.sample_rate;
        uint16_t current_len = m_octets_per_frame;

        if (m_lc3_codec.getSampleRate() != current_sr || 
            m_lc3_codec.getFrameDurationUs() != active_dur_us ||
            m_lc3_codec.getOctetsPerFrame() != current_len) {
            m_lc3_codec.reconfigureEncoder(current_sr, current_len, active_dur_us);
            if (m_tone_gen) {
                m_tone_gen->setSampleRate(current_sr);
            }
        }

        if (is_stereo) {
            if (m_lc3_codec_r.getSampleRate() != current_sr ||
                m_lc3_codec_r.getFrameDurationUs() != active_dur_us ||
                m_lc3_codec_r.getOctetsPerFrame() != current_len) {
                m_lc3_codec_r.reconfigureEncoder(current_sr, current_len, active_dur_us);
                m_tone_gen_r.setSampleRate(current_sr);
            }
        }

        size_t samples_to_gen = Codec::calculateRequiredPcmSamples(current_sr, active_dur_us);

        int64_t enc_start_us = esp_timer_get_time();

        if (!is_stereo) {
            // ================= MONO MODE =================
            // 1. Generate single mono tone
            if (m_tone_gen) {
                m_tone_gen->generateFrame(pcm_buffer_l, samples_to_gen);
            }
            // 2. Encode single LC3 frame
            m_lc3_codec.encodeFrame(pcm_buffer_l, samples_to_gen, curr_lc3_l, sizeof(curr_lc3_l), &encoded_bytes_l);
            m_audio_meter.pushFramePcm(pcm_buffer_l, samples_to_gen);
        } else {
            // ================= STEREO MODE =================
            // 1. Generate Left (Channel 0, 220Hz nominal) and Right (Channel 1, 440Hz nominal) tones
            if (m_tone_gen) {
                m_tone_gen->generateFrame(pcm_buffer_l, samples_to_gen);
            }
            m_tone_gen_r.generateFrame(pcm_buffer_r, samples_to_gen);

            // 2. Encode two individual LC3 frames
            m_lc3_codec.encodeFrame(pcm_buffer_l, samples_to_gen, curr_lc3_l, sizeof(curr_lc3_l), &encoded_bytes_l);
            m_lc3_codec_r.encodeFrame(pcm_buffer_r, samples_to_gen, curr_lc3_r, sizeof(curr_lc3_r), &encoded_bytes_r);
            m_audio_meter.pushFramePcm(pcm_buffer_l, samples_to_gen);
        }

        int64_t enc_dur_us = esp_timer_get_time() - enc_start_us;
        m_codec_duration_ring_buffer.push(static_cast<uint32_t>(enc_dur_us));

        uint32_t drop_val = m_simulated_drop_count.load(std::memory_order_relaxed);
        if (drop_val > 0) {
            m_simulated_drop_count.store(0, std::memory_order_relaxed);
            ESP_LOGW(TAG, "SOURCE: [TEST] Deliberately dropping packet Seq #%u to test SINK prev_frame recovery", seq);
            seq += 2;
            memcpy(prev_lc3_l, curr_lc3_l, current_len);
            if (is_stereo) memcpy(prev_lc3_r, curr_lc3_r, current_len);
        } else {
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
            memcpy(pkt_buf + VSAF_HEADER_LEN, curr_lc3_l, current_len);
            memcpy(pkt_buf + VSAF_HEADER_LEN + current_len, prev_lc3_l, current_len);
            esp_now_send(s_broadcast_mac, pkt_buf, pkt_len);

            // Packet 2: Channel 1 (Right - Node 24)
            hdr->seq = seq;
            hdr->cfg = (1 & 0x07) | (sampleRateToCode(current_sr) << 3) | (dur_bit << 6) | (0 << 7);
            hdr->pts_us = pts;
            if (!is_stereo) {
                // Mono: duplicate Ch0 payload into Ch1
                memcpy(pkt_buf + VSAF_HEADER_LEN, curr_lc3_l, current_len);
                memcpy(pkt_buf + VSAF_HEADER_LEN + current_len, prev_lc3_l, current_len);
            } else {
                // Stereo: use distinct Right channel payload
                memcpy(pkt_buf + VSAF_HEADER_LEN, curr_lc3_r, current_len);
                memcpy(pkt_buf + VSAF_HEADER_LEN + current_len, prev_lc3_r, current_len);
            }
            esp_now_send(s_broadcast_mac, pkt_buf, pkt_len);

            seq++;
            memcpy(prev_lc3_l, curr_lc3_l, current_len);
            if (is_stereo) {
                memcpy(prev_lc3_r, curr_lc3_r, current_len);
            }
            m_tx_packets_total.fetch_add(2, std::memory_order_relaxed);
            m_tx_packets_sec.fetch_add(2, std::memory_order_relaxed);
        }

        // ================= HIGH-PRECISION DRIFT-FREE PACING =================
        next_frame_target_us += active_dur_us;
        int64_t now_us = esp_timer_get_time();
        int64_t wait_us = next_frame_target_us - now_us;

        // If behind schedule by more than 2 frames, resynchronize to avoid burst catch-up
        if (wait_us < -static_cast<int64_t>(2 * active_dur_us)) {
            next_frame_target_us = now_us;
            wait_us = 0;
        }

        if (wait_us > 2000) {
            // FreeRTOS sleep for the coarse duration (yielding CPU to other tasks)
            uint32_t sleep_ms = static_cast<uint32_t>((wait_us - 1500) / 1000);
            if (sleep_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(sleep_ms));
            }
        }

        // High-precision hardware timer spin for the remaining sub-millisecond portion
        while (esp_timer_get_time() < next_frame_target_us) {
            esp_rom_delay_us(50);
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
                        if (m_i2s_dac) m_i2s_dac->reconfigureSampleRate(frame_sr, frame_dur);
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
                        if (m_i2s_dac) m_i2s_dac->reconfigureSampleRate(frame_sr, frame_dur);
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
                uint32_t wait_timeout = (m_telemetry.frame_duration_us / 1000) * 2 + 5;
                if (m_i2s_dac && m_i2s_dac->isRunning()) {
                    m_i2s_dac->waitForDmaSlot(wait_timeout);
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
                            m_i2s_dac->reconfigureSampleRate(frame_sr, frame_dur);
                        }
                        transitionTo(NetworkState::PREFILL);
                        break;
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

                    int64_t dec_start_us = esp_timer_get_time();
                    m_lc3_codec.decodeFrame(current_lc3_buf, lc3_len, decoded_pcm, MAX_PCM_FRAME_SAMPLES, &actual_samples,
                                            m_telemetry.sample_rate, m_telemetry.frame_duration_us);
                    int64_t dec_dur_us = esp_timer_get_time() - dec_start_us;
                    m_codec_duration_ring_buffer.push(static_cast<uint32_t>(dec_dur_us));

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

void EspNowAudioBroadcast::onPacketReceived(const uint8_t* mac_addr, const uint8_t* data, int data_len, int8_t rssi, uint8_t rate) {
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

    m_last_rx_rssi.store(rssi, std::memory_order_relaxed);
    m_last_rx_rate.store(rate, std::memory_order_relaxed);
    m_telemetry.rssi_dbm = rssi;

    uint8_t sr_code = (pkt->cfg >> 3) & 0x07;
    uint32_t sample_rate = codeToSampleRate(sr_code);
    uint8_t dur_bit = (pkt->cfg >> 6) & 0x01;
    uint32_t pkt_dur_us = dur_bit ? 7500 : 10000;
    uint16_t frame_len = 120; // Fixed intact 120-octet LC3 frame

    uint32_t pts_curr = pkt->pts_us;
    int64_t now_us = esp_timer_get_time();
    m_master_time_offset_us = static_cast<int64_t>(pts_curr) - now_us;
    m_last_sync_time_us.store(now_us, std::memory_order_relaxed);

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
                ESP_LOGD(TAG, "SINK: [PLC-RECOVERY] Successfully recovered dropped packet Seq #%u (PTS: %lu us, Dur: %.1fms) from prev_frame!",
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

void EspNowAudioBroadcast::resetErrorCounters() {
    if (m_i2s_dac) {
        m_i2s_dac->resetUnderrunCount();
    }
    m_fifo_underrun.store(0, std::memory_order_relaxed);
    m_fifo_overflow.store(0, std::memory_order_relaxed);
    m_prev_frame_recoveries.store(0, std::memory_order_relaxed);
    m_lc3_codec.resetPlcCount();
    m_clock_sync_micro_adjust_count.store(0, std::memory_order_relaxed);
    m_codec_duration_ring_buffer.reset();
}

void EspNowAudioBroadcast::resetStreamingCounters() {
    resetErrorCounters();
    m_rx_packets_total.store(0, std::memory_order_relaxed);
    m_rx_packets_sec.store(0, std::memory_order_relaxed);
    m_tx_packets_total.store(0, std::memory_order_relaxed);
    m_tx_packets_sec.store(0, std::memory_order_relaxed);
    m_last_rx_rssi.store(-127, std::memory_order_relaxed);
    m_last_rx_rate.store(0, std::memory_order_relaxed);
    m_last_sync_time_us.store(0, std::memory_order_relaxed);
}

void EspNowAudioBroadcast::transitionTo(NetworkState new_state) {
    if (m_state == new_state) return;

    NetworkState old_state = m_state;
    const char* old_str = getStateString();
    m_state = new_state;
    const char* new_str = getStateString();

    ESP_LOGI(TAG, "State Machine Transition: [%s] ---> [%s]", old_str, new_str);

    // If transitioning OUT of STREAMING (e.g. broadcast ended or loss of signal), reset error counters
    if (old_state == NetworkState::STREAMING || old_state == NetworkState::BROADCASTING) {
        resetErrorCounters();
    }

    switch (new_state) {
        case NetworkState::OFF: {
            if (m_i2s_dac) {
                m_i2s_dac->stop();
            }
            clear_rx_fifo();
            m_has_last_rx_seq = false;
            m_audio_meter.pushSilence();
            resetStreamingCounters();
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
            resetStreamingCounters();
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
            resetErrorCounters();
            m_last_sync_time_us.store(0, std::memory_order_relaxed);
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
            if (m_usb_stream_active.load(std::memory_order_relaxed)) {
                Hardware::getStatusLed().setSystemState(Hardware::SystemState::BROADCASTING_STREAM);
            } else {
                Hardware::getStatusLed().setSystemState(Hardware::SystemState::BROADCASTING_TONE);
            }
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

const char* EspNowAudioBroadcast::getActiveCodecName() const {
    if (m_node_role == NODE_ROLE_SOURCE) {
        if (m_state == NetworkState::BROADCASTING) {
            return m_is_stereo.load(std::memory_order_relaxed) ? "L3S" : "LC3";
        }
        return " - ";
    } else {
        return (m_state == NetworkState::STREAMING || m_state == NetworkState::PREFILL) ? "LC3" : " - ";
    }
}

const char* EspNowAudioBroadcast::getWifiPhyRateString() const {
    if (m_node_role == NODE_ROLE_SOURCE) {
        return "24M";
    }
    if (m_state == NetworkState::OFF || m_state == NetworkState::IDLE) {
        return " - ";
    }
    uint8_t rate = m_last_rx_rate.load(std::memory_order_relaxed);
    switch (rate) {
        case WIFI_PHY_RATE_1M_L:   return " 1M";
        case WIFI_PHY_RATE_2M_L:   
        case WIFI_PHY_RATE_2M_S:   return " 2M";
        case WIFI_PHY_RATE_5M_L:   
        case WIFI_PHY_RATE_5M_S:   return "5.5";
        case WIFI_PHY_RATE_11M_L:  
        case WIFI_PHY_RATE_11M_S:  return "11M";
        case WIFI_PHY_RATE_6M:     return " 6M";
        case WIFI_PHY_RATE_9M:     return " 9M";
        case WIFI_PHY_RATE_12M:    return "12M";
        case WIFI_PHY_RATE_18M:    return "18M";
        case WIFI_PHY_RATE_24M:    return "24M";
        case WIFI_PHY_RATE_36M:    return "36M";
        case WIFI_PHY_RATE_48M:    return "48M";
        case WIFI_PHY_RATE_54M:    return "54M";
        case WIFI_PHY_RATE_MCS0_LGI:
        case WIFI_PHY_RATE_MCS0_SGI: return "MC0";
        case WIFI_PHY_RATE_MCS1_LGI:
        case WIFI_PHY_RATE_MCS1_SGI: return "MC1";
        case WIFI_PHY_RATE_MCS2_LGI:
        case WIFI_PHY_RATE_MCS2_SGI: return "MC2";
        case WIFI_PHY_RATE_MCS3_LGI:
        case WIFI_PHY_RATE_MCS3_SGI: return "MC3";
        case WIFI_PHY_RATE_MCS4_LGI:
        case WIFI_PHY_RATE_MCS4_SGI: return "MC4";
        case WIFI_PHY_RATE_MCS5_LGI:
        case WIFI_PHY_RATE_MCS5_SGI: return "MC5";
        case WIFI_PHY_RATE_MCS6_LGI:
        case WIFI_PHY_RATE_MCS6_SGI: return "MC6";
        case WIFI_PHY_RATE_MCS7_LGI:
        case WIFI_PHY_RATE_MCS7_SGI: return "MC7";
        default: return (m_state == NetworkState::STREAMING) ? "24M" : " - ";
    }
}

} // namespace AudioNet
