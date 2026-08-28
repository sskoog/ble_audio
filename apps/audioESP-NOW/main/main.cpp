#include "config.h"
#include "lc3_codec.hpp"
#include "tone_generator.hpp"
#include "i2s_audio.hpp"
#include "status_led.hpp"
#include "espnow_audio_broadcast.hpp"
#include "diagnostics.hpp"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* TAG = "MAIN";

static Codec::Lc3CodecEngine s_lc3_codec;
static Audio::ToneGenerator s_tone_gen(CONFIG_ESPNOW_SAMPLE_RATE_HZ);
static Hardware::I2sAudioDriver* s_i2s_dac = nullptr;
static Hardware::StatusLed* s_status_led = nullptr;
static AudioNet::EspNowAudioBroadcast* s_espnow_broadcast = nullptr;
static Diagnostics::SystemDiagnostics* s_diagnostics = nullptr;

static void diagnostics_task_routine(void* pvParameters) {
    auto* diag = static_cast<Diagnostics::SystemDiagnostics*>(pvParameters);
    while (true) {
        diag->tick();
        vTaskDelay(pdMS_TO_TICKS(100)); // 100 ms tick (10 Hz)
    }
}

static void console_task_routine(void* pvParameters) {
    char line_buf[64];
    int line_idx = 0;

    while (true) {
        int c = getchar();
        if (c == EOF || c == 0xFF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (line_idx > 0) {
                line_buf[line_idx] = '\0';
                ESP_LOGI(TAG, "Console CMD: '%s'", line_buf);

                if (strncmp(line_buf, "rate ", 5) == 0) {
                    uint32_t rate = atoi(line_buf + 5);
                    if (s_espnow_broadcast) {
                        s_espnow_broadcast->setSampleRate(rate);
                    }
                } else if (strncmp(line_buf, "magic ", 6) == 0) {
                    uint16_t magic = strtoul(line_buf + 6, nullptr, 0);
                    if (s_espnow_broadcast) {
                        s_espnow_broadcast->setTestMagicWord(magic);
                        ESP_LOGW(TAG, "Active Magic Word set to: 0x%04X", magic);
                    }
                } else if (strncmp(line_buf, "dur ", 4) == 0 || strncmp(line_buf, "duration ", 9) == 0) {
                    const char* p = (strncmp(line_buf, "dur ", 4) == 0) ? (line_buf + 4) : (line_buf + 9);
                    uint32_t dur_us = 10000;
                    if (strstr(p, "7.5") != nullptr || strstr(p, "7500") != nullptr) {
                        dur_us = 7500;
                    } else {
                        dur_us = 10000;
                    }
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
        } else if (line_idx < sizeof(line_buf) - 1) {
            line_buf[line_idx++] = static_cast<char>(c);
        }
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=========================================================");
    ESP_LOGI(TAG, "   audioESP-NOW: High-Fidelity LC3 Audio Streamer        ");
    ESP_LOGI(TAG, "   Target: ESP32-C6 | Wi-Fi ESP-NOW | Bluetooth DISABLED ");
    ESP_LOGI(TAG, "=========================================================");

    // Step 1: Basic ESP32 Inits (NVS, Config, Pins, LED, DAC structures)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    const system_config_t* cfg = get_system_config();
    ESP_LOGI(TAG, "Booting Node: %s (Role: %s, ID: %u)",
             cfg->device_name, (cfg->node_role == NODE_ROLE_SOURCE) ? "SOURCE" : "SINK", cfg->node_id);

    // Initialize Status LED
    s_status_led = new Hardware::StatusLed();
    s_status_led->init(cfg->status_led_gpio);
    s_status_led->setSystemState(Hardware::SystemState::IDLE);

    // Initialize I2S DAC (for SINK node - pin configurations, etc.)
    if (cfg->node_role == NODE_ROLE_SINK) {
        s_i2s_dac = new Hardware::I2sAudioDriver(cfg->i2s_bclk_gpio, cfg->i2s_ws_gpio, cfg->i2s_dout_gpio, -1, 0);
        s_i2s_dac->init(CONFIG_ESPNOW_SAMPLE_RATE_HZ, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    }

    // Step 2: Initialize ESP-NOW Audio Broadcast Subsystem (node automatically enters state OFF)
    s_espnow_broadcast = new AudioNet::EspNowAudioBroadcast(s_lc3_codec, &s_tone_gen, s_i2s_dac);
    s_espnow_broadcast->init(cfg->node_role);

    // Initialize Diagnostics
    s_diagnostics = new Diagnostics::SystemDiagnostics(*s_espnow_broadcast, *s_status_led);
    s_diagnostics->init();

    // Step 3: Transition from OFF to IDLE (initializes Wi-Fi, ESP-NOW, gates I2S clock, prepares audio buffers)
    s_espnow_broadcast->transitionTo(AudioNet::NetworkState::IDLE);

    // Step 4: Transition to SCANNING (for SINK) or BROADCASTING (for SOURCE)
    if (cfg->node_role == NODE_ROLE_SOURCE) {
        s_espnow_broadcast->transitionTo(AudioNet::NetworkState::BROADCASTING);
    } else {
        s_espnow_broadcast->transitionTo(AudioNet::NetworkState::SCANNING);
    }

    // Step 5: Start Audio Processing Task
    s_espnow_broadcast->startAudioTask();

    // Start Diagnostics Task (1 Hz telemetry logger)
    xTaskCreate(diagnostics_task_routine, "diag_task", 4096, s_diagnostics, 3, nullptr);

    // Start Interactive Console Task
    xTaskCreate(console_task_routine, "console_task", 3072, nullptr, 2, nullptr);

    ESP_LOGI(TAG, "System initialization complete. Audio pipeline running!");
}
