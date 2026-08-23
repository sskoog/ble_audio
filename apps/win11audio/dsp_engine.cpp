#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <cmath>
#include <vector>
#include <array>
#include <complex>

namespace py = pybind11;

constexpr float PI_F = 3.14159265358979323846f;
constexpr float TWO_PI_F = 6.28318530717958647692f;
constexpr int HILBERT_TAPS = 31;
constexpr int HILBERT_DELAY = (HILBERT_TAPS - 1) / 2; // 15 samples
constexpr int LUT_SIZE = 1024;
constexpr int LUT_MASK = LUT_SIZE - 1;

enum MixingMode {
    MODE_PHASE_SHIFT = 0,
    MODE_PHASE_MODULATOR = 1,
    MODE_FREQUENCY_SHIFT = 2
};

// -----------------------------------------------------------------------------
// 1024-Point Precomputed Sine/Cosine LUT with Linear Interpolation
// -----------------------------------------------------------------------------
class FastTrigLUT {
public:
    FastTrigLUT() {
        for (int i = 0; i < LUT_SIZE; ++i) {
            float angle = (TWO_PI_F * static_cast<float>(i)) / static_cast<float>(LUT_SIZE);
            lut_[i] = std::sin(angle);
        }
    }

    // Fast interpolated sine evaluation without std::sin()
    inline float sin(float rad) const {
        float phase_norm = rad * (static_cast<float>(LUT_SIZE) / TWO_PI_F);
        int idx0 = static_cast<int>(std::floor(phase_norm));
        float frac = phase_norm - static_cast<float>(idx0);
        
        int i0 = idx0 & LUT_MASK;
        int i1 = (i0 + 1) & LUT_MASK;
        return (1.0f - frac) * lut_[i0] + frac * lut_[i1];
    }

    // Cosine obtained via exact quarter-wave phase offset
    inline float cos(float rad) const {
        return sin(rad + (0.5f * PI_F));
    }

    // Simultaneous Sine/Cosine calculation
    inline void get_sin_cos(float rad, float& s, float& c) const {
        s = sin(rad);
        c = cos(rad);
    }

private:
    std::array<float, LUT_SIZE> lut_;
};

// -----------------------------------------------------------------------------
// Recursive Quadrature Oscillator (Coupled Complex Form with Amplitude Control)
// -----------------------------------------------------------------------------
class RecursiveQuadratureOscillator {
public:
    RecursiveQuadratureOscillator() { reset(); }

    void set_frequency(float delta_f_hz, float sample_rate) {
        float w0 = TWO_PI_F * delta_f_hz / sample_rate;
        cos_w_ = std::cos(w0);
        sin_w_ = std::sin(w0);
    }

    inline void process(float& cos_out, float& sin_out) {
        // Complex phasor rotation: [x, y] * e^(j*w0)
        float next_cos = (cos_state_ * cos_w_) - (sin_state_ * sin_w_);
        float next_sin = (cos_state_ * sin_w_) + (sin_state_ * cos_w_);

        // 1st-order Padé/Taylor normalization to eliminate floating-point energy drift
        float mod2 = (next_cos * next_cos) + (next_sin * next_sin);
        float norm = 1.5f - (0.5f * mod2);

        cos_state_ = next_cos * norm;
        sin_state_ = next_sin * norm;

        cos_out = cos_state_;
        sin_out = sin_state_;
    }

    void reset() {
        cos_state_ = 1.0f;
        sin_state_ = 0.0f;
    }

private:
    float cos_w_ = 1.0f;
    float sin_w_ = 0.0f;
    float cos_state_ = 1.0f;
    float sin_state_ = 0.0f;
};

// -----------------------------------------------------------------------------
// Matched Circular Buffer Delay Line (for In-Phase Alignment)
// -----------------------------------------------------------------------------
template <size_t N>
class DelayLine {
public:
    DelayLine() { buffer_.fill(0.0f); }

    inline float process(float in) {
        buffer_[head_] = in;
        size_t read_idx = (head_ + 1) % N;
        head_ = read_idx;
        return buffer_[read_idx];
    }

    void reset() {
        buffer_.fill(0.0f);
        head_ = 0;
    }

private:
    std::array<float, N> buffer_;
    size_t head_ = 0;
};

// -----------------------------------------------------------------------------
// 100 Hz High-Pass Biquad (Direct Form II Transposed)
// -----------------------------------------------------------------------------
class BiquadHPF {
public:
    BiquadHPF() { reset(); }

    void init(float sample_rate, float cutoff_freq = 100.0f, float q = 0.70710678f) {
        float w0 = TWO_PI_F * cutoff_freq / sample_rate;
        float alpha = std::sin(w0) / (2.0f * q);
        float cos_w0 = std::cos(w0);

        float b0 =  (1.0f + cos_w0) * 0.5f;
        float b1 = -(1.0f + cos_w0);
        float b2 =  (1.0f + cos_w0) * 0.5f;
        float a0 =   1.0f + alpha;
        float a1 =  -2.0f * cos_w0;
        float a2 =   1.0f - alpha;

        b0_ = b0 / a0;
        b1_ = b1 / a0;
        b2_ = b2 / a0;
        a1_ = a1 / a0;
        a2_ = a2 / a0;
        reset();
    }

    inline float process(float in) {
        float out = (b0_ * in) + s1_;
        s1_ = (b1_ * in) - (a1_ * out) + s2_;
        s2_ = (b2_ * in) - (a2_ * out);
        return out;
    }

    void reset() {
        s1_ = 0.0f;
        s2_ = 0.0f;
    }

private:
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
    float s1_ = 0.0f, s2_ = 0.0f;
};

// -----------------------------------------------------------------------------
// 31-Tap Type-III FIR Hilbert Transformer (Quadrature Splitter)
// -----------------------------------------------------------------------------
class HilbertTransformer {
public:
    HilbertTransformer() {
        for (int i = 0; i < HILBERT_TAPS; ++i) {
            int n = i - HILBERT_DELAY;
            if (n % 2 != 0) {
                float ideal_h = 2.0f / (PI_F * static_cast<float>(n));
                float w = 0.42f - (0.5f * std::cos(TWO_PI_F * static_cast<float>(i) / static_cast<float>(HILBERT_TAPS - 1)))
                               + (0.08f * std::cos(2.0f * TWO_PI_F * static_cast<float>(i) / static_cast<float>(HILBERT_TAPS - 1)));
                h_[i] = ideal_h * w;
            } else {
                h_[i] = 0.0f;
            }
        }
        reset();
    }

    inline void process(float in, float& i_out, float& q_out) {
        // In-Phase Path: Exact 15-sample alignment delay
        i_out = delay_match_.process(in);

        // Quadrature Path: Anti-symmetric FIR convolution
        state_[head_] = in;
        float acc = 0.0f;
        int idx = head_;
        for (int i = 0; i < HILBERT_TAPS; ++i) {
            acc += h_[i] * state_[idx];
            idx = (idx == 0) ? (HILBERT_TAPS - 1) : (idx - 1);
        }
        head_ = (head_ + 1) % HILBERT_TAPS;
        q_out = acc;
    }

    void reset() {
        state_.fill(0.0f);
        head_ = 0;
        delay_match_.reset();
    }

private:
    std::array<float, HILBERT_TAPS> h_;
    std::array<float, HILBERT_TAPS> state_;
    int head_ = 0;
    DelayLine<HILBERT_DELAY + 1> delay_match_;
};

// -----------------------------------------------------------------------------
// Complete Hybrid C++ Audio DSP Pipeline
// -----------------------------------------------------------------------------
class AudioDSPPipeline {
public:
    AudioDSPPipeline(float sample_rate = 48000.0f) : sample_rate_(sample_rate) {
        hpf_left_.init(sample_rate_, 100.0f);
        hpf_right_.init(sample_rate_, 100.0f);
        osc_nco_.set_frequency(5.0f, sample_rate_);
    }

    void set_mode(int mode) {
        mode_ = static_cast<MixingMode>(mode);
    }

    void set_phase_shift(float phase_rad) {
        static_phase_rad_ = phase_rad;
    }

    void set_phase_modulator(float lfo_freq_hz, float depth_rad) {
        lfo_inc_ = TWO_PI_F * lfo_freq_hz / sample_rate_;
        mod_depth_ = depth_rad;
    }

    void set_dfs_offset(float delta_f_hz) {
        osc_nco_.set_frequency(delta_f_hz, sample_rate_);
    }

    void reset() {
        hpf_left_.reset();
        hpf_right_.reset();
        hilbert_mid_.reset();
        hilbert_side_.reset();
        osc_nco_.reset();
        lfo_phase_ = 0.0f;
    }

    py::dict process_frames(py::array_t<float> input_l, py::array_t<float> input_r) {
        auto buf_l = input_l.request();
        auto buf_r = input_r.request();
        size_t n_samples = buf_l.size;

        const float* ptr_l = static_cast<const float*>(buf_l.ptr);
        const float* ptr_r = static_cast<const float*>(buf_r.ptr);

        // Pre-allocate NumPy buffers (Intermediates, Analytics, and Final Outputs)
        auto raw_l = py::array_t<float>(n_samples);
        auto raw_r = py::array_t<float>(n_samples);
        auto mid_pre = py::array_t<float>(n_samples);
        auto side_pre = py::array_t<float>(n_samples);
        auto mid_analytic = py::array_t<std::complex<float>>(n_samples);
        auto side_analytic = py::array_t<std::complex<float>>(n_samples);
        auto out_l = py::array_t<float>(n_samples);
        auto out_r = py::array_t<float>(n_samples);

        float* p_raw_l = static_cast<float*>(raw_l.request().ptr);
        float* p_raw_r = static_cast<float*>(raw_r.request().ptr);
        float* p_mid_pre = static_cast<float*>(mid_pre.request().ptr);
        float* p_side_pre = static_cast<float*>(side_pre.request().ptr);
        std::complex<float>* p_mid_post = static_cast<std::complex<float>*>(mid_analytic.request().ptr);
        std::complex<float>* p_side_post = static_cast<std::complex<float>*>(side_analytic.request().ptr);
        float* p_out_l = static_cast<float*>(out_l.request().ptr);
        float* p_out_r = static_cast<float*>(out_r.request().ptr);

        // Cached coefficients for Mode 0
        float static_cos, static_sin;
        lut_.get_sin_cos(static_phase_rad_, static_sin, static_cos);

        for (size_t i = 0; i < n_samples; ++i) {
            float in_l = ptr_l[i];
            float in_r = ptr_r[i];

            p_raw_l[i] = in_l;
            p_raw_r[i] = in_r;

            // 1. 100 Hz High-Pass Filter
            float hp_l = hpf_left_.process(in_l);
            float hp_r = hpf_right_.process(in_r);

            // 2. M/S Extraction (Pre-Hilbert)
            float mid = 0.5f * (hp_l + hp_r);
            float side = 0.5f * (hp_l - hp_r);
            p_mid_pre[i] = mid;
            p_side_pre[i] = side;

            // 3. Analytic Signal Generation via Matched Delay + Hilbert FIR
            float mid_i, mid_q;
            float side_i, side_q;
            hilbert_mid_.process(mid, mid_i, mid_q);
            hilbert_side_.process(side, side_i, side_q);

            p_mid_post[i] = std::complex<float>(mid_i, mid_q);
            p_side_post[i] = std::complex<float>(side_i, side_q);

            // 4. Selectable Processing & Modulation Modes
            float mod_mid = 0.0f;
            float mod_side = 0.0f;

            switch (mode_) {
                case MODE_PHASE_SHIFT: {
                    // Mode 1: Static Phase Shift
                    mod_mid  = (mid_i * static_cos)  - (mid_q * static_sin);
                    mod_side = (side_i * static_cos) - (side_q * static_sin);
                    break;
                }
                case MODE_PHASE_MODULATOR: {
                    // Mode 2: LFO Phase Modulator (LUT-driven)
                    float lfo_val = lut_.sin(lfo_phase_);
                    lfo_phase_ += lfo_inc_;
                    if (lfo_phase_ >= TWO_PI_F) lfo_phase_ -= TWO_PI_F;

                    float current_theta = mod_depth_ * lfo_val;
                    float lfo_sin, lfo_cos;
                    lut_.get_sin_cos(current_theta, lfo_sin, lfo_cos);

                    mod_mid  = (mid_i * lfo_cos)  - (mid_q * lfo_sin);
                    mod_side = (side_i * lfo_cos) - (side_q * lfo_sin);
                    break;
                }
                case MODE_FREQUENCY_SHIFT: {
                    // Mode 3: Continuous Frequency Shift (Single-Sideband NCO)
                    float nco_cos, nco_sin;
                    osc_nco_.process(nco_cos, nco_sin);

                    mod_mid  = (mid_i * nco_cos)  - (mid_q * nco_sin);
                    mod_side = (side_i * nco_cos) - (side_q * nco_sin);
                    break;
                }
            }

            // 5. Stereo Reconstruction Matrix
            p_out_l[i] = mod_mid + mod_side;
            p_out_r[i] = mod_mid - mod_side;
        }

        py::dict res;
        res["raw_l"] = raw_l;
        res["raw_r"] = raw_r;
        res["mid_pre"] = mid_pre;
        res["side_pre"] = side_pre;
        res["mid_post"] = mid_analytic;
        res["side_post"] = side_analytic;
        res["out_l"] = out_l;
        res["out_r"] = out_r;
        return res;
    }

private:
    float sample_rate_;
    MixingMode mode_ = MODE_PHASE_SHIFT;

    // Filter Modules
    BiquadHPF hpf_left_;
    BiquadHPF hpf_right_;
    HilbertTransformer hilbert_mid_;
    HilbertTransformer hilbert_side_;

    // Modulation Modules
    FastTrigLUT lut_;
    RecursiveQuadratureOscillator osc_nco_;

    // Modulation States
    float static_phase_rad_ = 0.0f;
    float lfo_phase_ = 0.0f;
    float lfo_inc_ = 0.0f;
    float mod_depth_ = 0.0f;
};

PYBIND11_MODULE(dsp_engine, m) {
    py::enum_<MixingMode>(m, "MixingMode")
        .value("MODE_PHASE_SHIFT", MODE_PHASE_SHIFT)
        .value("MODE_PHASE_MODULATOR", MODE_PHASE_MODULATOR)
        .value("MODE_FREQUENCY_SHIFT", MODE_FREQUENCY_SHIFT)
        .export_values();

    py::class_<AudioDSPPipeline>(m, "AudioDSPPipeline")
        .def(py::init<float>(), py::arg("sample_rate") = 48000.0f)
        .def("set_mode", &AudioDSPPipeline::set_mode)
        .def("set_phase_shift", &AudioDSPPipeline::set_phase_shift)
        .def("set_phase_modulator", &AudioDSPPipeline::set_phase_modulator)
        .def("set_dfs_offset", &AudioDSPPipeline::set_dfs_offset)
        .def("process_frames", &AudioDSPPipeline::process_frames)
        .def("reset", &AudioDSPPipeline::reset);
}