#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <Arduino.h>
#include <AudioOutputI2S.h>
#include "config.h"
#include <driver/i2s.h>
#include "time_sync_lora.h"
#include <vector>
#include <algorithm>


#define DSP_TIME_RINGBUFFER_SIZE 512


// External dependencies defined in main.cpp
extern volatile int currentDspMode;

extern volatile int16_t activeDqsDegrees;
extern volatile int16_t activeDfsFreq_mHz;

//extern volatile bool playScheduled;
extern volatile uint32_t scheduledPlayTime;
extern void unmuteAmplifier();

// Fast Parabolic Sine/Cosine Trigonometry
inline float fastSin(float x) {
    if (x < -PI) {
        float num = ceilf((-PI - x) / (2.0f * PI));
        x += num * 2.0f * PI;
    } else if (x > PI) {
        float num = ceilf((x - PI) / (2.0f * PI));
        x -= num * 2.0f * PI;
    }
    float sin_val = 1.27323954f * x - 0.405284735f * x * fabsf(x);
    sin_val = 0.225f * (sin_val * fabsf(sin_val) - sin_val) + sin_val;
    return sin_val;
}

inline float fastCos(float x) {
    float x_plus_half_pi = x + 1.570796327f;
    if (x_plus_half_pi > PI) {
        x_plus_half_pi -= 2.0f * PI;
    }
    return fastSin(x_plus_half_pi);
}

// Biquad High-Pass Filter Class
class BiquadHPF {
private:
    float a0, a1, a2;
    float b1, b2;
    float x1, x2;
    float y1, y2;
    float cutoff;

public:
    BiquadHPF(float cutoffFreq = 150.0f) : cutoff(cutoffFreq) {
        a0 = 1.0f; a1 = 0.0f; a2 = 0.0f;
        b1 = 0.0f; b2 = 0.0f;
        reset();
    }
    
    void reset() {
        x1 = x2 = y1 = y2 = 0.0f;
    }

    void calculateCoefficients(float sampleRate) {
        if (sampleRate <= 0.0f) return;
        float Fc = cutoff / sampleRate;
        if (Fc >= 0.49f) Fc = 0.49f; // Nyquist limit safety
        float K = tanf(PI * Fc);
        float Q_val = 0.70710678f; // Butterworth
        float norm = 1.0f / (1.0f + K / Q_val + K * K);
        
        a0 = norm;
        a1 = -2.0f * a0;
        a2 = a0;
        b1 = 2.0f * (K * K - 1.0f) * norm;
        b2 = (1.0f - K / Q_val + K * K) * norm;
    }

    inline float process(float x0) {
        float y0 = a0 * x0 + a1 * x1 + a2 * x2 - b1 * y1 - b2 * y2;
        x2 = x1;
        x1 = x0;
        y2 = y1;
        y1 = y0;
        if (fabsf(y1) < 1e-8f) y1 = 0.0f;
        if (fabsf(y2) < 1e-8f) y2 = 0.0f;
        return y0;
    }

    void processBlock(const float* in, float* out, size_t blockSize) {
        for (size_t i = 0; i < blockSize; i++) {
            out[i] = process(in[i]);
        }
    }
};

// Hilbert Transform Cascaded Allpass Class
class HilbertTransform {
private:
    const float a0 = 0.161758f;
    const float a1 = 0.733029f;
    const float a2 = 0.945349f;
    const float b0 = 0.479401f;
    const float b1 = 0.876218f;
    const float b2 = 0.985953f;

    float xA0, yA0, xA1, yA1, xA2, yA2;
    float xB0, yB0, xB1, yB1, xB2, yB2;

    inline float allpass(float x, float pole, float& x_z1, float& y_z1) {
        float y = pole * (x + y_z1) - x_z1;
        x_z1 = x;
        y_z1 = y;
        return y;
    }

public:
    HilbertTransform() { reset(); }

    void reset() {
        xA0 = yA0 = xA1 = yA1 = xA2 = yA2 = 0.0f;
        xB0 = yB0 = xB1 = yB1 = xB2 = yB2 = 0.0f;
    }

    inline void processSample(float in, float& outReal, float& outImag) {
        // Path A
        float outA0 = allpass(in, a0, xA0, yA0);
        float outA1 = allpass(outA0, a1, xA1, yA1);
        outReal = allpass(outA1, a2, xA2, yA2);

        // Path B
        float outB0 = allpass(in, b0, xB0, yB0);
        float outB1 = allpass(outB0, b1, xB1, yB1);
        outImag = allpass(outB1, b2, xB2, yB2);
    }
};

class SpatialAllpassSplitter {
private:
    BiquadHPF hpCenter;
    BiquadHPF hpSide;
    HilbertTransform hilbert;

    float osc_x;
    float osc_y;
    float lfo_phase;
    
    int16_t last_freq_mHz;
    int16_t last_dqs_deg;
    int last_sampleRate;
    
    float cos_omega;
    float sin_omega;
    float cos_theta;
    float sin_theta;
    uint32_t norm_counter;

public:
    SpatialAllpassSplitter() : hpCenter(HPF_CUTOFF_CENTER), hpSide(HPF_CUTOFF_SIDE) {
        osc_x = 1.0f;
        osc_y = 0.0f;
        lfo_phase = 0.0f;
        last_freq_mHz = -1;
        last_dqs_deg = -999;
        last_sampleRate = -1;
        cos_omega = 1.0f;
        sin_omega = 0.0f;
        cos_theta = 1.0f;
        sin_theta = 0.0f;
        norm_counter = 0;
    }

    void reset(int sampleRate) {
        hpCenter.reset();
        hpSide.reset();
        hpCenter.calculateCoefficients(sampleRate);
        hpSide.calculateCoefficients(sampleRate);
        hilbert.reset();
        osc_x = 1.0f;
        osc_y = 0.0f;
        lfo_phase = 0.0f;
        norm_counter = 0;
        last_freq_mHz = -1;
        last_dqs_deg = -999;
        last_sampleRate = -1;
    }

    void processMonoBlock(const float* in, float* out, size_t size, int dspMode, int sampleRate) {
        // Mode 0: Only HP-filter and gain (Bypass mono processing)
        if (dspMode == 0) {
            hpCenter.processBlock(in, out, size);
            return;
        }

        // Apply HPF for safety in all spatial modes
        float temp[64];
        hpCenter.processBlock(in, temp, size);

        int16_t freq_mHz = activeDfsFreq_mHz;
        int16_t dqs_deg = activeDqsDegrees;

        // Recalculate coefficients if parameter state changed
        if (freq_mHz != last_freq_mHz || sampleRate != last_sampleRate) {
            float omega = (sampleRate > 0) ? ((2.0f * PI * (float)freq_mHz) / (1000.0f * (float)sampleRate)) : 0.0f;
            cos_omega = cosf(omega);
            sin_omega = sinf(omega);
            last_freq_mHz = freq_mHz;
            last_sampleRate = sampleRate;
        }

        if (dqs_deg != last_dqs_deg) {
            float theta = (float)dqs_deg * PI / 180.0f;
            cos_theta = cosf(theta);
            sin_theta = sinf(theta);
            last_dqs_deg = dqs_deg;
        }

        // Algorithm 1: Mono Hilbert dry/wet mix: DQS + DFS
        if (dspMode == 1) {
            // Static DQS Phase Offset for the node (NODE_ID * 72)
            float node_theta = (float)(NODE_ID * 72) * PI / 180.0f;
            float cos_node = cosf(node_theta);
            float sin_node = sinf(node_theta);

            for (size_t i = 0; i < size; i++) {
                float xi, xq;
                hilbert.processSample(temp[i], xi, xq);

                // Update complex oscillator
                float next_x = osc_x * cos_omega - osc_y * sin_omega;
                float next_y = osc_y * cos_omega + osc_x * sin_omega;
                osc_x = next_x;
                osc_y = next_y;

                if (++norm_counter >= 512) {
                    norm_counter = 0;
                    float len = sqrtf(osc_x * osc_x + osc_y * osc_y);
                    if (len > 1e-6f) {
                        osc_x /= len;
                        osc_y /= len;
                    }
                }

                // Dynamic offset combining static base and LoRa calibration
                // cos_theta/sin_theta represent LoRa activeDqsDegrees
                float cos_tot = cos_node * cos_theta - sin_node * sin_theta;
                float sin_tot = sin_node * cos_theta + cos_node * sin_theta;

                // Composite rotation
                float cos_final = osc_x * cos_tot - osc_y * sin_tot;
                float sin_final = osc_y * cos_tot + osc_x * sin_tot;

                float wet = xi * cos_final - xq * sin_final;
                // Mix wet and dry
                out[i] = temp[i] + 0.5f * wet;
            }
        }
        // Algorithm 2: Mono Hilbert DQS + LFO primes (Rotary Leslie)
        else if (dspMode == 2) {
            const float lfo_primes[5] = {0.11f, 0.13f, 0.17f, 0.19f, 0.23f};
            float base_lfo_freq = lfo_primes[NODE_ID];
            // Scale LFO speed by LoRa frequency (default to 1000mHz = 1x speed)
            float lfo_freq = base_lfo_freq * (freq_mHz > 0 ? (float)freq_mHz / 1000.0f : 1.0f);
            float lfo_omega = (sampleRate > 0) ? (2.0f * PI * lfo_freq / (float)sampleRate) : 0.0f;
            
            // Depth beta from activeDqsDegrees (0..360 maps to 0..PI)
            float beta = (dqs_deg / 360.0f) * PI;

            for (size_t i = 0; i < size; i++) {
                float xi, xq;
                hilbert.processSample(temp[i], xi, xq);

                lfo_phase += lfo_omega;
                if (lfo_phase > PI) lfo_phase -= 2.0f * PI;
                if (lfo_phase < -PI) lfo_phase += 2.0f * PI;

                float lfo_val = fastSin(lfo_phase);
                float theta = beta * lfo_val;

                float c_val = fastCos(theta);
                float s_val = fastSin(theta);

                out[i] = xi * c_val - xq * s_val;
            }
        }
        // Algorithm 3: Mono Hilbert DFS stepped
        else if (dspMode == 3) {
            // Node-specific frequency shift: df_k = k * step_size
            // activeDfsFreq_mHz sets the step size (e.g. 400 mHz is 0.4 Hz step size)
            float step_size = (freq_mHz > 0) ? (float)freq_mHz / 1000.0f : 0.4f;
            float df = (float)NODE_ID * step_size;
            
            // Local custom omega for this step
            float local_omega = (sampleRate > 0) ? ((2.0f * PI * df) / (float)sampleRate) : 0.0f;
            float c_omega = cosf(local_omega);
            float s_omega = sinf(local_omega);

            for (size_t i = 0; i < size; i++) {
                float xi, xq;
                hilbert.processSample(temp[i], xi, xq);

                // Update complex oscillator
                float next_x = osc_x * c_omega - osc_y * s_omega;
                float next_y = osc_y * c_omega + osc_x * s_omega;
                osc_x = next_x;
                osc_y = next_y;

                if (++norm_counter >= 512) {
                    norm_counter = 0;
                    float len = sqrtf(osc_x * osc_x + osc_y * osc_y);
                    if (len > 1e-6f) {
                        osc_x /= len;
                        osc_y /= len;
                    }
                }

                out[i] = xi * osc_x - xq * osc_y;
            }
        }
    }

    void processStereoBlock(const float* inL, const float* inR, float* out, size_t size, int dspMode, int sampleRate) {
        // If dspMode != 4, we use processMonoBlock on the left channel as fallback
        if (dspMode != 4) {
            processMonoBlock(inL, out, size, dspMode, sampleRate);
            return;
        }

        int16_t freq_mHz = activeDfsFreq_mHz;
        int16_t dqs_deg = activeDqsDegrees;

        // Recalculate coefficients if parameter state changed
        // For side channel chorusing, default to 0.2 Hz
        float side_dfs = (freq_mHz > 0) ? (float)freq_mHz / 1000.0f : 0.2f;
        if (freq_mHz != last_freq_mHz || sampleRate != last_sampleRate) {
            float omega = (sampleRate > 0) ? ((2.0f * PI * side_dfs) / (float)sampleRate) : 0.0f;
            cos_omega = cosf(omega);
            sin_omega = sinf(omega);
            last_freq_mHz = freq_mHz;
            last_sampleRate = sampleRate;
        }

        if (dqs_deg != last_dqs_deg) {
            float theta = (float)dqs_deg * PI / 180.0f;
            cos_theta = cosf(theta);
            sin_theta = sinf(theta);
            last_dqs_deg = dqs_deg;
        }

        // Static node-unique offset (k * 72 deg)
        float node_theta = (float)(NODE_ID * 72) * PI / 180.0f;
        float cos_node = cosf(node_theta);
        float sin_node = sinf(node_theta);

        // Composite static offset (NODE_ID offset + dynamic activeDqsDegrees)
        float cos_tot = cos_node * cos_theta - sin_node * sin_theta;
        float sin_tot = sin_node * cos_theta + cos_node * sin_theta;

        for (size_t i = 0; i < size; i++) {
            // M/S matrixing
            float m = 0.5f * (inL[i] + inR[i]);
            float s = 0.5f * (inL[i] - inR[i]);

            // High pass filtering
            float m_filt = hpCenter.process(m);
            float s_filt = hpSide.process(s);

            // Hilbert transform Side channel
            float si, sq;
            hilbert.processSample(s_filt, si, sq);

            // Update complex oscillator for Side chorusing
            float next_x = osc_x * cos_omega - osc_y * sin_omega;
            float next_y = osc_y * cos_omega + osc_x * sin_omega;
            osc_x = next_x;
            osc_y = next_y;

            if (++norm_counter >= 512) {
                norm_counter = 0;
                float len = sqrtf(osc_x * osc_x + osc_y * osc_y);
                if (len > 1e-6f) {
                    osc_x /= len;
                    osc_y /= len;
                }
            }

            // Composite rotation
            float cos_final = osc_x * cos_tot - osc_y * sin_tot;
            float sin_final = osc_y * cos_tot + osc_x * sin_tot;

            float s_rotated = si * cos_final - sq * sin_final;

            // Output mono mix of M and rotated S
            out[i] = m_filt + s_rotated;
        }
    }
};


struct DspTimeRingBuffer {
    uint64_t buffer[DSP_TIME_RINGBUFFER_SIZE] = {0};
    int head = 0;
    int count = 0;

    void push(uint64_t val) {
        buffer[head] = val;
        head = (head + 1) % DSP_TIME_RINGBUFFER_SIZE;
        if (count < DSP_TIME_RINGBUFFER_SIZE) {
            count++;
        }
    }

    uint64_t getMax() const {
        if (count == 0) return 0;
        uint64_t m = buffer[0];
        for (int i = 1; i < count; i++) {
            if (buffer[i] > m) {
                m = buffer[i];
            }
        }
        return m;
    }

    uint64_t getMin() const {
        if (count == 0) return 0;
        uint64_t m = buffer[0];
        for (int i = 1; i < count; i++) {
            if (buffer[i] < m) {
                m = buffer[i];
            }
        }
        return m;
    }

    float getAverage() const {
        if (count == 0) return 0.0f;
        double sum = 0;
        for (int i = 0; i < count; i++) {
            sum += (double)buffer[i];
        }
        return (float)(sum / count);
    }

    uint64_t getMedian() const {
        if (count == 0) return 0;
        std::vector<uint64_t> temp(count);
        for (int i = 0; i < count; i++) {
            temp[i] = buffer[i];
        }
        std::sort(temp.begin(), temp.end());
        if (count % 2 == 0) {
            return (temp[count / 2 - 1] + temp[count / 2]) / 2;
        } else {
            return temp[count / 2];
        }
    }

    float getStd() const {
        if (count <= 1) return 0.0f;
        float avg = getAverage();
        double sumSqDiff = 0.0;
        for (int i = 0; i < count; i++) {
            double diff = (double)buffer[i] - avg;
            sumSqDiff += diff * diff;
        }
        return (float)sqrt(sumSqDiff / count);
    }
};

class FilteredAudioOutputI2S : public AudioOutputI2S {
public:
    DspTimeRingBuffer dspTimeBuffer;

    float get_DSP_margin_avg() const {
        if (dspTimeBuffer.count == 0) return 0.0f;
        float sampleRate = currentSampleRate > 0 ? (float)currentSampleRate : 44100.0f;
        float budget = (DSP_BLOCK_SIZE * 1000000.0f) / sampleRate;
        float avgTime = dspTimeBuffer.getAverage();
        return (1.0f - (avgTime / budget)) * 100.0f;
    }

    float get_DSP_margin_std() const {
        if (dspTimeBuffer.count == 0) return 0.0f;
        float sampleRate = currentSampleRate > 0 ? (float)currentSampleRate : 44100.0f;
        float budget = (DSP_BLOCK_SIZE * 1000000.0f) / sampleRate;
        float stdTime = dspTimeBuffer.getStd();
        return (stdTime / budget) * 100.0f;
    }

    static const size_t DSP_BLOCK_SIZE = 64;
    float blockL[DSP_BLOCK_SIZE];
    float blockR[DSP_BLOCK_SIZE];
    float blockOut[DSP_BLOCK_SIZE];
    size_t writeIdx;
    size_t readIdx;

    FilteredAudioOutputI2S() : AudioOutputI2S(0, EXTERNAL_I2S, 16, APLL_DISABLE) {
        SetLsbJustified(false); // Match FMT -> GND (Philips standard I2S)
        currentSampleRate = 44100; // Fallback default
        volumeGain = VOLUME_DEFAULT; // Default gain
        x0_prev_int = 0;
        y1_prev_int = 0;
        currentBitsPerSample = I2S_BIT_DEPTH;
        
        // Initialize Biquad history states
        bq_x1 = bq_x2 = bq_y1 = bq_y2 = 0.0f;
        i_bq_z1 = i_bq_z2 = 0;

        writeIdx = 0;
        readIdx = DSP_BLOCK_SIZE;
        memset(blockL, 0, sizeof(blockL));
        memset(blockR, 0, sizeof(blockR));
        memset(blockOut, 0, sizeof(blockOut));
        
        CalculateFilterCoefficients(HIGH_PASS_FREQ);
    }

    bool WriteSilence_samples(uint32_t numSamples) {
        if (!i2sOn) return false;
        uint8_t bits = currentBitsPerSample;
        for (uint32_t i = 0; i < numSamples; i++) {
            writeI2SSample(0, 0, bits);
        }
        return true;
    }

    bool WriteSilence_ms(uint32_t durationMs) {
        uint32_t frames = (currentSampleRate * durationMs) / 1000;
        return WriteSilence_samples(frames);
    }
    
    void ClearDmaBuffer() {
        if (i2sOn) {
            i2s_zero_dma_buffer((i2s_port_t)portNo);
        }
    }
    
    virtual bool SetRate(int hz) override {
        currentSampleRate = hz;
        
        // Reset our processor
        spatialSplitter.reset(hz);
        
        // Reset block buffer pointers and history
        writeIdx = 0;
        readIdx = DSP_BLOCK_SIZE;
        memset(blockL, 0, sizeof(blockL));
        memset(blockR, 0, sizeof(blockR));
        memset(blockOut, 0, sizeof(blockOut));

        bq_x1 = bq_x2 = bq_y1 = bq_y2 = 0.0f;
        i_bq_z1 = i_bq_z2 = 0;
        
        CalculateFilterCoefficients(HIGH_PASS_FREQ);
        
        // Let the base class set up standard clock dynamically without re-installing the driver
        bool ret = AudioOutputI2S::SetRate(hz);
        return ret;
    }

    void flush() override {
        yield();
    }
    
    void SetVolumeGain(float gain) {
        // Set output digital gain as float
        if (gain > VOLUME_LOG_MAX) {
            gain = VOLUME_LOG_MAX;
        }
        if (gain < VOLUME_LOG_MIN) {
            gain = VOLUME_LOG_MIN;
        }
        volumeGain = gain;
    }
    
    inline int16_t applyGain_int(int16_t sampleVal) {
        // Apply gain on sample(s) using only integer operations (Q15 fixed-point)
        int32_t vol_q15 = (int32_t)(volumeGain * 32768.0f);
        return (int16_t)((sampleVal * vol_q15) >> 15);
    }
    
    inline int16_t applyGain_float(int16_t sampleVal) {
        // Apply gain on sample(s) using floating-point operations (gain value 0.0 - 0.5)
        float val = (float)sampleVal * volumeGain;
        return (int16_t)constrain(roundf(val), -32768, 32767);
    }

    inline void updateWordLength(int bits) {
        // If the bits per sample configuration has changed, re-initialize I2S driver
        int hw_bits = (bits == 16) ? 16 : 32;
        int current_hw_bits = (currentBitsPerSample == 16) ? 16 : 32;
        if (hw_bits != current_hw_bits) {
            reconfigureI2SBits(bits);
        } else {
            currentBitsPerSample = bits; // Just update logically
        }
    }

    inline bool writeI2SSample(int16_t left, int16_t right, int bits) {
        if (!i2sOn) return false;
        size_t written = 0;
        if (bits == 16) {
            // Write standard 16-bit packed stereo sample frame
            uint32_t s32 = (((uint32_t)right & 0xffff) << 16) | ((uint32_t)left & 0xffff);
            i2s_write((i2s_port_t)portNo, &s32, sizeof(uint32_t), &written, portMAX_DELAY);
        } else if (bits == 24) {
            // Write 24-bit resolution inside 32-bit container (lower 8 bits cleared)
            int32_t frame[2];
            frame[0] = (((int32_t)left) << 16) & 0xffffff00;
            frame[1] = (((int32_t)right) << 16) & 0xffffff00;
            i2s_write((i2s_port_t)portNo, frame, sizeof(frame), &written, portMAX_DELAY);
        } else {
            // Write 32-bit stereo sample frame (4 bytes per channel -> 8 bytes total)
            int32_t frame[2];
            frame[0] = ((int32_t)left) << 16;
            frame[1] = ((int32_t)right) << 16;
            i2s_write((i2s_port_t)portNo, frame, sizeof(frame), &written, portMAX_DELAY);
        }
        return written > 0;
    }

    inline float processBiquad(float x0) {
        float y0 = bq_a0 * x0 + bq_a1 * bq_x1 + bq_a2 * bq_x2 - bq_b1 * bq_y1 - bq_b2 * bq_y2;
        bq_x2 = bq_x1;
        bq_x1 = x0;
        bq_y2 = bq_y1;
        bq_y1 = y0;
        if ( fabsf(bq_y1) < 1e-8f ) bq_y1 = 0.0f;
        if ( fabsf(bq_y2) < 1e-8f ) bq_y2 = 0.0f;
        return y0;
    }

    inline float processFirstOrderHP(float x0) {
        float y0 = 0.5f * (1.0f + hp1_alpha) * (x0 - x0_prev) + hp1_alpha * y1_prev;
        x0_prev = x0;
        y1_prev = y0;
        return y0;
    }

    inline int16_t processBiquadInt(int16_t x0_val) {
        int64_t x0_scaled = (int64_t)x0_val << 16;
        int64_t y0_scaled = ((x0_scaled * (int64_t)i_bq_a0) >> 16) + i_bq_z1;
        i_bq_z1 = ((x0_scaled * (int64_t)i_bq_a1) >> 16) + i_bq_z2 - (((int64_t)i_bq_b1 * y0_scaled) >> 16);
        i_bq_z2 = ((x0_scaled * (int64_t)i_bq_a2) >> 16) - (((int64_t)i_bq_b2 * y0_scaled) >> 16);
        return (int16_t)constrain(y0_scaled >> 16, -32768, 32767);
    }

    virtual bool ConsumeSample(int16_t sample[2]) override {
        uint64_t startUs = esp_timer_get_time();
        extern volatile bool stopRequestPending;
        extern volatile int currentDspMode;
        if (stopRequestPending) {
            return false;
        }

        // Store sample in block buffer
        blockL[writeIdx] = (float)sample[0];
        blockR[writeIdx] = (float)sample[1];
        writeIdx++;

        bool blockProcessed = false;
        // If block buffer is full, process it
        if (writeIdx >= DSP_BLOCK_SIZE) {
            blockProcessed = true;
            if (currentDspMode == 4) {
                spatialSplitter.processStereoBlock(blockL, blockR, blockOut, DSP_BLOCK_SIZE, currentDspMode, currentSampleRate);
            } else {
                spatialSplitter.processMonoBlock(blockL, blockOut, DSP_BLOCK_SIZE, currentDspMode, currentSampleRate);
            }
            writeIdx = 0;
            readIdx = 0;
        }

        // Read processed sample from output queue
        float processed = 0.0f;
        if (readIdx < DSP_BLOCK_SIZE) {
            processed = blockOut[readIdx];
            readIdx++;
        }

        // Apply gain (all modes use the volume gain)
        int16_t output_mono = applyGain_float(processed);

        bool ret = writeI2SSample(output_mono, output_mono, I2S_BIT_DEPTH);
        if (blockProcessed) {
            uint64_t elapsedUs = esp_timer_get_time() - startUs;
            dspTimeBuffer.push(elapsedUs);
        }
        return ret;
    }
    
private:
    SpatialAllpassSplitter spatialSplitter;
    int currentSampleRate;
    float alpha_200hz = 0.0f;
    float volumeGain = VOLUME_DEFAULT;
    
    // Previous sample variables (history) for floating-point high-pass
    float x0_prev = 0.0f;
    float y1_prev = 0.0f;
    float hp1_alpha = 0.0f;
    
    // Integer-only high-pass history and coefficient (Q14)
    int32_t alpha_200hz_q14 = 0;
    int32_t x0_prev_int = 0;
    int32_t y1_prev_int = 0;

    // Biquad filter coefficients and history states (Float & Int)
    float bq_a0 = 1.0f, bq_a1 = 0.0f, bq_a2 = 0.0f;
    float bq_b1 = 0.0f, bq_b2 = 0.0f;
    float bq_x1 = 0.0f, bq_x2 = 0.0f;
    float bq_y1 = 0.0f, bq_y2 = 0.0f;

    int32_t i_bq_a0 = 65536, i_bq_a1 = 0, i_bq_a2 = 0;
    int32_t i_bq_b1 = 0, i_bq_b2 = 0;
    int64_t i_bq_z1 = 0, i_bq_z2 = 0;
    
    // Track I2S hardware configurations
    int currentBitsPerSample = 16;
    
    void reconfigureI2SBits(int bits) {
        if (!i2sOn) return;
        
        // 1. Uninstall the existing driver
        i2s_driver_uninstall((i2s_port_t)portNo);
        
        // 2. Set new bits per sample in config (only 16 or 32 at the hardware level)
        i2s_bits_per_sample_t i2s_bps = I2S_BITS_PER_SAMPLE_16BIT;
        if (bits == 24 || bits == 32) {
            i2s_bps = I2S_BITS_PER_SAMPLE_32BIT;
        }
        
        i2s_config_t i2s_config = {
            .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
            .sample_rate = (uint32_t)currentSampleRate,
            .bits_per_sample = i2s_bps,
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
            .communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_I2S,
            .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
            .dma_buf_count = 8, 
            .dma_buf_len = 128,
            .use_apll = APLL_DISABLE,
            .tx_desc_auto_clear = true
        };
        
        // 3. Re-install driver
        i2s_driver_install((i2s_port_t)portNo, &i2s_config, 0, NULL);
        
        // 4. Re-configure pins
        i2s_pin_config_t pin_config = {
            .bck_io_num = I2S_BCLK,
            .ws_io_num = I2S_LRCK,
            .data_out_num = I2S_DOUT,
            .data_in_num = I2S_PIN_NO_CHANGE
        };
        i2s_set_pin((i2s_port_t)portNo, &pin_config);
        
        i2s_zero_dma_buffer((i2s_port_t)portNo);
        
        currentBitsPerSample = bits;
        
        Serial.printf("[I2S] Dynamically reconfigured to %d-bit depth at %d Hz\n", bits, currentSampleRate);
    }

    void CalculateFilterCoefficients(float cutoff_hz = HIGH_PASS_FREQ) {
        float dt = 1.0f / currentSampleRate;
        float RC = 1.0f / (2.0f * PI * cutoff_hz);
        alpha_200hz = RC / (RC + dt);
        alpha_200hz_q14 = (int32_t)roundf(alpha_200hz * 16384.0f);

        // 1st-order High-Pass Filter Coefficient
        float K_1st = tanf(PI * cutoff_hz / (float)currentSampleRate);
        hp1_alpha = (1.0f - K_1st) / (1.0f + K_1st);

        // Biquad Butterworth 2nd-order High-Pass Filter Coefficients
        float Fc = cutoff_hz / (float)currentSampleRate;
        if (Fc >= 0.5f) Fc = 0.49f; // Nyquist limit safety
        float K = tanf(PI * Fc);
        float Q_val = 0.70710678f; // Butterworth
        float norm = 1.0f / (1.0f + K / Q_val + K * K);
        
        bq_a0 = norm;
        bq_a1 = -2.0f * bq_a0;
        bq_a2 = bq_a0;
        bq_b1 = 2.0f * (K * K - 1.0f) * norm;
        bq_b2 = (1.0f - K / Q_val + K * K) * norm;

        // Convert to Q16 integer coefficients
        i_bq_a0 = (int32_t)roundf(bq_a0 * 65536.0f);
        i_bq_a1 = (int32_t)roundf(bq_a1 * 65536.0f);
        i_bq_a2 = (int32_t)roundf(bq_a2 * 65536.0f);
        i_bq_b1 = (int32_t)roundf(bq_b1 * 65536.0f);
        i_bq_b2 = (int32_t)roundf(bq_b2 * 65536.0f);
    }
};

#endif // AUDIO_OUTPUT_H
