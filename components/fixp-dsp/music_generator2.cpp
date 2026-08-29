#include <Arduino.h>
#include <math.h>

// 5 Pentatonic Pitch Classes (F# / Black Keys in Hz)
const float PITCH_SET[5] = { 185.00f, 207.65f, 233.08f, 277.18f, 311.13f }; // F#3, G#3, A#3, C#4, D#4

// Rhythmic Durations (relative to a quarter note = 1.0)
// 0: Eighth (0.5), 1: Quarter (1.0), 2: Half (2.0), 3: Dotted Half (3.0)
const float RHYTHM_SET[4] = { 0.5f, 1.0f, 2.0f, 3.0f };

class MarkovMusicGenerator {
private:
    float sample_rate;
    float bpm;
    float quarter_duration_samples;
    
    // Synth State
    float main_phase = 0.0f;
    float lfo_phase = 0.0f;
    float lfo_freq = 4.0f;
    float current_freq = 220.0f;
    float current_velocity = 0.8f;
    float envelope_val = 0.0f;
    float decay_multiplier = 0.9999f;
    int samples_remaining = 0;
    int total_duration = 0;
    int attack_samples = 441;
    bool is_rest = false;

    // Markov States
    uint8_t current_pitch_idx = 0;
    uint8_t current_rhythm_idx = 1;
    
    // Configurable Parameters
    float gate_ratio = 0.85f;      // Legato vs Staccato
    float rest_probability = 0.1f; // Phrase breathing
    
    // Cumulative Probability Transition Matrices (Values 0 - 1000)
    // Pitch Transition Matrix (5x5)
    const uint16_t pitch_matrix[5][5] = {
        { 200,  500,  750,  900, 1000 }, // From F#
        { 300,  450,  700,  850, 1000 }, // From G#
        { 200,  400,  550,  850, 1000 }, // From A#
        { 150,  300,  600,  800, 1000 }, // From C#
        { 250,  450,  700,  850, 1000 }  // From D#
    };

    // Rhythm Transition Matrix (4x4)
    const uint16_t rhythm_matrix[4][4] = {
        { 300,  700,  950, 1000 }, // From Eighth
        { 400,  750,  950, 1000 }, // From Quarter
        { 200,  600,  850, 1000 }, // From Half
        { 100,  600,  900, 1000 }  // From Dotted Half
    };

    inline float parabolic_sine(float theta) {
        return (4.0f / PI) * theta - (4.0f / (PI * PI)) * theta * fabsf(theta);
    }

    uint8_t step_markov(const uint16_t* row, size_t size) {
        uint16_t roll = (uint16_t)random(0, 1000);
        for (size_t i = 0; i < size; i++) {
            if (roll < row[i]) return (uint8_t)i;
        }
        return 0;
    }

public:
    MarkovMusicGenerator(float sampleRate = 44100.0f, float initialBpm = 100.0f) {
        sample_rate = sampleRate;
        setBPM(initialBpm);
    }

    void setBPM(float newBpm) {
        bpm = newBpm;
        quarter_duration_samples = (60.0f / bpm) * sample_rate;
    }

    void setGateRatio(float gate) { gate_ratio = constrain(gate, 0.1f, 1.0f); }
    void setRestProbability(float prob) { rest_probability = constrain(prob, 0.0f, 0.5f); }

    void processBlock(float* out_buffer, size_t block_size) {
        for (size_t i = 0; i < block_size; i++) {
            
            // Advance Markov Chain on Note Boundary
            if (samples_remaining <= 0) {
                // Next States
                current_pitch_idx = step_markov(pitch_matrix[current_pitch_idx], 5);
                current_rhythm_idx = step_markov(rhythm_matrix[current_rhythm_idx], 4);
                
                // Frequency + Register Variation (Random Octave Shift)
                float base_freq = PITCH_SET[current_pitch_idx];
                int octave_shift = (random(0, 10) > 7) ? 2 : 1; 
                current_freq = base_freq * (float)octave_shift;

                // Duration Calculation
                float beat_factor = RHYTHM_SET[current_rhythm_idx];
                total_duration = (int)(quarter_duration_samples * beat_factor);
                samples_remaining = total_duration;

                // Phrasing & Dynamic Trigger
                is_rest = (((float)random(0, 1000) / 1000.0f) < rest_probability);
                current_velocity = (float)random(500, 1000) / 1000.0f;

                int active_samples = (int)(total_duration * gate_ratio);
                int decay_samples = max(1, active_samples - attack_samples);
                decay_multiplier = powf(0.001f, 1.0f / (float)decay_samples);
                envelope_val = 0.0f;
            }

            int elapsed = total_duration - samples_remaining;
            int active_duration = (int)(total_duration * gate_ratio);

            // Execute Piano Envelope
            if (is_rest || elapsed >= active_duration) {
                envelope_val = 0.0f;
            } else if (elapsed < attack_samples) {
                envelope_val = (float)elapsed / (float)attack_samples;
            } else {
                envelope_val *= decay_multiplier;
            }
            samples_remaining--;

            // Main Parabolic Sine Oscillator
            main_phase += (2.0f * PI * current_freq) / sample_rate;
            if (main_phase > PI) main_phase -= 2.0f * PI;
            float tone = parabolic_sine(main_phase);

            // LFO Amplitude Breathing
            lfo_phase += (2.0f * PI * lfo_freq) / sample_rate;
            if (lfo_phase > PI) lfo_phase -= 2.0f * PI;
            float am_mod = 1.0f + 0.15f * parabolic_sine(lfo_phase);

            out_buffer[i] = tone * envelope_val * current_velocity * am_mod;
        }
    }
};