#include <Arduino.h>
#include <math.h>

class SimplePianoGenerator {
private:
    float sample_rate = 44100.0f;
    float main_phase = 0.0f;
    float lfo_phase = 0.0f;
    float lfo_freq = 4.0f; // 3-5 Hz Amplitude Breathing
    
    int samples_remaining = 0;
    int total_duration = 0;
    float current_freq = 440.0f;

    float envelope_val = 0.0f;
    float decay_multiplier = 0.9999f;
    int attack_samples = 441; // ~10ms fast hammer strike

    // Fast Parabolic Sine Approximation wrapped in [-PI, PI]
    inline float parabolic_sine(float theta) {
        return (4.0f / PI) * theta - (4.0f / (PI * PI)) * theta * fabsf(theta);
    }

public:
    void triggerNote(float freq_hz, float duration_sec) {
        current_freq = freq_hz;
        total_duration = (int)(sample_rate * duration_sec);
        samples_remaining = total_duration;
        
        int decay_samples = total_duration - attack_samples;
        // Exponential decay targeting 0.001 amplitude at the end of the tail
        decay_multiplier = powf(0.001f, 1.0f / (float)decay_samples); 
        envelope_val = 0.0f;
    }

    void processBlock(float* out_buffer, size_t block_size) {
        for (size_t i = 0; i < block_size; i++) {
            if (samples_remaining <= 0) {
                out_buffer[i] = 0.0f;
                continue;
            }

            // 1. Envelope Generation
            int elapsed = total_duration - samples_remaining;
            if (elapsed < attack_samples) {
                envelope_val = (float)elapsed / (float)attack_samples;
            } else {
                envelope_val *= decay_multiplier;
            }
            samples_remaining--;

            // 2. Main Oscillator
            main_phase += (2.0f * PI * current_freq) / sample_rate;
            if (main_phase > PI) main_phase -= 2.0f * PI;
            float tone = parabolic_sine(main_phase);

            // 3. Amplitude Breathing (AM)
            lfo_phase += (2.0f * PI * lfo_freq) / sample_rate;
            if (lfo_phase > PI) lfo_phase -= 2.0f * PI;
            // A(t) = env(t) * (1.0 + 0.2 * sin(2*pi*fam*t))
            float am_mod = 1.0f + 0.2f * parabolic_sine(lfo_phase);

            out_buffer[i] = tone * envelope_val * am_mod;
        }
    }
};