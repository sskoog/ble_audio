#include <cstdio>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "lc3_codec.hpp"
#include "tone_generator.hpp"
#include "i2s_audio.hpp"
#include "lcd_display.hpp"
#include "status_led.hpp"
#include "ble_audio_broadcast.hpp"
#include "diagnostics.hpp"
#include "wifi_manager.hpp"
#include "web_dashboard.hpp"

static const char* TAG = "MAIN";

extern "C" void app_main(void)
{
    const system_config_t* cfg = get_system_config();
    ESP_LOGI(TAG, "Starting ESP32-C6 BLE Audio Broadcast Node: %s (Role: %d, Board: %d)...",
             cfg->device_name, cfg->node_role, cfg->board_type);

    // 1. Initialize WS2812B Status LED Controller on GPIO 8
    Hardware::getStatusLed().init(8);

    // 2. Initialize LCD Display if running on Waveshare LCD Board (Node20)
    static Hardware::LcdDisplay lcd_display;
    if (cfg->board_type == BOARD_WAVESHARE_LCD) {
        esp_err_t ret = lcd_display.init();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ST7789 LCD initialization returned: %s", esp_err_to_name(ret));
        }
    }

    // 3. Initialize Hardware & Audio Subsystems according to Role
    static Codec::Lc3CodecEngine lc3_codec;
    static Audio::ToneGenerator tone_gen;
    static Hardware::I2sAudioDriver i2s_dac;

    if (cfg->node_role == NODE_ROLE_SOURCE) {
        // --- Audio SOURCE (Node21) ---
        ESP_LOGI(TAG, "Configuring Node as Audio SOURCE (VCO Tone Generator + Broadcaster + Web Dashboard)...");
        
        // 440 Hz VCO modulated 220-880 Hz by 0.5-2.0 Hz VFO @ 30% amplitude
        tone_gen.init(AUDIO_SAMPLE_RATE_HZ, VCO_NOMINAL_FREQ_HZ, VCO_MIN_FREQ_HZ, VCO_MAX_FREQ_HZ, VCO_AMPLITUDE_PERCENT);
        
        static Bluetooth::BleAudioBroadcast ble_broadcast(lc3_codec, &tone_gen, nullptr);
        ble_broadcast.init(NODE_ROLE_SOURCE);
        ble_broadcast.startAudioTask();
        ble_broadcast.transitionTo(Bluetooth::BluetoothState::BROADCASTING);

        static Diagnostics::DiagnosticMonitor diag_monitor(ble_broadcast, &tone_gen, nullptr);
        diag_monitor.start();

        // 4. Initialize Wi-Fi & Native Web Dashboard for Node21 (SOURCE)
        Network::WifiManager::getInstance().init();
        Web::WebDashboard::getInstance().start();

        Web::WebDashboard::getInstance().setLfoToggleCallback([](bool enabled) {
            ESP_LOGI("APP", "Web Dashboard: LFO Sine Modulation %s", enabled ? "ENABLED" : "DISABLED");
        });

        Web::WebDashboard::getInstance().setVolumeChangeCallback([&ble_broadcast](uint8_t vol_pct) {
            uint8_t vol_setting = (vol_pct * 255) / 100;
            ESP_LOGI("APP", "Web Dashboard: Setting Manual SINK Volume to %d%% (%d/255)", vol_pct, vol_setting);
            for (auto& sink : ble_broadcast.getTrackedSinksMutable()) {
                if (sink.connected && sink.vcs_cp_handle != 0) {
                    ble_broadcast.sendVcsVolumeToSink(sink, vol_setting);
                }
            }
        });

        Web::WebDashboard::getInstance().setGainChangeCallback([&tone_gen](float gain_pct) {
            ESP_LOGI("APP", "Web Dashboard: Setting Tone Gain to %.1f%%", gain_pct);
            tone_gen.set_gain_pct(gain_pct);
        });

    } else {
        // --- Audio SINK (Node20) ---
        ESP_LOGI(TAG, "Configuring Node as Audio SINK (PBP/BIG Receiver + MAX98357A DAC)...");

        // Initialize MAX98357A I2S DAC Driver
        i2s_dac.init(AUDIO_SAMPLE_RATE_HZ, 
                     static_cast<gpio_num_t>(I2S_DAC_BCLK_PIN), 
                     static_cast<gpio_num_t>(I2S_DAC_WS_PIN), 
                     static_cast<gpio_num_t>(I2S_DAC_DOUT_PIN));

        static Bluetooth::BleAudioBroadcast ble_broadcast(lc3_codec, nullptr, &i2s_dac);
        ble_broadcast.init(NODE_ROLE_SINK);
        ble_broadcast.startAudioTask();
        ble_broadcast.transitionTo(Bluetooth::BluetoothState::SCANNING);

        static Diagnostics::DiagnosticMonitor diag_monitor(ble_broadcast, nullptr, &lcd_display);
        diag_monitor.start();
    }

    ESP_LOGI(TAG, "ESP32-C6 BLE Audio Broadcast Node Initialized Successfully.");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
