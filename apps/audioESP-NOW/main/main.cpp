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

static const char* TAG = "MAIN";

static Codec::Lc3CodecEngine s_lc3_codec;
static Audio::ToneGenerator s_tone_gen(32000);
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

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=========================================================");
    ESP_LOGI(TAG, "   audioESP-NOW: High-Fidelity LC3 Audio Streamer        ");
    ESP_LOGI(TAG, "   Target: ESP32-C6 | Wi-Fi ESP-NOW | Bluetooth DISABLED ");
    ESP_LOGI(TAG, "=========================================================");

    // Initialize NVS (Required for Wi-Fi and ESP-NOW)
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

    // Initialize I2S DAC (for SINK node)
    if (cfg->node_role == NODE_ROLE_SINK) {
        s_i2s_dac = new Hardware::I2sAudioDriver(cfg->i2s_bclk_gpio, cfg->i2s_ws_gpio, cfg->i2s_dout_gpio, -1, 0);
        s_i2s_dac->init(32000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    }

    // Initialize ESP-NOW Audio Broadcast Subsystem
    s_espnow_broadcast = new AudioNet::EspNowAudioBroadcast(s_lc3_codec, &s_tone_gen, s_i2s_dac);
    s_espnow_broadcast->init(cfg->node_role);

    // Initialize Diagnostics
    s_diagnostics = new Diagnostics::SystemDiagnostics(*s_espnow_broadcast, *s_status_led);
    s_diagnostics->init();

    // Start Network State Machine (Initializes Wi-Fi & ESP-NOW first)
    if (cfg->node_role == NODE_ROLE_SOURCE) {
        s_status_led->setSystemState(Hardware::SystemState::BROADCASTING);
        s_espnow_broadcast->transitionTo(AudioNet::NetworkState::BROADCASTING);
    } else {
        s_status_led->setSystemState(Hardware::SystemState::STREAMING);
        s_espnow_broadcast->transitionTo(AudioNet::NetworkState::SCANNING);
    }

    // Start Audio Processing Task (after Wi-Fi & ESP-NOW are fully initialized)
    s_espnow_broadcast->startAudioTask();

    // Start Diagnostics Task (1 Hz telemetry logger)
    xTaskCreate(diagnostics_task_routine, "diag_task", 4096, s_diagnostics, 3, nullptr);

    ESP_LOGI(TAG, "System initialization complete. Audio pipeline running!");
}
