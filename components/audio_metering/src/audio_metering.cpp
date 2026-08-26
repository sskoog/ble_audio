#include "audio_metering.hpp"
#include <algorithm>

namespace AudioMetering {

int16_t AudioSignalMeter::getAudioFramePeak_int16(unsigned int frameCount) const {
    if (frameCount == 0) frameCount = 1;
    if (frameCount > DEFAULT_HISTORY_FRAMES) frameCount = DEFAULT_HISTORY_FRAMES;

    int16_t items[DEFAULT_HISTORY_FRAMES];
    size_t count = m_peak_ring_buf.getRecent(items, frameCount);
    if (count == 0) return 0;

    int16_t max_peak = 0;
    for (size_t i = 0; i < count; ++i) {
        if (items[i] > max_peak) {
            max_peak = items[i];
        }
    }
    return max_peak;
}

float AudioSignalMeter::getAudioFramePeak_dBFS(unsigned int frameCount) const {
    int16_t peak = getAudioFramePeak_int16(frameCount);
    if (peak <= 0) return -INFINITY;

    float dbfs = 20.0f * log10f(static_cast<float>(peak) / 32767.0f);
    if (dbfs < -95.0f) return -INFINITY;
    if (dbfs > 0.0f) dbfs = 0.0f;
    return dbfs;
}

float AudioSignalMeter::getAudioFramePeak_pct(unsigned int frameCount) const {
    float dbfs = getAudioFramePeak_dBFS(frameCount);
    if (std::isinf(dbfs) || dbfs <= -95.0f) return 0.0f;
    if (dbfs >= 0.0f) return 100.0f;
    return (dbfs + 95.0f) / 95.0f * 100.0f;
}

int16_t AudioSignalMeter::getAudioFrameRMS_int16(unsigned int frameCount) const {
    if (frameCount == 0) frameCount = 1;
    if (frameCount > DEFAULT_HISTORY_FRAMES) frameCount = DEFAULT_HISTORY_FRAMES;

    int16_t items[DEFAULT_HISTORY_FRAMES];
    size_t count = m_rms_ring_buf.getRecent(items, frameCount);
    if (count == 0) return 0;

    int64_t sum_sq = 0;
    for (size_t i = 0; i < count; ++i) {
        sum_sq += static_cast<int32_t>(items[i]) * static_cast<int32_t>(items[i]);
    }
    uint32_t mean_sq = static_cast<uint32_t>(sum_sq / count);
    return static_cast<int16_t>(isqrt32(mean_sq));
}

float AudioSignalMeter::getAudioFrameRMS_dBFS(unsigned int frameCount) const {
    int16_t rms = getAudioFrameRMS_int16(frameCount);
    if (rms <= 0) return -INFINITY;

    float dbfs = 20.0f * log10f(static_cast<float>(rms) / 32767.0f);
    if (dbfs < -95.0f) return -INFINITY;
    if (dbfs > 0.0f) dbfs = 0.0f;
    return dbfs;
}

float AudioSignalMeter::getAudioFrameRMS_pct(unsigned int frameCount) const {
    float dbfs = getAudioFrameRMS_dBFS(frameCount);
    if (std::isinf(dbfs) || dbfs <= -95.0f) return 0.0f;
    if (dbfs >= 0.0f) return 100.0f;
    return (dbfs + 95.0f) / 95.0f * 100.0f;
}

} // namespace AudioMetering
