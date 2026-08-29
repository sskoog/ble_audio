#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

enum class CodecType {
    FIXED_POINT_ESP,     // Espressif Fixed-Point (libesp_audio_codec)
    FLOATING_POINT_FPU   // Google / Zephyr Floating-Point with Hardware FPU (liblc3)
};

struct BenchmarkResult {
    CodecType codec;
    uint32_t sample_rate;
    float frame_duration_ms;
    uint32_t frame_octets;
    uint32_t bitrate_bps;
    uint32_t total_frames;
    float min_ms;
    float avg_ms;
    float median_ms;
    float p95_ms;
    float max_ms;
    float cpu_load_pct;
    float realtime_factor;
};

class Lc3BenchmarkRunner {
public:
    Lc3BenchmarkRunner();
    ~Lc3BenchmarkRunner();

    bool init();
    void runFullSuite();
    void printResultsTable();
    const std::vector<BenchmarkResult>& getResults() const { return m_results; }

private:
    bool runSingleBenchmark(CodecType codec, uint32_t sample_rate, float frame_duration_ms, uint32_t octets, BenchmarkResult& out_res);
    bool readPcmClip(uint32_t sample_rate, const int16_t*& out_samples, uint32_t& out_count);

    const void* m_mmap_handle = nullptr;
    const uint8_t* m_flash_base = nullptr;
    std::vector<BenchmarkResult> m_results;
};
