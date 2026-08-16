#include "diagnostics.hpp"
#include "status_led.hpp"
#include "config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_clk_tree.h"
#include "esp_pm.h"
#include "driver/temperature_sensor.h"
#include <cstring>
#include <cstdio>

static const char* TAG = "HBEAT";

namespace Diagnostics {

float getCPUfreq_MHz() {
    uint32_t freq_hz = 0;
    if (esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_APPROX, &freq_hz) == ESP_OK) {
        return static_cast<float>(freq_hz) / 1000000.0f;
    }
    return 160.0f; // Fallback nominal ESP32-C6 CPU clock
}

esp_err_t setCPUfreq_MHz(int mhz) {
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = mhz,
        .min_freq_mhz = mhz,
        .light_sleep_enable = false
    };
    return esp_pm_configure(&pm_config);
#else
    ESP_LOGW(TAG, "PM not enabled in sdkconfig (CONFIG_PM_ENABLE=y required for dynamic frequency switching)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static temperature_sensor_handle_t temp_handle = nullptr;

static int calculateCpuUsagePct() {
    static uint32_t last_idle_time = 0;
    static uint32_t last_total_time = 0;

    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    TaskStatus_t* task_status_array = static_cast<TaskStatus_t*>(pvPortMalloc(task_count * sizeof(TaskStatus_t)));
    if (!task_status_array) return 0;

    uint32_t total_time = 0;
    uint32_t idle_time = 0;

    UBaseType_t num_tasks = uxTaskGetSystemState(task_status_array, task_count, &total_time);

    for (UBaseType_t i = 0; i < num_tasks; i++) {
        if (strcmp(task_status_array[i].pcTaskName, "IDLE") == 0 ||
            strcmp(task_status_array[i].pcTaskName, "IDLE0") == 0) {
            idle_time += task_status_array[i].ulRunTimeCounter;
        }
    }
    vPortFree(task_status_array);

    uint32_t delta_total = total_time - last_total_time;
    uint32_t delta_idle = idle_time - last_idle_time;

    last_total_time = total_time;
    last_idle_time = idle_time;

    if (delta_total == 0) return 0;

    float cpu_busy_pct = (1.0f - (static_cast<float>(delta_idle) / static_cast<float>(delta_total))) * 100.0f;
    if (cpu_busy_pct < 0.0f) cpu_busy_pct = 0.0f;
    if (cpu_busy_pct > 100.0f) cpu_busy_pct = 100.0f;

    return static_cast<int>(cpu_busy_pct + 0.5f);
}

DiagnosticMonitor::DiagnosticMonitor(Bluetooth::BleAudioBroadcast& ble_broadcast, 
                                     Audio::ToneGenerator* tone_gen, 
                                     Hardware::LcdDisplay* lcd_display)
    : m_ble_broadcast(ble_broadcast), m_tone_gen(tone_gen), m_lcd_display(lcd_display) {}

DiagnosticMonitor::~DiagnosticMonitor() {
    stop();
}

esp_err_t DiagnosticMonitor::start() {
    // 1. Initialize onboard ESP32-C6 Temperature Sensor
    temperature_sensor_config_t temp_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    esp_err_t ret = temperature_sensor_install(&temp_cfg, &temp_handle);
    if (ret == ESP_OK) {
        temperature_sensor_enable(temp_handle);
    } else {
        ESP_LOGW(TAG, "Temperature sensor install failed: %s", esp_err_to_name(ret));
    }

    // 2. Create 1 Hz FreeRTOS Diagnostics Task
    m_is_running = true;
    BaseType_t task_ret = xTaskCreate(taskRoutine, "diag_1hz_task", DIAGNOSTICS_TASK_STACK_SIZE, this, DIAGNOSTICS_TASK_PRIORITY, &m_task_handle);
    if (task_ret == pdPASS) {
        ESP_LOGI(TAG, "1 Hz Diagnostic Telemetry Task Started.");
        return ESP_OK;
    }
    m_is_running = false;
    return ESP_FAIL;
}

void DiagnosticMonitor::stop() {
    if (m_is_running && m_task_handle) {
        vTaskDelete(m_task_handle);
        m_task_handle = nullptr;
        m_is_running = false;
    }
    if (temp_handle) {
        temperature_sensor_disable(temp_handle);
        temperature_sensor_uninstall(temp_handle);
        temp_handle = nullptr;
    }
}

void DiagnosticMonitor::taskRoutine(void* pvParameters) {
    auto* instance = static_cast<DiagnosticMonitor*>(pvParameters);
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t interval_ticks = pdMS_TO_TICKS(DIAGNOSTICS_TASK_INTERVAL_MS); // 1000 ms

    while (instance->m_is_running) {
        instance->printDiagnostics();
        vTaskDelayUntil(&last_wake_time, interval_ticks);
    }

    vTaskDelete(NULL);
}

void DiagnosticMonitor::printDiagnostics() {
    m_loop_count++;

    // 1. Temperature Sensor Reading
    int cpu_temp_c = 0;
    if (temp_handle) {
        float raw_temp = 0.0f;
        if (temperature_sensor_get_celsius(temp_handle, &raw_temp) == ESP_OK) {
            cpu_temp_c = static_cast<int>(raw_temp + 0.5f);
        }
    }

    // 2. System Metrics
    uint32_t uptime_sec = m_loop_count;
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_free_heap = esp_get_minimum_free_heap_size();
    int cpu_usage_pct = calculateCpuUsagePct();

    // 3. Bluetooth Stream Metrics
    const auto& stream = m_ble_broadcast.getStreamTelemetry();
    const char* bt_state = m_ble_broadcast.getStateString();

    const system_config_t* cfg = get_system_config();

    ESP_LOGI("", "===== [%s] heartbeat #%lu | Uptime: %lu s =====", cfg->device_name, m_loop_count, uptime_sec);
    ESP_LOGI("SYS", "CPU: %d%% @ %.0f MHz | Temp: %d C | Heap: %lu KB (%lu KB Min)",
             cpu_usage_pct, getCPUfreq_MHz(), cpu_temp_c, free_heap / 1024, min_free_heap / 1024);
    ESP_LOGI("BT", "Role: %s | State: %s | Pkts: %lu | RSSI: %d dBm | BIS ID: %u",
             (cfg->node_role == NODE_ROLE_SOURCE) ? "SOURCE (Broadcaster)" : "SINK (Receiver)",
             bt_state, stream.packets_count, stream.rssi_dbm, stream.bis_index);
    ESP_LOGI("AUDIO", "Codec: %s | %d-ch %lu kbps | %u-bit @ %.1f kHz",
             stream.codec_name.c_str(), stream.channels, stream.bitrate_kbps,
             stream.bit_depth, static_cast<float>(stream.sample_rate) / 1000.0f);

    if (cfg->node_role == NODE_ROLE_SOURCE && m_tone_gen) {
        ESP_LOGI("SOURCE", "Gain %.0f%% (%.1f dB)  | Tone: %.1f Hz | VFO: %.2f Hz",
            m_tone_gen->get_gain_pct(), m_tone_gen->get_gain_dB(), m_tone_gen->getCurrentFrequency(), m_tone_gen->getModulationRate());
    }

    // 4. Render to 1.47" ST7789 LCD Console if available (Node20)
    if (m_lcd_display && m_lcd_display->isInitialized()) {
        char buf[128];

        snprintf(buf, sizeof(buf), "NODE: %s | UP: %lus", 
            (cfg->node_role == NODE_ROLE_SOURCE) ? "SOURCE" : "SINK", uptime_sec);
        m_lcd_display->printLine(0, buf, Hardware::COLOR_WHITE);

        snprintf(buf, sizeof(buf), "CPU: %d%% @ %.0f MHz | %d C | %luKB", 
            cpu_usage_pct, getCPUfreq_MHz(), cpu_temp_c, free_heap / 1024);
        m_lcd_display->printLine(1, buf, Hardware::COLOR_GREEN);
        
        // Display Bluetooth status and Source 0x09 ID Name
        snprintf(buf, sizeof(buf), "BT: %d dBm | %s | BIS: %s", stream.rssi_dbm, bt_state, stream.source_name.c_str());
        m_lcd_display->printLine(2, buf, Hardware::COLOR_BLUE);

        snprintf(buf, sizeof(buf), "AUDIO: %.1f kHz %u-bit %s", static_cast<float>(stream.sample_rate) / 1000.0f, stream.bit_depth, (stream.channels == 1) ? "Mono" : "Stereo");
        m_lcd_display->printLine(3, buf, Hardware::COLOR_CYAN);

        snprintf(buf, sizeof(buf), "CODEC: %s (%lu kbps)", stream.codec_name.c_str(), stream.bitrate_kbps);
        m_lcd_display->printLine(4, buf, Hardware::COLOR_CYAN);

        snprintf(buf, sizeof(buf), "BIS: #%u | PKTS: %lu", stream.bis_index, stream.packets_count);
        m_lcd_display->printLine(5, buf, Hardware::COLOR_MAGENTA);

        if (cfg->node_role == NODE_ROLE_SOURCE && m_tone_gen) {
            snprintf(buf, sizeof(buf), "VCO: %.1f Hz (VFO: %.2fHz)", 
            m_tone_gen->getCurrentFrequency(), m_tone_gen->getModulationRate());
            m_lcd_display->printLine(6, buf, Hardware::COLOR_ORANGE);
        } else {
            snprintf(buf, sizeof(buf), "DAC: MAX98357A I2S Mono");
            m_lcd_display->printLine(6, buf, Hardware::COLOR_WHITE);
        }

        m_lcd_display->flush();
    }

    // 5. Update Unified WS2812B Status LED Controller across both Node20 and Node21
    if (strcmp(bt_state, "BROADCASTING") == 0) {
        Hardware::getStatusLed().setSystemState(Hardware::SystemState::BROADCASTING); // BLINK_FAST, COLOR_BLUE @ 32
    } else if (strcmp(bt_state, "STREAMING") == 0) {
        Hardware::getStatusLed().setSystemState(Hardware::SystemState::STREAMING);    // BLINK_FAST, COLOR_TEAL @ 32
    } else if (strcmp(bt_state, "SYNCED (PBP)") == 0 || strcmp(bt_state, "SYNCED") == 0) {
        Hardware::getStatusLed().setSystemState(Hardware::SystemState::BT_SYNC);      // BLINK_SLOW, COLOR_TEAL @ 32
    } else {
        Hardware::getStatusLed().setSystemState(Hardware::SystemState::IDLE);         // BLINK_SLOW, COLOR_GREEN @ 32
    }
}

} // namespace Diagnostics
