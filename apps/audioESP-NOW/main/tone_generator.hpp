#pragma once

#include <cstdint>
#include <cstddef>
#include "esp_err.h"

namespace Audio {

// Fundamental frequencies for piano black keys from C#2 to Bb5 (Pentatonic Scale)
static const float BLACK_KEYS[20] = {
    73.42f, 77.78f, 92.50f, 103.83f, 116.54f, 
    146.83f, 155.56f, 185.00f, 207.65f, 233.08f, 
    293.66f, 311.13f, 369.99f, 415.30f, 466.16f, 
    587.33f, 622.25f, 739.99f, 830.61f, 932.33f
};

class ToneGenerator {
public:
    ToneGenerator(uint32_t sample_rate_hz = 32000);
    ~ToneGenerator();

    esp_err_t init(uint32_t sample_rate_hz = 32000, 
                   float nominal_freq_hz = 275.0f, 
                   float min_freq_hz = 110.0f, 
                   float max_freq_hz = 440.0f,
                   float amplitude_pct = 30.0f);

    size_t generateFrame(int16_t* out_pcm, size_t num_samples);

    void set_gain_dB(float gain_db);
    void set_gain_pct(float pct);
    void setNominalFrequency(float nominal_freq_hz);
    float get_gain_dB() const { return m_gain_db; }
    float get_gain_pct() const { return m_gain_pct; }
    float getCurrentFrequency() const { return m_current_freq_hz; }

private:
    uint32_t m_sample_rate = 32000;
    
    // Fast 32-bit Phase Accumulators
    uint32_t m_main_phase_acc = 0;
    uint32_t m_lfo_phase_acc = 0;
    uint32_t m_lfo_phase_inc = 22817; // 0.17 Hz @ 32 kHz
    uint32_t m_base_phase_inc = 0;

    int m_samples_remaining = 0;
    float m_current_freq_hz = 440.0f;

    // High-Efficiency Q15 Envelope State (Zero divisions per sample)
    int32_t m_env_q15 = 0;
    int32_t m_attack_step_q15 = 68; // 15ms linear attack
    int32_t m_decay_mult_q15 = 32764; // Exponential decay to -60dB
    bool m_in_attack = true;

    int16_t m_peak_amplitude = 248; // Gentle quiet volume scale
    float m_gain_db = -42.42f;
    float m_gain_pct = 30.0f;
};

} // namespace Audio
