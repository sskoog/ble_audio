#include "espnow_audio_broadcast.hpp"
#include "config.h"
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
    uint8_t data[80];
    uint8_t len;
    uint8_t seq;
};

static constexpr size_t LC3_RX_FIFO_CAPACITY = 16;
static Lc3RxFrame s_rx_fifo[LC3_RX_FIFO_CAPACITY];
static size_t s_rx_fifo_head = 0;
static size_t s_rx_fifo_tail = 0;
static size_t s_rx_fifo_count = 0;
static portMUX_TYPE s_fifo_mux = portMUX_INITIALIZER_UNLOCKED;

static inline bool push_rx_lc3_frame(const uint8_t* data, size_t len, uint8_t seq) {
    if (!data || len == 0 || len > 80) return false;
    taskENTER_CRITICAL(&s_fifo_mux);
    if (s_rx_fifo_count >= LC3_RX_FIFO_CAPACITY) {
        // Drop oldest frame on overflow
        s_rx_fifo_tail = (s_rx_fifo_tail + 1) % LC3_RX_FIFO_CAPACITY;
        s_rx_fifo_count--;
    }
    s_rx_fifo[s_rx_fifo_head].len = static_cast<uint8_t>(len);
    s_rx_fifo[s_rx_fifo_head].seq = seq;
    memcpy(s_rx_fifo[s_rx_fifo_head].data, data, len);
    s_rx_fifo_head = (s_rx_fifo_head + 1) % LC3_RX_FIFO_CAPACITY;
    s_rx_fifo_count++;
    taskEXIT_CRITICAL(&s_fifo_mux);
    return true;
}

static inline bool pop_rx_lc3_frame(uint8_t* out_data, size_t* out_len, uint8_t* out_seq = nullptr) {
    if (!out_data || !out_len) return false;
    taskENTER_CRITICAL(&s_fifo_mux);
    if (s_rx_fifo_count == 0) {
        taskEXIT_CRITICAL(&s_fifo_mux);
        return false;
    }
    *out_len = s_rx_fifo[s_rx_fifo_tail].len;
    if (out_seq) *out_seq = s_rx_fifo[s_rx_fifo_tail].seq;
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

static void onEspNowSendCb(const esp_now_send_info_t *info, esp_now_send_status_t status) {
    // Optional TX telemetry callback
}

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
    m_telemetry.sample_rate = 32000;
    m_telemetry.channels = 1;
    m_telemetry.bitrate_kbps = 64;

    if (m_node_role == NODE_ROLE_SOURCE) {
        m_lc3_codec.initEncoder(32000, 1, 10000, 80);
    } else { // SINK
        m_lc3_codec.initDecoder(32000, 1, 10000, 80);
    }

    ESP_LOGI(TAG, "EspNowAudioBroadcast initialized (Role: %s, Node ID: %u)",
             (node_role == NODE_ROLE_SOURCE) ? "SOURCE" : "SINK", m_node_id);
    return ESP_OK;
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
    esp_wifi_config_11b_rate(WIFI_IF_STA, true); // Disable slow 11b basic rate
    esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_24M); // 24Mbps OFDM
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); // Full power / unthrottled streaming
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "Local STA MAC: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(onEspNowSendCb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(onEspNowRecvCb));

    // Add broadcast peer
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
    ESP_LOGI(TAG, "ESP-NOW Audio Source Loop Started (Target: Exact 100.0 fps)...");

    static int16_t pcm_buffer[320] = {0};
    static uint8_t curr_lc3[80] = {0};
    static uint8_t prev_lc3[80] = {0};
    size_t encoded_bytes = 0;
    uint8_t seq = 0;

    int64_t next_frame_us = esp_timer_get_time() + 10000;

    while (m_audio_task_running) {
        int64_t now_us = esp_timer_get_time();
        int64_t wait_us = next_frame_us - now_us;
        if (wait_us > 2000) {
            vTaskDelay(pdMS_TO_TICKS(wait_us / 1000 - 1));
        }
        while (esp_timer_get_time() < next_frame_us) {
            esp_rom_delay_us(10);
        }
        next_frame_us += 10000;

        if (m_state == NetworkState::BROADCASTING) {
            if (m_tone_gen) {
                m_tone_gen->generateFrame(pcm_buffer, 320);
            }
            m_lc3_codec.encodeFrame(pcm_buffer, 320, curr_lc3, sizeof(curr_lc3), &encoded_bytes);
            m_audio_meter.pushFramePcm(pcm_buffer, 320);

            // Assemble VSAF Packet with Recipient Tag (0xFF = broadcast to all SINKs)
            EspNowAudioPacket pkt;
            pkt.magic = 0xE501;
            pkt.target_node_id = 0xFF; // Broadcast to all SINKs (or specific SINK node_id)
            pkt.source_node_id = m_node_id;
            pkt.seq = seq++;
            pkt.flags = m_telemetry.is_muted ? 0x01 : 0x00;
            pkt.frame_len = 80;
            memcpy(pkt.prev_frame, prev_lc3, 80);
            memcpy(pkt.curr_frame, curr_lc3, 80);

            // Transmit over high-speed broadcast
            esp_now_send(s_broadcast_mac, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
            memcpy(prev_lc3, curr_lc3, 80);

            m_tx_packets_total.fetch_add(1, std::memory_order_relaxed);
            m_tx_packets_sec.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void EspNowAudioBroadcast::runSinkLoop() {
    ESP_LOGI(TAG, "ESP-NOW Audio Sink Processing Loop Started (Dual-Slot DMA / 5-Packet Jitter Lock-In)...");

    static uint8_t current_lc3_buf[80] = {0};
    static int16_t decoded_pcm[320] = {0};
    static int16_t stereo_pcm[640] = {0};
    size_t actual_samples = 0;
    size_t bytes_written = 0;

    enum class SinkDmaState {
        IDLE_WAIT_LOCKIN_5_FRAMES,
        STREAMING_ACTIVE
    };

    SinkDmaState dma_state = SinkDmaState::IDLE_WAIT_LOCKIN_5_FRAMES;
    uint32_t consecutive_plc_count = 0;

    while (m_audio_task_running) {
        uint32_t vol_scale = m_telemetry.is_muted ? 0 : ((static_cast<uint32_t>(m_telemetry.volume_percent) * 255) / 100);

        // --- STATE 1: IDLE / LOCK-IN (WAIT UNTIL FIFO HAS AT LEAST 5 PACKETS) ---
        if (dma_state == SinkDmaState::IDLE_WAIT_LOCKIN_5_FRAMES) {
            size_t buffered = get_rx_fifo_count();
            if (buffered < 5) {
                // Wait for producer notification on incoming packets
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(40));
                continue;
            }

            ESP_LOGI(TAG, "SINK: Lock-in threshold reached (FIFO = %zu pkts). Pre-loading dual DMA descriptors...", buffered);

            // Pop & decode Frame 1 for DMA Descriptor 0
            size_t lc3_len = 0;
            uint8_t seq = 0;
            if (pop_rx_lc3_frame(current_lc3_buf, &lc3_len, &seq) && lc3_len == 80) {
                m_lc3_codec.decodeFrame(current_lc3_buf, lc3_len, decoded_pcm, 320, &actual_samples);
                for (size_t i = 0; i < actual_samples; ++i) {
                    int16_t sample = static_cast<int16_t>((static_cast<int32_t>(decoded_pcm[i]) * vol_scale) / 255);
                    stereo_pcm[2 * i]     = sample; // Left
                    stereo_pcm[2 * i + 1] = sample; // Right (Mono duplicated)
                }
                m_audio_meter.pushFramePcm(decoded_pcm, actual_samples);
                if (m_i2s_dac) {
                    m_i2s_dac->preload(stereo_pcm, actual_samples * 2 * sizeof(int16_t), &bytes_written);
                }
            }

            // Pop & decode Frame 2 for DMA Descriptor 1
            if (pop_rx_lc3_frame(current_lc3_buf, &lc3_len, &seq) && lc3_len == 80) {
                m_lc3_codec.decodeFrame(current_lc3_buf, lc3_len, decoded_pcm, 320, &actual_samples);
                for (size_t i = 0; i < actual_samples; ++i) {
                    int16_t sample = static_cast<int16_t>((static_cast<int32_t>(decoded_pcm[i]) * vol_scale) / 255);
                    stereo_pcm[2 * i]     = sample;
                    stereo_pcm[2 * i + 1] = sample;
                }
                m_audio_meter.pushFramePcm(decoded_pcm, actual_samples);
                if (m_i2s_dac) {
                    m_i2s_dac->preload(stereo_pcm, actual_samples * 2 * sizeof(int16_t), &bytes_written);
                    // Both DMA descriptors are pre-loaded! Start I2S hardware clocks now!
                    m_i2s_dac->start();
                }
            }

            consecutive_plc_count = 0;
            transitionTo(NetworkState::STREAMING);
            dma_state = SinkDmaState::STREAMING_ACTIVE;
            ESP_LOGI(TAG, "SINK: I2S Clocks started! 3 packets remain in FIFO cushion (buffered = %zu).", get_rx_fifo_count());
            continue;
        }

        // --- STATE 2: STREAMING ACTIVE (DMA INTERRUPT-DRIVEN CONSUMER PULL) ---
        if (dma_state == SinkDmaState::STREAMING_ACTIVE) {
            // Block until 1 DMA descriptor finishes playing (driven by DMA on_sent ISR)
            if (m_i2s_dac && m_i2s_dac->isRunning()) {
                m_i2s_dac->waitForDmaSlot(25);
            }

            size_t lc3_len = 0;
            uint8_t seq = 0;
            bool has_packet = pop_rx_lc3_frame(current_lc3_buf, &lc3_len, &seq);

            if (has_packet && lc3_len == 80) {
                consecutive_plc_count = 0; // Reset consecutive PLC watchdog
                m_lc3_codec.decodeFrame(current_lc3_buf, lc3_len, decoded_pcm, 320, &actual_samples);

                for (size_t i = 0; i < actual_samples; ++i) {
                    int16_t sample = static_cast<int16_t>((static_cast<int32_t>(decoded_pcm[i]) * vol_scale) / 255);
                    stereo_pcm[2 * i]     = sample;
                    stereo_pcm[2 * i + 1] = sample;
                }
                m_audio_meter.pushFramePcm(decoded_pcm, actual_samples);

                if (m_i2s_dac && m_i2s_dac->isInitialized()) {
                    m_i2s_dac->write(stereo_pcm, actual_samples * 2 * sizeof(int16_t), &bytes_written, 20);
                }
            } else {
                // FIFO was empty: increment consecutive PLC watchdog
                m_fifo_underrun.fetch_add(1, std::memory_order_relaxed);
                consecutive_plc_count++;

                if (consecutive_plc_count < 5) {
                    // Conceal frame using LC3 Packet Loss Concealment (PLC)
                    m_lc3_codec.decodeFrame(nullptr, 0, decoded_pcm, 320, &actual_samples);
                    for (size_t i = 0; i < 320; ++i) {
                        int16_t sample = static_cast<int16_t>((static_cast<int32_t>(decoded_pcm[i]) * vol_scale) / 255);
                        stereo_pcm[2 * i]     = sample;
                        stereo_pcm[2 * i + 1] = sample;
                    }
                    m_audio_meter.pushFramePcm(decoded_pcm, 320);

                    if (m_i2s_dac && m_i2s_dac->isInitialized()) {
                        m_i2s_dac->write(stereo_pcm, 320 * 2 * sizeof(int16_t), &bytes_written, 20);
                    }
                } else {
                    // 5 consecutive missing frames (50 ms of loss) -> Gate I2S clocks & transition to IDLE
                    ESP_LOGW(TAG, "SINK: Reached 5 consecutive PLC frames. Gating I2S clocks and entering IDLE...");
                    m_audio_meter.pushSilence();
                    if (m_i2s_dac) {
                        m_i2s_dac->stop(); // Gated: Stops BCLK/WS and puts MAX98357A into silent standby
                    }
                    dma_state = SinkDmaState::IDLE_WAIT_LOCKIN_5_FRAMES;
                    consecutive_plc_count = 0;
                    m_has_last_rx_seq = false;
                    transitionTo(NetworkState::SCANNING);
                }
            }
        }
    }
}

void EspNowAudioBroadcast::onPacketReceived(const uint8_t* mac_addr, const uint8_t* data, int data_len) {
    if (m_node_role != NODE_ROLE_SINK) return;
    if (data_len < static_cast<int>(sizeof(EspNowAudioPacket))) return;

    const auto* pkt = reinterpret_cast<const EspNowAudioPacket*>(data);
    if (pkt->magic != 0xE501) return;

    // Fast receiver filtering: check recipient address tag (0xFF = broadcast, or matches local node_id)
    if (pkt->target_node_id != 0xFF && pkt->target_node_id != m_node_id) {
        return; // Discard packet intended for another SINK node
    }

    m_telemetry.rssi_dbm = -26;

    // Check packet sequence number & de-duplicate
    if (m_has_last_rx_seq) {
        if (pkt->seq == m_last_rx_seq) {
            return; // Exact duplicate of already processed packet
        }

        uint8_t expected = m_last_rx_seq + 1;
        if (pkt->seq == static_cast<uint8_t>(expected + 1)) {
            // Exactly 1 packet was dropped! Recover frame N-1 from pkt->prev_frame!
            if (push_rx_lc3_frame(pkt->prev_frame, 80, expected)) {
                m_rx_packets_total.fetch_add(1, std::memory_order_relaxed);
                m_rx_packets_sec.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // Push unique current frame N
    if (push_rx_lc3_frame(pkt->curr_frame, 80, pkt->seq)) {
        m_rx_packets_total.fetch_add(1, std::memory_order_relaxed);
        m_rx_packets_sec.fetch_add(1, std::memory_order_relaxed);

        m_last_rx_seq = pkt->seq;
        m_has_last_rx_seq = true;

        if (s_audio_task_handle) {
            vTaskNotifyGiveFromISR(s_audio_task_handle, nullptr);
        }
    } else {
        m_fifo_overflow.fetch_add(1, std::memory_order_relaxed);
    }
}

void EspNowAudioBroadcast::transitionTo(NetworkState new_state) {
    if (m_state == new_state) return;
    ESP_LOGI(TAG, "Network State Transition: [%s] ---> [%s]", getStateString(),
             (new_state == NetworkState::SCANNING) ? "SCANNING" :
             (new_state == NetworkState::STREAMING) ? "STREAMING" :
             (new_state == NetworkState::BROADCASTING) ? "BROADCASTING" : "OFF");

    m_state = new_state;

    if (!m_wifi_initialized && (new_state == NetworkState::BROADCASTING || new_state == NetworkState::SCANNING)) {
        enableWifiEspNow();
    }
}

const char* EspNowAudioBroadcast::getStateString() const {
    switch (m_state) {
        case NetworkState::OFF: return "OFF";
        case NetworkState::SCANNING: return "LISTENING";
        case NetworkState::CONNECTED: return "CONNECTED";
        case NetworkState::BROADCASTING: return "BROADCASTING";
        case NetworkState::STREAMING: return "STREAMING";
        default: return "UNKNOWN";
    }
}

} // namespace AudioNet
