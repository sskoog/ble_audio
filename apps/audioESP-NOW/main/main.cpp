#include "config.h"
#include "espnow_audio_broadcast.hpp"
#include "lc3_codec.hpp"
#include "tone_generator.hpp"
#include "i2s_audio.hpp"
#include "status_led.hpp"
#include "diagnostics.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_vfs_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

static const char* TAG = "MAIN";

static Hardware::StatusLed*        s_status_led = nullptr;
static Hardware::I2sAudioDriver*    s_i2s_dac = nullptr;
static Codec::Lc3CodecEngine       s_lc3_codec;
static Audio::ToneGenerator        s_tone_gen;
static AudioNet::EspNowAudioBroadcast* s_espnow_broadcast = nullptr;
static Diagnostics::SystemDiagnostics* s_diagnostics = nullptr;

static void diagnostics_task_routine(void* pvParameters) {
    while (true) {
        if (s_diagnostics) {
            s_diagnostics->tick();
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // 10 Hz tick
    }
}

static void console_task_routine(void* pvParameters) {
    uint8_t rx_buf[512];
    static uint8_t ring_buf[1024];
    size_t ring_len = 0;
    char line_buf[64];
    int line_idx = 0;

    const size_t PACKET_SIZE = sizeof(AudioNet::EspNowAudioPacket); // 248 bytes

    while (true) {
        int n = uart_read_bytes(UART_NUM_0, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(5));
        if (n <= 0) {
            continue;
        }

        for (int i = 0; i < n; ++i) {
            uint8_t byte = rx_buf[i];

            if (ring_len < sizeof(ring_buf)) {
                ring_buf[ring_len++] = byte;
            } else {
                memmove(ring_buf, ring_buf + 1, ring_len - 1);
                ring_buf[ring_len - 1] = byte;
            }

            // ASCII command collection if not in binary sync
            if (ring_len < 2 || (ring_buf[0] != 0x37 || ring_buf[1] != 0x13)) {
                if (byte == '\r' || byte == '\n') {
                    if (line_idx > 0) {
                        line_buf[line_idx] = '\0';
                        ESP_LOGI(TAG, "Console CMD: '%s'", line_buf);

                        if (strncmp(line_buf, "rate ", 5) == 0) {
                            uint32_t rate = atoi(line_buf + 5);
                            if (s_espnow_broadcast) {
                                s_espnow_broadcast->setSampleRate(rate);
                            }
                        } else if (strncmp(line_buf, "ch ", 3) == 0 || strncmp(line_buf, "channel ", 8) == 0) {
                            const char* p = (strncmp(line_buf, "ch ", 3) == 0) ? (line_buf + 3) : (line_buf + 8);
                            uint8_t ch = atoi(p);
                            if (s_espnow_broadcast) {
                                s_espnow_broadcast->setTargetChannel(ch);
                                ESP_LOGW(TAG, "Target Listening Audio Channel set to: Ch %u", ch);
                            }
                        } else if (strncmp(line_buf, "magic ", 6) == 0) {
                            uint16_t magic = strtoul(line_buf + 6, nullptr, 0);
                            if (s_espnow_broadcast) {
                                s_espnow_broadcast->setTestMagicWord(magic);
                                ESP_LOGW(TAG, "Active Magic Word set to: 0x%04X", magic);
                            }
                        } else if (strncmp(line_buf, "dur ", 4) == 0 || strncmp(line_buf, "duration ", 9) == 0) {
                            const char* p = (strncmp(line_buf, "dur ", 4) == 0) ? (line_buf + 4) : (line_buf + 9);
                            uint32_t dur_us = (strstr(p, "7.5") != nullptr || strstr(p, "7500") != nullptr) ? 7500 : 10000;
                            if (s_espnow_broadcast) {
                                s_espnow_broadcast->setFrameDuration(dur_us);
                            }
                        } else if (strcmp(line_buf, "drop") == 0) {
                            if (s_espnow_broadcast) {
                                s_espnow_broadcast->triggerSimulatedPacketDrop();
                            }
                        }
                        line_idx = 0;
                    }
                } else if (line_idx < sizeof(line_buf) - 1 && byte >= 32 && byte <= 126) {
                    line_buf[line_idx++] = static_cast<char>(byte);
                }
            } else {
                line_idx = 0;
            }
        }

        // Process all full 248-byte VSAF packets in ring buffer
        while (ring_len >= PACKET_SIZE) {
            if (ring_buf[0] == 0x37 && ring_buf[1] == 0x13) {
                if (s_espnow_broadcast) {
                    s_espnow_broadcast->processUsbVsafPacket(ring_buf, PACKET_SIZE);
                }
                memmove(ring_buf, ring_buf + PACKET_SIZE, ring_len - PACKET_SIZE);
                ring_len -= PACKET_SIZE;
            } else {
                // Slide forward 1 byte to find valid magic sync word
                memmove(ring_buf, ring_buf + 1, ring_len - 1);
                ring_len--;
            }
        }
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=========================================================");
    ESP_LOGI(TAG, "   audioESP-NOW: High-Fidelity LC3 Audio Streamer        ");
    ESP_LOGI(TAG, "   Target: ESP32-C6 | Wi-Fi ESP-NOW | Bluetooth DISABLED ");
    ESP_LOGI(TAG, "=========================================================");

    // Install high-performance UART0 driver for 2000000 baud (2 Mbaud) transparent binary ingestion
    uart_config_t uart_config = {
        .baud_rate = 2000000,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_NUM_0, 4096, 512, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &uart_config);
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    uart_vfs_dev_use_driver(UART_NUM_0);
#else
    esp_vfs_dev_uart_use_driver(UART_NUM_0);
#endif

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    const system_config_t* cfg = get_system_config();
    ESP_LOGI(TAG, "Booting Node: %s (Role: %s, ID: %u)",
             cfg->device_name, (cfg->node_role == NODE_ROLE_SOURCE) ? "SOURCE" : "SINK", cfg->node_id);

    s_status_led = new Hardware::StatusLed();
    s_status_led->init(cfg->status_led_gpio);
    s_status_led->setSystemState(Hardware::SystemState::IDLE);

    if (cfg->node_role == NODE_ROLE_SINK) {
        s_i2s_dac = new Hardware::I2sAudioDriver(cfg->i2s_bclk_gpio, cfg->i2s_ws_gpio, cfg->i2s_dout_gpio, -1, 0);
        s_i2s_dac->init(CONFIG_ESPNOW_SAMPLE_RATE_HZ, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    }

    s_espnow_broadcast = new AudioNet::EspNowAudioBroadcast(s_lc3_codec, &s_tone_gen, s_i2s_dac);
    s_espnow_broadcast->init(cfg->node_role);

    s_diagnostics = new Diagnostics::SystemDiagnostics(*s_espnow_broadcast, *s_status_led);
    s_diagnostics->init();

    s_espnow_broadcast->transitionTo(AudioNet::NetworkState::IDLE);

    if (cfg->node_role == NODE_ROLE_SOURCE) {
        s_espnow_broadcast->transitionTo(AudioNet::NetworkState::BROADCASTING);
    } else {
        s_espnow_broadcast->transitionTo(AudioNet::NetworkState::SCANNING);
    }

    s_espnow_broadcast->startAudioTask();

    xTaskCreate(diagnostics_task_routine, "diagnostics", 4096, nullptr, 1, nullptr);
    xTaskCreate(console_task_routine, "console", 4096, nullptr, 2, nullptr);

    ESP_LOGI(TAG, "audioESP-NOW node initialized and running.");
}
