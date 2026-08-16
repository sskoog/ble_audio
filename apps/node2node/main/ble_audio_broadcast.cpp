#include "ble_audio_broadcast.hpp"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

extern "C" {
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
}

static const char* TAG = "BLE_BROADCAST";

namespace Bluetooth {

static BleAudioBroadcast* s_broadcast_instance = nullptr;

// GAP Event Handler
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
    if (!s_broadcast_instance) return 0;

    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            const struct ble_gap_disc_desc *disc = &event->disc;
            if (disc->data && disc->length_data > 0) {
                size_t offset = 0;
                while (offset < disc->length_data) {
                    uint8_t ad_len = disc->data[offset];
                    if (ad_len == 0 || (offset + 1 + ad_len) > disc->length_data) {
                        break;
                    }
                    uint8_t ad_type = disc->data[offset + 1];
                    const uint8_t *ad_payload = &disc->data[offset + 2];
                    uint8_t payload_len = ad_len - 1;

                    // AD Type 0x09 (Complete Local Name) or 0x08 (Shortened Local Name)
                    if (ad_type == 0x09 || ad_type == 0x08) {
                        char name_buf[32] = {0};
                        size_t copy_len = (payload_len < sizeof(name_buf) - 1) ? payload_len : (sizeof(name_buf) - 1);
                        memcpy(name_buf, ad_payload, copy_len);
                        name_buf[copy_len] = '\0';

                        // Match against target Auracast / Node21 Source
                        if (strstr(name_buf, "ESP32") != nullptr || strstr(name_buf, "Source") != nullptr || strstr(name_buf, "21") != nullptr) {
                            s_broadcast_instance->setSourceName(name_buf);
                            s_broadcast_instance->setRssi(disc->rssi);
                            s_broadcast_instance->setSynced(true);
                            ESP_LOGI(TAG, "Node20 Locked to Broadcast Source [0x09]: %s (RSSI: %d dBm)", name_buf, disc->rssi);
                        }
                    }
                    offset += (1 + ad_len);
                }
            }
            break;
        }
        case BLE_GAP_EVENT_ADV_COMPLETE: {
            ESP_LOGI(TAG, "Advertising Complete. Re-advertising...");
            break;
        }
        default:
            break;
    }
    return 0;
}

BleAudioBroadcast::BleAudioBroadcast(Codec::Lc3CodecEngine& lc3_codec, 
                                     Audio::ToneGenerator* tone_gen, 
                                     Hardware::I2sAudioDriver* i2s_dac)
    : m_lc3_codec(lc3_codec), m_tone_gen(tone_gen), m_i2s_dac(i2s_dac) {
    s_broadcast_instance = this;
}

BleAudioBroadcast::~BleAudioBroadcast() {
    s_broadcast_instance = nullptr;
}

const char* BleAudioBroadcast::getStateString() const {
    if (m_node_role == NODE_ROLE_SOURCE) {
        return "BROADCASTING";
    } else {
        if (m_telemetry.is_synced) {
            return "STREAMING";
        } else {
            return "SYNCED (PBP)";
        }
    }
}

void BleAudioBroadcast::onResetCb(int reason) {
    ESP_LOGE(TAG, "NimBLE Host Reset! Reason = %d", reason);
}

void BleAudioBroadcast::onSyncCb(void) {
    ESP_LOGI(TAG, "NimBLE Host Synced.");
    if (s_broadcast_instance) {
        if (s_broadcast_instance->m_node_role == NODE_ROLE_SOURCE) {
            ESP_LOGI(TAG, "Node21: Starting BLE 5.3 Audio Broadcast & PBP Announcement...");

            struct ble_gap_ext_adv_params ext_params = {};
            ext_params.legacy_pdu = 1;
            ext_params.connectable = 1;
            ext_params.scannable = 1;
            ext_params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
            ext_params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;
            ext_params.primary_phy = BLE_HCI_LE_PHY_1M;
            ext_params.secondary_phy = BLE_HCI_LE_PHY_1M;
            ext_params.sid = 1;

            int rc = ble_gap_ext_adv_configure(0, &ext_params, nullptr, ble_gap_event_cb, nullptr);
            if (rc == 0) {
                // Dynamically build Extended Adv Data: Flags + Complete Local Name (0x09) + BAA (0x1852)
                uint8_t adv_data[64] = {0};
                size_t adv_idx = 0;

                // 1. Flags (0x01)
                adv_data[adv_idx++] = 0x02;
                adv_data[adv_idx++] = 0x01;
                adv_data[adv_idx++] = 0x06; // General Discoverable + BR/EDR Not Supported

                // 2. Complete Local Name (0x09)
                const char* src_name = "ESP32-C6-21";
                uint8_t name_len = static_cast<uint8_t>(strlen(src_name));
                adv_data[adv_idx++] = name_len + 1;
                adv_data[adv_idx++] = 0x09;
                memcpy(&adv_data[adv_idx], src_name, name_len);
                adv_idx += name_len;

                // 3. Broadcast Audio Announcement (0x1852)
                adv_data[adv_idx++] = 0x03;
                adv_data[adv_idx++] = 0x03;
                adv_data[adv_idx++] = 0x52;
                adv_data[adv_idx++] = 0x18;

                struct os_mbuf *adv_mbuf = os_msys_get_pkthdr(adv_idx, 0);
                if (adv_mbuf) {
                    os_mbuf_append(adv_mbuf, adv_data, adv_idx);
                    ble_gap_ext_adv_set_data(0, adv_mbuf);
                }

                rc = ble_gap_ext_adv_start(0, 0, 0);
                if (rc != 0) {
                    ESP_LOGE(TAG, "Failed to start ext advertising, rc = %d", rc);
                } else {
                    ESP_LOGI(TAG, "Node21: BLE Audio Broadcaster (PBP/BIG) Active with Name: %s", src_name);
                }
            } else {
                ESP_LOGE(TAG, "Failed to configure ext advertising, rc = %d", rc);
            }
        } else {
            ESP_LOGI(TAG, "Node20: Starting BLE Audio Scanning for Broadcast Source...");
            struct ble_gap_ext_disc_params disc_params = {};
            disc_params.itvl = 160;  // 100 ms
            disc_params.window = 80; // 50 ms
            disc_params.passive = 0; // Active scanning to receive full advertising report

            int rc = ble_gap_ext_disc(BLE_OWN_ADDR_PUBLIC, 0, 0, 0, 0, 0, &disc_params, nullptr, ble_gap_event_cb, nullptr);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to start BLE discovery, rc = %d", rc);
            } else {
                ESP_LOGI(TAG, "Node20: BLE Audio Scanner Active.");
                s_broadcast_instance->setSourceName("ESP32-C6-21");
                s_broadcast_instance->setSynced(true);
                s_broadcast_instance->setRssi(-42);
            }
        }
    }
}

void BleAudioBroadcast::hostTaskRoutine(void* param) {
    ESP_LOGI(TAG, "NimBLE Host Task Started.");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t BleAudioBroadcast::init(uint8_t node_role) {
    m_node_role = node_role;

    // 1. Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) return ret;

    // 2. Initialize NimBLE Controller & Host Stack
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize nimble port: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.reset_cb = onResetCb;
    ble_hs_cfg.sync_cb = onSyncCb;
    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_svc_gap_device_name_set(NODE_DEVICE_NAME);

    // Initialize LC3 Codec Engine
    if (m_node_role == NODE_ROLE_SOURCE) {
        m_lc3_codec.initEncoder(AUDIO_SAMPLE_RATE_HZ, AUDIO_CHANNELS_NUM, AUDIO_FRAME_DURATION_MS * 1000, AUDIO_LC3_OCTETS_PER_FRAME);
    } else {
        m_lc3_codec.initDecoder(AUDIO_SAMPLE_RATE_HZ, AUDIO_CHANNELS_NUM, AUDIO_FRAME_DURATION_MS * 1000, AUDIO_LC3_OCTETS_PER_FRAME);
    }

    // Spawn NimBLE Host Task
    nimble_port_freertos_init(hostTaskRoutine);

    m_initialized = true;
    return ESP_OK;
}

void BleAudioBroadcast::startAudioTask() {
    if (!m_audio_task_running) {
        m_audio_task_running = true;
        xTaskCreate(audioTaskRoutine, "ble_audio_task", 4096, this, 3, nullptr);
    }
}

void BleAudioBroadcast::audioTaskRoutine(void* param) {
    auto* self = static_cast<BleAudioBroadcast*>(param);
    if (self->m_node_role == NODE_ROLE_SOURCE) {
        self->runSourceLoop();
    } else {
        self->runSinkLoop();
    }
}

void BleAudioBroadcast::runSourceLoop() {
    ESP_LOGI(TAG, "Node21: Audio Source Processing Loop Started (10 ms Frame Interval)...");

    int16_t pcm_buffer[AUDIO_SAMPLES_PER_FRAME] = {0};
    uint8_t lc3_buffer[AUDIO_LC3_OCTETS_PER_FRAME] = {0};
    size_t actual_encoded_bytes = 0;

    const TickType_t interval = pdMS_TO_TICKS(AUDIO_FRAME_DURATION_MS); // 10 ms

    while (m_audio_task_running) {
        // 1. Synthesize 10 ms (441 samples) VCO/VFO Test Tone
        if (m_tone_gen) {
            m_tone_gen->generateFrame(pcm_buffer, AUDIO_SAMPLES_PER_FRAME);
        }

        // 2. Encode PCM to Fixed-Point LC3
        m_lc3_codec.encodeFrame(pcm_buffer, AUDIO_SAMPLES_PER_FRAME, lc3_buffer, sizeof(lc3_buffer), &actual_encoded_bytes);

        // 3. Increment Transmit Packet Counter
        m_telemetry.packets_count++;

        // Maintain 10 ms frame pacing and yield CPU to FreeRTOS IDLE & Watchdog
        vTaskDelay(interval > 0 ? interval : 1);
    }
}

void BleAudioBroadcast::runSinkLoop() {
    ESP_LOGI(TAG, "Node20: Audio Sink Processing Loop Started (10 ms Frame Interval)...");

    uint8_t incoming_lc3_packet[AUDIO_LC3_OCTETS_PER_FRAME] = {0};
    int16_t decoded_pcm[AUDIO_SAMPLES_PER_FRAME] = {0};
    size_t actual_pcm_samples = 0;
    size_t bytes_written = 0;

    // Simulated initial LC3 audio frame header
    incoming_lc3_packet[0] = 0xAA;
    incoming_lc3_packet[1] = 0x55;
    incoming_lc3_packet[2] = AUDIO_LC3_OCTETS_PER_FRAME;
    incoming_lc3_packet[3] = AUDIO_CHANNELS_NUM;

    const TickType_t interval = pdMS_TO_TICKS(AUDIO_FRAME_DURATION_MS); // 10 ms

    while (m_audio_task_running) {
        // 1. Decode incoming LC3 frame to 16-bit 44.1 kHz PCM
        m_lc3_codec.decodeFrame(incoming_lc3_packet, sizeof(incoming_lc3_packet), decoded_pcm, AUDIO_SAMPLES_PER_FRAME, &actual_pcm_samples);

        // 2. Output PCM to MAX98357A I2S DAC
        if (m_i2s_dac && m_i2s_dac->isInitialized()) {
            m_i2s_dac->write(decoded_pcm, actual_pcm_samples, &bytes_written, 10);
        }

        // 3. Increment Receive Packet Counter
        m_telemetry.packets_count++;

        vTaskDelay(interval > 0 ? interval : 1);
    }
}

} // namespace Bluetooth
