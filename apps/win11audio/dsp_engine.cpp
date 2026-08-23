#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <cmath>
#include <vector>
#include <array>
#include <complex>
#include <algorithm>

namespace py = pybind11;

constexpr float PI_F = 3.14159265358979323846f;
constexpr float TWO_PI_F = 6.28318530717958647692f;
constexpr float HPF_CUTOFF_HZ = 150.0f; // Pre-Hilbert HPF cutoff frequency in Hz
constexpr float LPF_CUTOFF_HZ = 150.0f; // Low-end bypass LPF cutoff frequency in Hz
constexpr float MIX_MS_DEFAULT = 0.5f;  // Default mixing factor between center anchor and wet side channel
constexpr int HILBERT_TAPS = 31;
constexpr int HILBERT_DELAY = (HILBERT_TAPS - 1) / 2; // 15 samples
constexpr int LUT_SIZE = 1024;
constexpr int LUT_MASK = LUT_SIZE - 1;
constexpr int MAX_OUTPUT_CHANNELS = 8;

// Input mixing (passive matrix)
enum InputMatrixMode {
    INPUT_MONO = 0,    // dry0 = in_l, dry1 = in_l
    INPUT_STEREO = 1,  // dry0 = in_l, dry1 = in_r
    INPUT_MS = 2       // dry0 = Mid 0.5*(L+R), dry1 = Side 0.5*(L-R)
};

// Spatial audio algorithms
enum SpatialAlgorithm {
    SPATIAL_BYPASS = 0,       // 0. All bypass (raw input -> output)
    SPATIAL_BASIC = 1,        // 1. Basic: Fixed phase shift, no freq shift
    SPATIAL_BASIC_PLUS = 2,   // 2. Basic+: Fixed phase shift + fixed frequency shift
    SPATIAL_SWIRL = 3,        // 3. Swirl/Distributed Chorus: Fixed phase shift + slow (~0.3 Hz) DFS
    SPATIAL_ROTARY = 4,       // 4. Spatial rotary: Unique prime LFO modulation per channel (0.11 - 0.23 Hz)
    SPATIAL_HETERODYNE = 5    // 5. Fractional Heterodyning: Stepped DFS across outputs [0, 0.4, 0.8, 1.2] Hz
};

// Backwards-compatible alias for previous mixing modes
enum MixingMode {
    MODE_PHASE_SHIFT = SPATIAL_BASIC,
    MODE_PHASE_MODULATOR = SPATIAL_ROTARY,
    MODE_FREQUENCY_SHIFT = SPATIAL_BASIC_PLUS
};

// Hilbert transform implementation type
enum HilbertType {
    HILBERT_FIR = 0,          // 31-Tap Type-III FIR (linear phase, 15-sample delay)
    HILBERT_IIR_ALLPASS = 1   // 2x3-Pole Cascaded Allpass IIR (low CPU weight, minimal latency)
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

    inline float sin(float rad) const {
        float phase_norm = rad * (static_cast<float>(LUT_SIZE) / TWO_PI_F);
        int idx0 = static_cast<int>(std::floor(phase_norm));
        float frac = phase_norm - static_cast<float>(idx0);
        
        int i0 = idx0 & LUT_MASK;
        int i1 = (i0 + 1) & LUT_MASK;
        return (1.0f - frac) * lut_[i0] + frac * lut_[i1];
    }

    inline float cos(float rad) const {
        return sin(rad + (0.5f * PI_F));
    }

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
        if (sample_rate > 0.0f) {
            float w0 = TWO_PI_F * delta_f_hz / sample_rate;
            cos_w_ = std::cos(w0);
            sin_w_ = std::sin(w0);
        }
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
// 2nd-Order Butterworth High-Pass Biquad (Direct Form II Transposed)
// -----------------------------------------------------------------------------
class BiquadHPF {
public:
    BiquadHPF() { reset(); }

    void init(float sample_rate, float cutoff_freq = HPF_CUTOFF_HZ, float q = 0.70710678f) {
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
// 2nd-Order Butterworth Low-Pass Biquad (Direct Form II Transposed)
// -----------------------------------------------------------------------------
class BiquadLPF {
public:
    BiquadLPF() { reset(); }

    void init(float sample_rate, float cutoff_freq = LPF_CUTOFF_HZ, float q = 0.70710678f) {
        float w0 = TWO_PI_F * cutoff_freq / sample_rate;
        float alpha = std::sin(w0) / (2.0f * q);
        float cos_w0 = std::cos(w0);

        float b0 =  (1.0f - cos_w0) * 0.5f;
        float b1 =   1.0f - cos_w0;
        float b2 =  (1.0f - cos_w0) * 0.5f;
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
    float b0_ = 0.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
    float s1_ = 0.0f, s2_ = 0.0f;
};

// -----------------------------------------------------------------------------
// 31-Tap Type-III FIR Hilbert Transformer (Quadrature Splitter)
// -----------------------------------------------------------------------------
class HilbertFIR {
public:
    HilbertFIR() {
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

// Backward-compatible alias
using HilbertTransformer = HilbertFIR;

// -----------------------------------------------------------------------------
// 2x3-Pole Cascaded Allpass IIR Hilbert Transformer (from forestChirp audio_output.h)
// -----------------------------------------------------------------------------
class HilbertIIR {
public:
    HilbertIIR() { reset(); }

    void reset() {
        xA0_ = yA0_ = xA1_ = yA1_ = xA2_ = yA2_ = 0.0f;
        xB0_ = yB0_ = xB1_ = yB1_ = xB2_ = yB2_ = 0.0f;
    }

    inline void process(float in, float& i_out, float& q_out) {
        // Path A (In-Phase Real Component)
        float outA0 = allpass(in, a0, xA0_, yA0_);
        float outA1 = allpass(outA0, a1, xA1_, yA1_);
        i_out = allpass(outA1, a2, xA2_, yA2_);

        // Path B (Quadrature Imaginary Component)
        float outB0 = allpass(in, b0, xB0_, yB0_);
        float outB1 = allpass(outB0, b1, xB1_, yB1_);
        q_out = allpass(outB1, b2, xB2_, yB2_);
    }

private:
    static constexpr float a0 = 0.161758f;
    static constexpr float a1 = 0.733029f;
    static constexpr float a2 = 0.945349f;
    static constexpr float b0 = 0.479401f;
    static constexpr float b1 = 0.876218f;
    static constexpr float b2 = 0.985953f;

    float xA0_, yA0_, xA1_, yA1_, xA2_, yA2_;
    float xB0_, yB0_, xB1_, yB1_, xB2_, yB2_;

    static inline float allpass(float x, float pole, float& x_z1, float& y_z1) {
        float y = pole * (x + y_z1) - x_z1;
        x_z1 = x;
        y_z1 = y;
        return y;
    }
};

using HilbertIIRAllpass = HilbertIIR;

// -----------------------------------------------------------------------------
// Complete Hybrid C++ Audio DSP Pipeline
// -----------------------------------------------------------------------------
class AudioDSPPipeline {
public:
    AudioDSPPipeline(float sample_rate = 48000.0f, float hpf_cutoff = HPF_CUTOFF_HZ, float mix_ms = MIX_MS_DEFAULT)
        : sample_rate_(sample_rate), hpf_cutoff_hz_(hpf_cutoff), lpf_cutoff_hz_(hpf_cutoff), mix_ms_(mix_ms) {
        init_filters();
        init_oscillators();
    }

    void set_input_mode(int mode) {
        input_mode_ = static_cast<InputMatrixMode>(mode);
    }

    int get_input_mode() const {
        return static_cast<int>(input_mode_);
    }

    void set_spatial_algorithm(int algo) {
        spatial_algo_ = static_cast<SpatialAlgorithm>(algo);
        update_oscillators();
    }

    int get_spatial_algorithm() const {
        return static_cast<int>(spatial_algo_);
    }

    // Backwards-compatible alias for set_mode
    void set_mode(int mode) {
        set_spatial_algorithm(mode);
    }

    void set_hilbert_type(int type) {
        hilbert_type_ = static_cast<HilbertType>(type);
    }

    int get_hilbert_type() const {
        return static_cast<int>(hilbert_type_);
    }

    void set_num_channels(int n) {
        num_channels_ = std::max(1, std::min(n, MAX_OUTPUT_CHANNELS));
        update_oscillators();
    }

    int get_num_channels() const {
        return num_channels_;
    }

    void set_mix_ms(float mix) {
        mix_ms_ = std::max(0.0f, std::min(mix, 1.0f));
    }

    float get_mix_ms() const {
        return mix_ms_;
    }

    void set_hpf_cutoff(float cutoff_hz) {
        hpf_cutoff_hz_ = cutoff_hz;
        lpf_cutoff_hz_ = cutoff_hz;
        init_filters();
    }

    float get_hpf_cutoff() const {
        return hpf_cutoff_hz_;
    }

    void set_phase_shift(float phase_rad) {
        user_phase_shift_rad_ = phase_rad;
    }

    void set_phase_shift_deg(float deg) {
        user_phase_shift_rad_ = deg * (PI_F / 180.0f);
    }

    float get_phase_shift_deg() const {
        return user_phase_shift_rad_ * (180.0f / PI_F);
    }

    void set_dfs_offset(float delta_f_hz) {
        global_dfs_offset_hz_ = delta_f_hz;
        update_oscillators();
    }

    float get_dfs_offset() const {
        return global_dfs_offset_hz_;
    }

    void set_dfs_step(float step_hz) {
        heterodyne_step_hz_ = step_hz;
        update_oscillators();
    }

    float get_dfs_step() const {
        return heterodyne_step_hz_;
    }

    void set_rotary_params(float lfo_freq_scale, float depth_deg) {
        rotary_lfo_scale_ = lfo_freq_scale;
        rotary_depth_rad_ = depth_deg * (PI_F / 180.0f);
    }

    // Backwards-compatible alias for set_phase_modulator
    void set_phase_modulator(float lfo_freq_hz, float depth_rad) {
        rotary_lfo_scale_ = lfo_freq_hz;
        rotary_depth_rad_ = depth_rad;
    }

    void reset() {
        hpf_dry0_.reset();
        hpf_dry1_.reset();
        lpf_dry0_.reset();
        lpf_dry1_.reset();
        hilbert_fir_dry0_.reset();
        hilbert_fir_dry1_.reset();
        hilbert_iir_dry0_.reset();
        hilbert_iir_dry1_.reset();

        for (int ch = 0; ch < MAX_OUTPUT_CHANNELS; ++ch) {
            osc_nco_[ch].reset();
            lfo_phase_[ch] = 0.0f;
        }
    }

    py::dict process_frames(py::array_t<float> input_l, py::array_t<float> input_r) {
        auto buf_l = input_l.request();
        auto buf_r = input_r.request();
        size_t n_samples = buf_l.size;

        const float* ptr_l = static_cast<const float*>(buf_l.ptr);
        const float* ptr_r = static_cast<const float*>(buf_r.ptr);

        // Pre-allocate NumPy output buffers
        auto raw_l = py::array_t<float>(n_samples);
        auto raw_r = py::array_t<float>(n_samples);
        auto dry0_lpf_arr = py::array_t<float>(n_samples);
        auto dry0_hpf_arr = py::array_t<float>(n_samples);
        auto dry1_lpf_arr = py::array_t<float>(n_samples);
        auto dry1_hpf_arr = py::array_t<float>(n_samples);
        auto mid_pre = py::array_t<float>(n_samples);
        auto side_pre = py::array_t<float>(n_samples);
        auto mid_analytic = py::array_t<std::complex<float>>(n_samples);
        auto side_analytic = py::array_t<std::complex<float>>(n_samples);
        auto out_l = py::array_t<float>(n_samples);
        auto out_r = py::array_t<float>(n_samples);
        
        // Multi-channel matrix output: shape (num_channels, n_samples)
        std::vector<size_t> shape_out = { static_cast<size_t>(num_channels_), n_samples };
        std::vector<size_t> strides_out = { n_samples * sizeof(float), sizeof(float) };
        auto out_channels = py::array_t<float>(shape_out, strides_out);

        float* p_raw_l = static_cast<float*>(raw_l.request().ptr);
        float* p_raw_r = static_cast<float*>(raw_r.request().ptr);
        float* p_dry0_lpf = static_cast<float*>(dry0_lpf_arr.request().ptr);
        float* p_dry0_hpf = static_cast<float*>(dry0_hpf_arr.request().ptr);
        float* p_dry1_lpf = static_cast<float*>(dry1_lpf_arr.request().ptr);
        float* p_dry1_hpf = static_cast<float*>(dry1_hpf_arr.request().ptr);
        float* p_mid_pre = static_cast<float*>(mid_pre.request().ptr);
        float* p_side_pre = static_cast<float*>(side_pre.request().ptr);
        std::complex<float>* p_mid_post = static_cast<std::complex<float>*>(mid_analytic.request().ptr);
        std::complex<float>* p_side_post = static_cast<std::complex<float>*>(side_analytic.request().ptr);
        float* p_out_l = static_cast<float*>(out_l.request().ptr);
        float* p_out_r = static_cast<float*>(out_r.request().ptr);
        float* p_out_ch = static_cast<float*>(out_channels.request().ptr);

        const float prime_bases[MAX_OUTPUT_CHANNELS] = {
            0.11f, 0.13f, 0.17f, 0.19f, 0.23f, 0.29f, 0.31f, 0.37f
        };

        for (size_t i = 0; i < n_samples; ++i) {
            float in_l = ptr_l[i];
            float in_r = ptr_r[i];
            p_raw_l[i] = in_l;
            p_raw_r[i] = in_r;

            // -------------------------------------------------------------
            // 1. Input Mixing (Passive Matrix)
            // -------------------------------------------------------------
            float dry0 = in_l;
            float dry1 = in_r;
            if (input_mode_ == INPUT_MONO) {
                float mono_in = 0.5f * (in_l + in_r);
                dry0 = mono_in;
                dry1 = mono_in;
            } else if (input_mode_ == INPUT_MS) {
                dry0 = 0.5f * (in_l + in_r); // Mid
                dry1 = 0.5f * (in_l - in_r); // Side
            }
            p_mid_pre[i] = dry0;
            p_side_pre[i] = dry1;

            // -------------------------------------------------------------
            // 2. Pre-Hilbert Conditioning (HPF & LPF Crossover)
            // -------------------------------------------------------------
            float dry0_hpf = hpf_dry0_.process(dry0);
            float dry0_lpf = lpf_dry0_.process(dry0);
            float dry1_hpf = hpf_dry1_.process(dry1);
            float dry1_lpf = lpf_dry1_.process(dry1);

            p_dry0_hpf[i] = dry0_hpf;
            p_dry0_lpf[i] = dry0_lpf;
            p_dry1_hpf[i] = dry1_hpf;
            p_dry1_lpf[i] = dry1_lpf;

            // -------------------------------------------------------------
            // 3. Hilbert Transform Quadrature Splitter
            // -------------------------------------------------------------
            float dry0_i = dry0_hpf, dry0_q = 0.0f;
            float dry1_i = dry1_hpf, dry1_q = 0.0f;

            if (input_mode_ == INPUT_MS) {
                // In MS mode: only Side channel is passed through Hilbert.
                // Mid channel is preserved untouched as center anchor.
                dry0_i = dry0_hpf;
                dry0_q = 0.0f;
                if (hilbert_type_ == HILBERT_FIR) {
                    hilbert_fir_dry1_.process(dry1_hpf, dry1_i, dry1_q);
                } else {
                    hilbert_iir_dry1_.process(dry1_hpf, dry1_i, dry1_q);
                }
            } else {
                // Mono or Stereo mode: Hilbert applied to input channels
                if (hilbert_type_ == HILBERT_FIR) {
                    hilbert_fir_dry0_.process(dry0_hpf, dry0_i, dry0_q);
                    hilbert_fir_dry1_.process(dry1_hpf, dry1_i, dry1_q);
                } else {
                    hilbert_iir_dry0_.process(dry0_hpf, dry0_i, dry0_q);
                    hilbert_iir_dry1_.process(dry1_hpf, dry1_i, dry1_q);
                }
            }

            p_mid_post[i] = std::complex<float>(dry0_i, dry0_q);
            p_side_post[i] = std::complex<float>(dry1_i, dry1_q);

            // -------------------------------------------------------------
            // 4. Spatial Audio Algorithms & Multi-Channel Rendering
            // -------------------------------------------------------------
            for (int ch = 0; ch < num_channels_; ++ch) {
                float ch_out = 0.0f;

                if (spatial_algo_ == SPATIAL_BYPASS) {
                    // Bypass mode: output raw input
                    if (ch == 0) ch_out = in_l;
                    else if (ch == 1) ch_out = in_r;
                    else ch_out = 0.5f * (in_l + in_r);
                } else {
                    // Spatial modulation angle calculation
                    // Node/Channel static angle: 0, 180 deg for stereo; k * 360/N for multi-channel
                    float ch_base_rad = (num_channels_ > 1) 
                        ? (static_cast<float>(ch) * TWO_PI_F / static_cast<float>(num_channels_))
                        : 0.0f;
                    
                    float theta = ch_base_rad + user_phase_shift_rad_;
                    float rot_cos = 1.0f, rot_sin = 0.0f;

                    if (spatial_algo_ == SPATIAL_BASIC) {
                        // 1. Basic: Fixed phase shift, no freq shift
                        lut_.get_sin_cos(theta, rot_sin, rot_cos);
                    } else if (spatial_algo_ == SPATIAL_BASIC_PLUS || spatial_algo_ == SPATIAL_SWIRL) {
                        // 2. Basic+ / 3. Swirl: Fixed phase shift + continuous NCO DFS rotation
                        float nco_c, nco_s;
                        osc_nco_[ch].process(nco_c, nco_s);
                        float base_s, base_c;
                        lut_.get_sin_cos(theta, base_s, base_c);
                        rot_cos = (base_c * nco_c) - (base_s * nco_s);
                        rot_sin = (base_s * nco_c) + (base_c * nco_s);
                    } else if (spatial_algo_ == SPATIAL_ROTARY) {
                        // 4. Spatial rotary: Unique prime LFO modulation per channel
                        float lfo_freq = prime_bases[ch % MAX_OUTPUT_CHANNELS] * rotary_lfo_scale_;
                        float lfo_inc = TWO_PI_F * lfo_freq / sample_rate_;
                        lfo_phase_[ch] += lfo_inc;
                        if (lfo_phase_[ch] >= TWO_PI_F) lfo_phase_[ch] -= TWO_PI_F;

                        float lfo_val = lut_.sin(lfo_phase_[ch]);
                        float mod_angle = theta + (rotary_depth_rad_ * lfo_val);
                        lut_.get_sin_cos(mod_angle, rot_sin, rot_cos);
                    } else if (spatial_algo_ == SPATIAL_HETERODYNE) {
                        // 5. Fractional Heterodyning: Stepped DFS NCO rotation per output
                        float nco_c, nco_s;
                        osc_nco_[ch].process(nco_c, nco_s);
                        rot_cos = nco_c;
                        rot_sin = nco_s;
                    }

                    // Apply rotation to analytic signal & perform Output Mixing
                    if (input_mode_ == INPUT_MS) {
                        // MS Mode:
                        // S_wet = side_i * rot_cos - side_q * rot_sin
                        float side_rotated = (dry1_i * rot_cos) - (dry1_q * rot_sin);
                        
                        // For stereo: ch0 gets +side_rotated, ch1 gets -side_rotated
                        float side_mult = (num_channels_ == 2 && ch == 1) ? -1.0f : 1.0f;
                        float wet_hpf = (1.0f - mix_ms_) * dry0_hpf + (mix_ms_ * side_rotated * side_mult);
                        
                        // Output = Mid_LPF + Wet_HPF
                        ch_out = dry0_lpf + wet_hpf;
                    } else if (input_mode_ == INPUT_MONO) {
                        // Mono Mode:
                        // out = dry0_lpf + wet_hpf
                        float wet_hpf = (dry0_i * rot_cos) - (dry0_q * rot_sin);
                        ch_out = dry0_lpf + wet_hpf;
                    } else {
                        // Stereo Mode:
                        float src_i = (ch % 2 == 0) ? dry0_i : dry1_i;
                        float src_q = (ch % 2 == 0) ? dry0_q : dry1_q;
                        float src_lpf = (ch % 2 == 0) ? dry0_lpf : dry1_lpf;
                        float wet_hpf = (src_i * rot_cos) - (src_q * rot_sin);
                        ch_out = src_lpf + wet_hpf;
                    }
                }

                // Store in multi-channel buffer
                p_out_ch[ch * n_samples + i] = ch_out;
            }

            // Assign stereo outputs
            p_out_l[i] = p_out_ch[0 * n_samples + i];
            p_out_r[i] = (num_channels_ > 1) ? p_out_ch[1 * n_samples + i] : p_out_ch[0 * n_samples + i];
        }

        py::dict res;
        res["raw_l"] = raw_l;
        res["raw_r"] = raw_r;
        res["dry0_lpf"] = dry0_lpf_arr;
        res["dry0_hpf"] = dry0_hpf_arr;
        res["dry1_lpf"] = dry1_lpf_arr;
        res["dry1_hpf"] = dry1_hpf_arr;
        res["mid_pre"] = mid_pre;
        res["side_pre"] = side_pre;
        res["mid_post"] = mid_analytic;
        res["side_post"] = side_analytic;
        res["out_l"] = out_l;
        res["out_r"] = out_r;
        res["out_channels"] = out_channels;
        return res;
    }

private:
    float sample_rate_;
    float hpf_cutoff_hz_ = HPF_CUTOFF_HZ;
    float lpf_cutoff_hz_ = LPF_CUTOFF_HZ;
    float mix_ms_ = MIX_MS_DEFAULT;
    int num_channels_ = 2;

    InputMatrixMode input_mode_ = INPUT_MS;
    SpatialAlgorithm spatial_algo_ = SPATIAL_BASIC_PLUS;
    HilbertType hilbert_type_ = HILBERT_FIR;

    // Filters for Dry0 (L / Mid) and Dry1 (R / Side)
    BiquadHPF hpf_dry0_;
    BiquadHPF hpf_dry1_;
    BiquadLPF lpf_dry0_;
    BiquadLPF lpf_dry1_;

    HilbertFIR hilbert_fir_dry0_;
    HilbertFIR hilbert_fir_dry1_;
    HilbertIIR hilbert_iir_dry0_;
    HilbertIIR hilbert_iir_dry1_;

    // Fast Trigonometry & Modulation States
    FastTrigLUT lut_;
    std::array<RecursiveQuadratureOscillator, MAX_OUTPUT_CHANNELS> osc_nco_;
    std::array<float, MAX_OUTPUT_CHANNELS> lfo_phase_{};

    float user_phase_shift_rad_ = 0.0f;
    float global_dfs_offset_hz_ = 5.0f;
    float heterodyne_step_hz_ = 0.4f;
    float rotary_lfo_scale_ = 1.0f;
    float rotary_depth_rad_ = 90.0f * (PI_F / 180.0f);

    void init_filters() {
        hpf_dry0_.init(sample_rate_, hpf_cutoff_hz_);
        hpf_dry1_.init(sample_rate_, hpf_cutoff_hz_);
        lpf_dry0_.init(sample_rate_, lpf_cutoff_hz_);
        lpf_dry1_.init(sample_rate_, lpf_cutoff_hz_);
    }

    void init_oscillators() {
        for (int ch = 0; ch < MAX_OUTPUT_CHANNELS; ++ch) {
            osc_nco_[ch].reset();
            lfo_phase_[ch] = 0.0f;
        }
        update_oscillators();
    }

    void update_oscillators() {
        for (int ch = 0; ch < MAX_OUTPUT_CHANNELS; ++ch) {
            if (spatial_algo_ == SPATIAL_SWIRL) {
                osc_nco_[ch].set_frequency(0.3f, sample_rate_);
            } else if (spatial_algo_ == SPATIAL_HETERODYNE) {
                float df = static_cast<float>(ch) * heterodyne_step_hz_;
                osc_nco_[ch].set_frequency(df, sample_rate_);
            } else {
                osc_nco_[ch].set_frequency(global_dfs_offset_hz_, sample_rate_);
            }
        }
    }
};

PYBIND11_MODULE(dsp_engine, m) {
    m.attr("HPF_CUTOFF_HZ") = HPF_CUTOFF_HZ;
    m.attr("LPF_CUTOFF_HZ") = LPF_CUTOFF_HZ;
    m.attr("MIX_MS_DEFAULT") = MIX_MS_DEFAULT;

    py::enum_<InputMatrixMode>(m, "InputMatrixMode")
        .value("INPUT_MONO", INPUT_MONO)
        .value("INPUT_STEREO", INPUT_STEREO)
        .value("INPUT_MS", INPUT_MS)
        .export_values();

    py::enum_<SpatialAlgorithm>(m, "SpatialAlgorithm")
        .value("SPATIAL_BYPASS", SPATIAL_BYPASS)
        .value("SPATIAL_BASIC", SPATIAL_BASIC)
        .value("SPATIAL_BASIC_PLUS", SPATIAL_BASIC_PLUS)
        .value("SPATIAL_SWIRL", SPATIAL_SWIRL)
        .value("SPATIAL_ROTARY", SPATIAL_ROTARY)
        .value("SPATIAL_HETERODYNE", SPATIAL_HETERODYNE)
        .export_values();

    // Backwards-compatible MixingMode enum
    py::enum_<MixingMode>(m, "MixingMode")
        .value("MODE_PHASE_SHIFT", MODE_PHASE_SHIFT)
        .value("MODE_PHASE_MODULATOR", MODE_PHASE_MODULATOR)
        .value("MODE_FREQUENCY_SHIFT", MODE_FREQUENCY_SHIFT)
        .export_values();

    py::enum_<HilbertType>(m, "HilbertType")
        .value("HILBERT_FIR", HILBERT_FIR)
        .value("HILBERT_IIR_ALLPASS", HILBERT_IIR_ALLPASS)
        .export_values();

    py::class_<AudioDSPPipeline>(m, "AudioDSPPipeline")
        .def(py::init<float, float, float>(),
             py::arg("sample_rate") = 48000.0f,
             py::arg("hpf_cutoff") = HPF_CUTOFF_HZ,
             py::arg("mix_ms") = MIX_MS_DEFAULT)
        .def("set_input_mode", &AudioDSPPipeline::set_input_mode)
        .def("get_input_mode", &AudioDSPPipeline::get_input_mode)
        .def("set_spatial_algorithm", &AudioDSPPipeline::set_spatial_algorithm)
        .def("get_spatial_algorithm", &AudioDSPPipeline::get_spatial_algorithm)
        .def("set_mode", &AudioDSPPipeline::set_mode)
        .def("set_hilbert_type", &AudioDSPPipeline::set_hilbert_type)
        .def("get_hilbert_type", &AudioDSPPipeline::get_hilbert_type)
        .def("set_num_channels", &AudioDSPPipeline::set_num_channels)
        .def("get_num_channels", &AudioDSPPipeline::get_num_channels)
        .def("set_mix_ms", &AudioDSPPipeline::set_mix_ms)
        .def("get_mix_ms", &AudioDSPPipeline::get_mix_ms)
        .def("set_hpf_cutoff", &AudioDSPPipeline::set_hpf_cutoff)
        .def("get_hpf_cutoff", &AudioDSPPipeline::get_hpf_cutoff)
        .def("set_phase_shift", &AudioDSPPipeline::set_phase_shift)
        .def("set_phase_shift_deg", &AudioDSPPipeline::set_phase_shift_deg)
        .def("get_phase_shift_deg", &AudioDSPPipeline::get_phase_shift_deg)
        .def("set_dfs_offset", &AudioDSPPipeline::set_dfs_offset)
        .def("get_dfs_offset", &AudioDSPPipeline::get_dfs_offset)
        .def("set_dfs_step", &AudioDSPPipeline::set_dfs_step)
        .def("get_dfs_step", &AudioDSPPipeline::get_dfs_step)
        .def("set_rotary_params", &AudioDSPPipeline::set_rotary_params)
        .def("set_phase_modulator", &AudioDSPPipeline::set_phase_modulator)
        .def("process_frames", &AudioDSPPipeline::process_frames)
        .def("reset", &AudioDSPPipeline::reset);
}