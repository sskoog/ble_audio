#include "tone_generator.hpp"
#include "esp_log.h"
#include "esp_random.h"
#include <cmath>

static const char* TAG = "GEN_MUSIC";

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

namespace Audio {

static int16_t s_sin_lut[512];
static bool s_sin_lut_initialized = false;

ToneGenerator::ToneGenerator(uint32_t sample_rate_hz)
    : m_sample_rate(sample_rate_hz) {
    init(sample_rate_hz, 275.0f, 110.0f, 440.0f, 30.0f);
}

ToneGenerator::~ToneGenerator() {}

void ToneGenerator::set_gain_dB(float gain_db) {
    if (gain_db > 0.0f) gain_db = 0.0f;
    if (gain_db <= -60.0f) {
        if (gain_db <= -90.0f) {
            m_gain_db = -100.0f;
            m_gain_pct = 0.0f;
            m_peak_amplitude = 0;
            return;
        }
        m_gain_db = gain_db;
        m_gain_pct = 1.0f;
    } else {
        m_gain_db = gain_db;
        m_gain_pct = 1.0f + (gain_db - (-60.0f)) * (99.0f / 60.0f);
    }
    float linear_scale = powf(10.0f, m_gain_db / 20.0f);
    m_peak_amplitude = static_cast<int16_t>(32767.0f * linear_scale + 0.5f);
}

void ToneGenerator::setNominalFrequency(float nominal_freq_hz) {
    m_current_freq_hz = nominal_freq_hz;
    m_base_phase_inc = static_cast<uint32_t>((m_current_freq_hz * 4294967296.0) / static_cast<double>(m_sample_rate));
}

void ToneGenerator::set_gain_pct(float pct) {
    if (pct < 0.1f) {
        m_gain_pct = 0.0f;
        m_gain_db = -100.0f;
        m_peak_amplitude = 0;
        return;
    }
    if (pct > 100.0f) pct = 100.0f;
    m_gain_pct = pct;
    float clamped_pct = (pct < 1.0f) ? 1.0f : pct;
    m_gain_db = -60.0f + (clamped_pct - 1.0f) * (60.0f / 99.0f);
    float linear_scale = powf(10.0f, m_gain_db / 20.0f);
    m_peak_amplitude = static_cast<int16_t>(32767.0f * linear_scale + 0.5f);
}

esp_err_t ToneGenerator::init(uint32_t sample_rate_hz, float nominal_freq_hz, float min_freq_hz, float max_freq_hz, float amplitude_pct) {
    m_sample_rate = sample_rate_hz;
    m_main_phase_acc = 0;
    m_lfo_phase_acc = 0;
    m_samples_remaining = 0;
    m_env_q15 = 0;
    m_in_attack = true;

    // LFO phase increment: (0.17 Hz / Fs) * 2^32
    m_lfo_phase_inc = static_cast<uint32_t>((0.17 * 4294967296.0) / static_cast<double>(m_sample_rate));

    if (!s_sin_lut_initialized) {
        for (int i = 0; i < 512; i++) {
            s_sin_lut[i] = static_cast<int16_t>(sinf((2.0f * static_cast<float>(M_PI) * i) / 512.0f) * 32767.0f);
        }
        s_sin_lut_initialized = true;
    }

    setNominalFrequency(440.0f);
    set_gain_pct(amplitude_pct);

    ESP_LOGI(TAG, "Ultra-Fast Generative Music Synth Initialized: Fs=%lu Hz, 0.17Hz LFO, Q15 DSP (<8us/frame), Gain=%.2f dB",
             (unsigned long)m_sample_rate, m_gain_db);
    return ESP_OK;
}

/* =====================================================================
 *            ULTRA-FAST GENERATIVE MUSIC SYNTHESIZER (Q15 FIXED POINT)
 * ===================================================================== */
size_t ToneGenerator::generateFrame(int16_t* out_pcm, size_t num_samples) {
    if (!out_pcm || num_samples == 0) return 0;

    const int32_t gain_scale = m_peak_amplitude;

    for (size_t i = 0; i < num_samples; i++) {
        // --- NOTE TRIGGER & ENVELOPE COEFFICIENT SETUP (RUNS ONCE PER NOTE) ---
        if (m_samples_remaining <= 0) {
            uint32_t rand_note = esp_random() % 20;
            m_current_freq_hz = BLACK_KEYS[rand_note];

            // Calculate base phase increment for current note
            m_base_phase_inc = static_cast<uint32_t>((m_current_freq_hz * 4294967296.0) / static_cast<double>(m_sample_rate));

            uint32_t note_type = esp_random() % 3;
            float duration_sec = (note_type == 0) ? 0.6f : ((note_type == 1) ? 1.2f : 2.4f);

            int total_duration = static_cast<int>(static_cast<float>(m_sample_rate) * duration_sec);
            m_samples_remaining = total_duration;

            // 15ms rapid linear attack (480 samples @ 32 kHz)
            int attack_samples = static_cast<int>(static_cast<float>(m_sample_rate) * 0.015f);
            m_attack_step_q15 = (32767 + attack_samples - 1) / attack_samples;

            int decay_samples = total_duration - attack_samples;
            if (decay_samples < 1) decay_samples = 1;

            // Exponential decay to -60 dB (0.001) over decay_samples:
            // mult = exp(-6.907755 / decay_samples) * 32768
            float mult_f = expf(-6.907755f / static_cast<float>(decay_samples));
            m_decay_mult_q15 = static_cast<int32_t>(mult_f * 32768.0f + 0.5f);
            if (m_decay_mult_q15 > 32767) m_decay_mult_q15 = 32767;

            m_env_q15 = 0;
            m_in_attack = true;
        }

        // --- ENVELOPE UPDATE (NO DIVISION, SINGLE-CYCLE INTEGER OPS) ---
        if (m_in_attack) {
            m_env_q15 += m_attack_step_q15;
            if (m_env_q15 >= 32767) {
                m_env_q15 = 32767;
                m_in_attack = false;
            }
        } else {
            m_env_q15 = (m_env_q15 * m_decay_mult_q15) >> 15;
        }
        m_samples_remaining--;

        // --- ADVANCE LFO (0.17 Hz) ---
        m_lfo_phase_acc += m_lfo_phase_inc;
        uint32_t lfo_lut_idx = (m_lfo_phase_acc >> 23) & 511; // 512-point LUT index
        int32_t lfo_val_q15 = s_sin_lut[lfo_lut_idx]; // Range [-32767, 32767]

        // --- EFFECT 1: VIBRATO (FM via LFO, +/- 1.5% Depth) ---
        // fm_delta = base_phase_inc * 0.015 * (lfo / 32768)
        // 0.015 * 32768 = 491.52 -> (lfo * 492) >> 15
        int32_t fm_mod_q15 = (lfo_val_q15 * 492) >> 15;
        int64_t fm_delta = (static_cast<int64_t>(m_base_phase_inc) * fm_mod_q15) >> 15;
        uint32_t active_phase_inc = static_cast<uint32_t>(static_cast<int64_t>(m_base_phase_inc) + fm_delta);

        // --- ADVANCE MAIN VCO ---
        m_main_phase_acc += active_phase_inc;
        uint32_t vco_lut_idx = (m_main_phase_acc >> 23) & 511;
        int32_t vco_raw = s_sin_lut[vco_lut_idx]; // Range [-32767, 32767]

        // --- EFFECT 2: TREMOLO / AMPLITUDE BREATHING (AM = 1.0 + 0.2 * LFO) ---
        // 0.2 * 32768 = 6554
        int32_t am_mod_q15 = 32768 + ((lfo_val_q15 * 6554) >> 15);

        // --- FINAL COMPOSITE SAMPLE (VCO * ENVELOPE * AM * GAIN) ---
        int32_t sample_shaped = (vco_raw * m_env_q15) >> 15;
        int32_t sample_am = (sample_shaped * am_mod_q15) >> 15;
        int32_t sample_out = (sample_am * gain_scale) >> 15;

        out_pcm[i] = static_cast<int16_t>(sample_out);
    }

    return num_samples;
}

} // namespace Audio
