#include "lc3_benchmark_runner.hpp"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_partition.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>

// Espressif fixed-point LC3 codec
#include "esp_lc3_enc.h"

// Google floating-point LC3 codec with hardware FPU
#include "lc3.h"

static const char* TAG = "LC3_BENCH";

#define BENCH_MAGIC 0x4C433342

struct ClipHeaderEntry {
    uint32_t sample_rate;
    uint32_t offset_bytes;
    uint32_t size_bytes;
    uint32_t sample_count;
};

struct BinaryStorageHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t clip_count;
    uint32_t header_size;
    ClipHeaderEntry entries[16];
};

Lc3BenchmarkRunner::Lc3BenchmarkRunner() {}

Lc3BenchmarkRunner::~Lc3BenchmarkRunner() {
    if (m_mmap_handle) {
        esp_partition_munmap((esp_partition_mmap_handle_t)(uintptr_t)m_mmap_handle);
        m_mmap_handle = nullptr;
    }
}

bool Lc3BenchmarkRunner::init() {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    
    if (!part) {
        ESP_LOGE(TAG, "Storage partition 'storage' not found in partition table!");
        return false;
    }

    esp_partition_mmap_handle_t mmap_h;
    const void* mapped_ptr = nullptr;
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &mapped_ptr, &mmap_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to memory-map 'storage' partition: %s", esp_err_to_name(err));
        return false;
    }

    m_mmap_handle = (const void*)(uintptr_t)mmap_h;
    m_flash_base = (const uint8_t*)mapped_ptr;

    const BinaryStorageHeader* hdr = (const BinaryStorageHeader*)m_flash_base;
    if (hdr->magic != BENCH_MAGIC) {
        ESP_LOGE(TAG, "Invalid storage partition magic: 0x%08lX (Expected 0x%08X)", (unsigned long)hdr->magic, BENCH_MAGIC);
        return false;
    }

    ESP_LOGI(TAG, "Storage partition mapped at %p, valid header found: %lu clips available",
             m_flash_base, (unsigned long)hdr->clip_count);
    return true;
}

bool Lc3BenchmarkRunner::readPcmClip(uint32_t sample_rate, const int16_t*& out_samples, uint32_t& out_count) {
    if (!m_flash_base) return false;
    const BinaryStorageHeader* hdr = (const BinaryStorageHeader*)m_flash_base;
    for (uint32_t i = 0; i < hdr->clip_count; ++i) {
        if (hdr->entries[i].sample_rate == sample_rate) {
            out_samples = (const int16_t*)(m_flash_base + hdr->entries[i].offset_bytes);
            out_count = hdr->entries[i].sample_count;
            return true;
        }
    }
    return false;
}

bool Lc3BenchmarkRunner::runSingleBenchmark(CodecType codec, uint32_t sample_rate, float frame_duration_ms, uint32_t octets, BenchmarkResult& out_res) {
    const int16_t* pcm_data = nullptr;
    uint32_t total_samples = 0;
    if (!readPcmClip(sample_rate, pcm_data, total_samples)) {
        ESP_LOGE(TAG, "No PCM clip found for sample rate %lu Hz", (unsigned long)sample_rate);
        return false;
    }

    uint32_t frame_samples = 0;
    void* esp_enc_handle = nullptr;
    lc3_encoder_t google_encoder = nullptr;
    void* google_enc_mem = nullptr;

    int dt_us = (int)(frame_duration_ms * 1000.0f + 0.5f);
    uint32_t bitrate_bps = (uint32_t)((float)(octets * 8) * (1000.0f / frame_duration_ms));

    if (codec == CodecType::FIXED_POINT_ESP) {
        esp_lc3_enc_config_t cfg = {};
        cfg.sample_rate = sample_rate;
        cfg.bits_per_sample = 16;
        cfg.channel = 1;
        cfg.frame_dms = (uint8_t)(frame_duration_ms * 10.0f + 0.5f);
        cfg.nbyte = (uint16_t)octets;
        cfg.len_prefixed = false;

        esp_audio_err_t err = esp_lc3_enc_open(&cfg, sizeof(cfg), &esp_enc_handle);
        if (err != ESP_AUDIO_ERR_OK || !esp_enc_handle) {
            ESP_LOGE(TAG, "Failed to open esp_lc3 fixed-point encoder for %lu Hz / %.1f ms / %lu B (err=%d)",
                     (unsigned long)sample_rate, frame_duration_ms, (unsigned long)octets, err);
            return false;
        }
        int in_size = 0, out_size = 0;
        esp_lc3_enc_get_frame_size(esp_enc_handle, &in_size, &out_size);
        frame_samples = (uint32_t)(in_size / sizeof(int16_t));
    } else {
        // Floating point Google liblc3 (Hardware FPU)
        int enc_sr = sample_rate;
        int pcm_sr = 0;
        if (sample_rate == 44100) {
            enc_sr = 48000;
            pcm_sr = 44100;
        }

        unsigned mem_size = lc3_encoder_size(dt_us, enc_sr);
        if (mem_size == 0) {
            ESP_LOGE(TAG, "Invalid liblc3 parameters: dt_us=%d, sr=%d", dt_us, enc_sr);
            return false;
        }
        google_enc_mem = malloc(mem_size);
        if (!google_enc_mem) {
            ESP_LOGE(TAG, "Failed to allocate %u bytes for liblc3 encoder", mem_size);
            return false;
        }
        google_encoder = lc3_setup_encoder(dt_us, enc_sr, pcm_sr, google_enc_mem);
        if (!google_encoder) {
            ESP_LOGE(TAG, "Failed to setup liblc3 encoder");
            free(google_enc_mem);
            return false;
        }
        frame_samples = lc3_frame_samples(dt_us, sample_rate);
    }

    if (frame_samples == 0) {
        if (esp_enc_handle) esp_lc3_enc_close(esp_enc_handle);
        if (google_enc_mem) free(google_enc_mem);
        return false;
    }

    uint32_t total_frames = total_samples / frame_samples;
    std::vector<float> durations_ms;
    durations_ms.reserve(total_frames);

    uint8_t out_frame_buf[512] = {0};

    // Warm-up iteration
    for (int w = 0; w < 5 && w < (int)total_frames; ++w) {
        const int16_t* in_pcm = pcm_data + (w * frame_samples);
        if (codec == CodecType::FIXED_POINT_ESP) {
            esp_audio_enc_in_frame_t in_f = {};
            in_f.buffer = (uint8_t*)in_pcm;
            in_f.len = frame_samples * sizeof(int16_t);
            esp_audio_enc_out_frame_t out_f = {};
            out_f.buffer = out_frame_buf;
            out_f.len = sizeof(out_frame_buf);
            esp_lc3_enc_process(esp_enc_handle, &in_f, &out_f);
        } else {
            lc3_encode(google_encoder, LC3_PCM_FORMAT_S16, in_pcm, 1, octets, out_frame_buf);
        }
    }

    // Benchmark Run
    for (uint32_t f = 0; f < total_frames; ++f) {
        const int16_t* in_pcm = pcm_data + (f * frame_samples);

        int64_t t0 = esp_timer_get_time();
        if (codec == CodecType::FIXED_POINT_ESP) {
            esp_audio_enc_in_frame_t in_f = {};
            in_f.buffer = (uint8_t*)in_pcm;
            in_f.len = frame_samples * sizeof(int16_t);
            esp_audio_enc_out_frame_t out_f = {};
            out_f.buffer = out_frame_buf;
            out_f.len = sizeof(out_frame_buf);
            esp_lc3_enc_process(esp_enc_handle, &in_f, &out_f);
        } else {
            lc3_encode(google_encoder, LC3_PCM_FORMAT_S16, in_pcm, 1, octets, out_frame_buf);
        }
        int64_t t1 = esp_timer_get_time();

        float dur = (float)(t1 - t0) / 1000.0f;
        durations_ms.push_back(dur);
    }

    if (esp_enc_handle) {
        esp_lc3_enc_close(esp_enc_handle);
    }
    if (google_enc_mem) {
        free(google_enc_mem);
    }

    if (durations_ms.empty()) return false;

    float min_val = *std::min_element(durations_ms.begin(), durations_ms.end());
    float max_val = *std::max_element(durations_ms.begin(), durations_ms.end());
    double sum = 0;
    for (float d : durations_ms) sum += d;
    float avg_val = (float)(sum / durations_ms.size());

    std::vector<float> sorted_durs = durations_ms;
    std::sort(sorted_durs.begin(), sorted_durs.end());
    float median_val = sorted_durs[sorted_durs.size() / 2];
    size_t p95_idx = (size_t)(sorted_durs.size() * 0.95f);
    if (p95_idx >= sorted_durs.size()) p95_idx = sorted_durs.size() - 1;
    float p95_val = sorted_durs[p95_idx];

    out_res.codec = codec;
    out_res.sample_rate = sample_rate;
    out_res.frame_duration_ms = frame_duration_ms;
    out_res.frame_octets = octets;
    out_res.bitrate_bps = bitrate_bps;
    out_res.total_frames = (uint32_t)durations_ms.size();
    out_res.min_ms = min_val;
    out_res.avg_ms = avg_val;
    out_res.median_ms = median_val;
    out_res.p95_ms = p95_val;
    out_res.max_ms = max_val;
    out_res.cpu_load_pct = (avg_val / frame_duration_ms) * 100.0f;
    out_res.realtime_factor = frame_duration_ms / avg_val;

    return true;
}

void Lc3BenchmarkRunner::runFullSuite() {
    m_results.clear();

    // Disable Wi-Fi during benchmark for pure CPU measurement
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "===============================================================");
    ESP_LOGI(TAG, "STARTING ESP32-WROOM-32 HARDWARE LC3 BENCHMARK SUITE (240 MHz)");
    ESP_LOGI(TAG, "===============================================================");

    struct TestConfig {
        uint32_t sample_rate;
        float frame_duration_ms;
        uint32_t octets;
    };

    std::vector<TestConfig> tests = {
        // Standard Project Bitrates (10.0 ms & 7.5 ms)
        {48000, 10.0f, 80},
        {48000, 7.5f,  60},
        {44100, 10.0f, 80},
        {44100, 7.5f,  60},
        {32000, 10.0f, 60},
        {32000, 7.5f,  45},
        {24000, 10.0f, 45},
        {24000, 7.5f,  35},
        {16000, 10.0f, 30},
        {16000, 7.5f,  23},
        {8000,  10.0f, 20},
        {8000,  7.5f,  20},

        // High Quality: 100 Octets / frame
        {48000, 10.0f, 100},
        {48000, 7.5f,  100},
        {44100, 10.0f, 100},
        {44100, 7.5f,  100},
        {32000, 10.0f, 100},
        {32000, 7.5f,  100},
        {24000, 10.0f, 100},
        {24000, 7.5f,  100},

        // Ultra High Quality: 120 Octets / frame
        {48000, 10.0f, 120},
        {48000, 7.5f,  120},
        {44100, 10.0f, 120},
        {44100, 7.5f,  120},
        {32000, 10.0f, 120},
        {32000, 7.5f,  120},
        {24000, 10.0f, 120},
        {24000, 7.5f,  120},

        // === 3 DEFINED PROJECT REFERENCE ENCODING LEVELS ===
        // HIGH:   48.0 kHz | 7.5 ms | 120 B | 128 kbps
        {48000, 7.5f,  120},
        // MEDIUM: 32.0 kHz | 10.0 ms | 80 B | 64 kbps
        {32000, 10.0f, 80},
        // LOW:    16.0 kHz | 10.0 ms | 40 B | 32 kbps
        {16000, 10.0f, 40},
    };

    // 1. Series A: Fixed-Point Codec (Espressif)
    ESP_LOGI(TAG, ">>> RUNNING SERIES A: FIXED-POINT LC3 (Espressif)");
    for (const auto& t : tests) {
        BenchmarkResult res = {};
        if (runSingleBenchmark(CodecType::FIXED_POINT_ESP, t.sample_rate, t.frame_duration_ms, t.octets, res)) {
            m_results.push_back(res);
            ESP_LOGI(TAG, "  [FIXP] %5lu Hz | %4.1f ms | %3lu B (%3lu kbps) -> Avg: %5.2f ms | P95: %5.2f ms | CPU: %5.1f%%",
                     (unsigned long)res.sample_rate, res.frame_duration_ms, (unsigned long)res.frame_octets,
                     (unsigned long)(res.bitrate_bps / 1000), res.avg_ms, res.p95_ms, res.cpu_load_pct);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 2. Series B: Floating-Point Codec (Google liblc3 with Hardware FPU)
    ESP_LOGI(TAG, ">>> RUNNING SERIES B: FLOATING-POINT LC3 (Google liblc3 + Hardware FPU)");
    for (const auto& t : tests) {
        BenchmarkResult res = {};
        if (runSingleBenchmark(CodecType::FLOATING_POINT_FPU, t.sample_rate, t.frame_duration_ms, t.octets, res)) {
            m_results.push_back(res);
            ESP_LOGI(TAG, "  [FLT_FPU] %5lu Hz | %4.1f ms | %3lu B (%3lu kbps) -> Avg: %5.2f ms | P95: %5.2f ms | CPU: %5.1f%%",
                     (unsigned long)res.sample_rate, res.frame_duration_ms, (unsigned long)res.frame_octets,
                     (unsigned long)(res.bitrate_bps / 1000), res.avg_ms, res.p95_ms, res.cpu_load_pct);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    printResultsTable();
}

void Lc3BenchmarkRunner::printResultsTable() {
    printf("\n\n+======================================================================================================================================+\n");
    printf("|                                  ESP32-WROOM-32 HARDWARE LC3 ENCODER BENCHMARK RESULTS (240 MHz, SINGLE CORE)                        |\n");
    printf("+======================================================================================================================================+\n");
    printf("| Codec Engine  | Sample Rate | Frame Dur | Octets | Bitrate | Frames | Min (ms) | Avg (ms) | Med (ms) | P95 (ms) | Max (ms) | CPU %% | Realtime |\n");
    printf("+---------------+-------------+-----------+--------+---------+--------+----------+----------+----------+----------+----------+-------+----------+\n");

    for (const auto& r : m_results) {
        const char* codec_str = (r.codec == CodecType::FIXED_POINT_ESP) ? "FixedP (Esp)" : "Float (FPU)";
        printf("| %-13s | %5.1f kHz  | %4.1f ms  | %4lu B | %3lu kbps| %6lu | %8.2f | %8.2f | %8.2f | %8.2f | %8.2f | %4.1f%% |  %5.2fx   |\n",
               codec_str,
               r.sample_rate / 1000.0f,
               r.frame_duration_ms,
               (unsigned long)r.frame_octets,
               (unsigned long)(r.bitrate_bps / 1000),
               (unsigned long)r.total_frames,
               r.min_ms,
               r.avg_ms,
               r.median_ms,
               r.p95_ms,
               r.max_ms,
               r.cpu_load_pct,
               r.realtime_factor);
    }
    printf("+======================================================================================================================================+\n\n");
}
