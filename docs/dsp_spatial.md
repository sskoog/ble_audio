# Audio DSP and Effects Documentation

This document describes the digital signal processing (DSP) pipeline implemented for the forestChirp nodes to process high-fidelity spatial audio.

---

## 1. Hilbert Transform & Post-Mixing (DQS and DFS)
Spatial localization and movement are achieved using a wideband **Hilbert Transformer** combined with Single Sideband (SSB) modulation.


### Path Separation (SpatialAllpassSplitter):
The mono input is split into two orthogonal paths (Real and Imaginary) with a constant $90^\circ$ relative phase shift across the audible frequency band. This is implemented via a dual-path allpass filter structure:
* **Real Path (Path A):** 3 cascaded allpass stages with poles at:
  * $a_0 = 0.161758f$
  * $a_1 = 0.733029f$
  * $a_2 = 0.945349f$
* **Imaginary Path (Path B):** 3 cascaded allpass stages with poles at:
  * $b_0 = 0.479401f$
  * $b_1 = 0.876218f$
  * $b_2 = 0.985953f$

### DQS (Dynamic Quadrature Splitter):
Adds a static phase offset ($\theta$) determined by `activeDqsDegrees` (mapped from Potentiometer 1):
$$\theta = \text{activeDqsDegrees} \cdot \frac{\pi}{180}$$

### DFS (Dynamic Frequency Shifter):
Shifts the carrier frequency continuously by incrementing a phase accumulator ($\phi$) at every sample period:
$$\Delta\phi = \frac{2\pi \cdot \text{activeDfsFreq_mHz}}{1000 \cdot f_s}$$
$$\phi[n] = (\phi[n-1] + \Delta\phi) \pmod{2\pi}$$
Where `activeDfsFreq_mHz` represents the frequency shift in millihertz (mapped from Potentiometer 2).

### Post-Mixing (SSB Modulation):
The orthogonal filter outputs (`outReal` and `outImag`) are combined with the accumulated phase `totalPhase = phi + theta` using single-sideband modulation to output a single real value:
$$y[n] = \text{outReal}[n] \cdot \cos(\phi + \theta) - \text{outImag}[n] \cdot \sin(\phi + \theta)$$


---

## Foundational DSP Math
All effects below rely on the extraction of the Analytic Signal via an IIR-based Hilbert transform.
For an input signal $x[n]$, the analytic signal is $x_a[n] = x_i[n] + j x_q[n]$, where $x_i[n]$ is the in-phase component (approximately $x[n]$ delayed) and $x_q[n]$ is the quadrature component $\mathcal{H}\{x[n]\}$.

Phase rotation by an angle $\theta$ is defined by:
$$y[n] = \Re\{x_a[n] \cdot e^{j\theta}\} = x_i[n]\cos(\theta) - x_q[n]\sin(\theta)$$

---

## Algorithm 1: Synthetic M/S via Parallel Hilbert Injection (The "Distributed Chorus")
**Input:** Mono audio ($x[n]$)
**Goal:** Create a synthetic diffuse field from a flat mono source while keeping the rhythmic core anchored.

### Theory
The dry mono signal acts as the physical acoustic anchor to trigger the Haas precedence effect. A parallel processing path runs the signal through a Hilbert transform, rotates the phase symmetrically across the 5 nodes, and shifts the frequency slightly. The synthetic path is then summed back with the dry path.

### Mathematical Definition
$$y_k[n] = x[n] + \alpha \cdot \Re\left\{ x_a[n] \cdot e^{j\theta_k} \cdot e^{j 2\pi \Delta f_k n / f_s} \right\}$$
Where:
*   $k$ is the node index (0 to 4).
*   $\alpha$ is the wet/dry mix coefficient (e.g., 0.5).
*   $\theta_k$ is the static DQS phase offset (e.g., $k \cdot 72^\circ$).
*   $\Delta f_k$ is a fractional frequency shift (e.g., 0.3 Hz) to create continuous phase slippage.

### Implementation Logic
1. Pass $x[n]$ through a 150 Hz HPF to protect drivers.
2. Route the filtered signal directly to a `dry_buffer`.
3. Route the filtered signal into the IIR Hilbert cascaded allpass filters to yield $x_i$ and $x_q$.
4. Calculate the total instantaneous rotation angle using an NCO configured for $\Delta f_k$, initialized with a starting phase of $\theta_k$.
5. Perform the complex multiply-accumulate to yield the `wet_buffer`.
6. Output `out[n] = dry_buffer[n] + (alpha * wet_buffer[n])`.

---

## Algorithm 2: The Spatial Rotary (Asynchronous LFO Phase Animation)
**Input:** Mono audio ($x[n]$)
**Goal:** Create a slow-motion, sweeping "Leslie Speaker" effect moving physically through the forest.

### Theory
Instead of a static phase shift $\theta_k$ or a constant linear frequency shift $\Delta f_k$, the phase angle is modulated by a slow, bounded Low-Frequency Oscillator (LFO). This results in Phase Modulation (PM), causing the pitch to bend continuously (Doppler chorus effect) and the physical interference nodes to sweep across the installation space.

### Mathematical Definition
$$y_k[n] = \Re\left\{ x_a[n] \cdot e^{j \beta \sin(2\pi f_{LFO, k} n / f_s)} \right\}$$
Where:
*   $\beta$ is the modulation depth in radians (e.g., $\pi$ for full inversion).
*   $f_{LFO, k}$ is the node-specific, mutually prime LFO frequency (e.g., 0.11 Hz, 0.13 Hz, 0.17 Hz).

### Implementation Logic
1. An NCO cannot be used here because the derivative of the phase angle is not constant.
2. Implement an LFO phase accumulator running at $f_{LFO, k}$.
3. For each sample, evaluate the LFO to get the instantaneous angle $\theta[n] = \beta \sin( LFO_phase )$.
4. Map $\theta[n]$ through the **Fast Trigonometry Approximation** (LUT or Parabolic) to get $\cos(\theta[n])$ and $\sin(\theta[n])$.
5. Apply the standard SSB rotation formula: `out[n] = (xi * cos_val) - (xq * sin_val)`.

---

## Algorithm 3: Fractional Heterodyning (The "Shepard Tone" Space)
**Input:** Mono audio ($x[n]$ - highly tonal, drone, or ambient material)
**Goal:** Generate physical acoustic beating and throbbing in the air without spatial comb filtering.

### Theory
By applying tiny, stepped constant frequency shifts across the array, the audio outputs become Single-Sideband (SSB) modulated variants of each other. In the open air, these varying frequencies physically heterodyne, creating continuous amplitude beating. Because the shifts are fractional ($<2$ Hz), the brain perceives spatial throbbing rather than dissonance.

### Mathematical Definition
$$y_k[n] = \Re\left\{ x_a[n] \cdot e^{j 2\pi \Delta f_k n / f_s} \right\}$$
Where:
*   $\Delta f_k$ are stepped linear shifts across the nodes (e.g., +0.0 Hz, +0.4 Hz, +0.8 Hz, +1.2 Hz, +1.6 Hz).
*   $\theta_k = 0^\circ$ (No static phase offset is required).

### Implementation Logic
1. This is a pure implementation of the Recursive Quadrature NCO.
2. The agent must construct the block processor such that `osc_x` and `osc_y` rotate constantly by $\omega = 2\pi\Delta f_k / f_s$.
3. Process the Hilbert transform, apply the NCO rotation, and output directly. No dry signal is mixed.
4. Ensure the NCO includes a periodic length-normalization check (e.g., every 512 samples) to prevent floating-point precision drift.

---

## Algorithm 4: Mid/Side Quadrature Phase/Frequency Shifting
**Input:** True Stereo audio ($L[n], R[n]$)
**Goal:** Expand the ambient/diffuse field to the physical limits of the forest while keeping rhythmic/centered elements perfectly localized and coherent.

### Theory
The stereo signal is matrixed into Mid ($M$) and Side ($S$). The correlated $M$ channel bypasses spatial manipulation to ensure stable Haas precedence effect localization. The uncorrelated $S$ channel undergoes Hilbert phase manipulation to expand the spatial field via two distinct psychoacoustic mechanisms:

*   **Hyper-Envelopment (IACC Minimization):** By symmetrically rotating the static phase $\theta_k$ (e.g., 0°, 72°, 144°, 216°, 288°) across the 5 nodes, the Interaural Cross-Correlation (IACC) of the diffuse field is driven toward zero. The listener's brain detects the ambient reflections but cannot assign a directional vector to them, causing the reverb to completely detach from the physical speakers and sound as if it is emanating from the outer boundaries of the forest itself.
*   **Organic Chorusing (DFS on Side Only):** By applying a small fractional frequency shift (e.g., $\Delta f_k = 0.2$ Hz) exclusively to the Side channel, the atmospheric reverbs slowly swirl and heterodyne in the open air. This creates a lush, three-dimensional acoustic chorus while the rhythm and vocals (the Mid channel) remain perfectly in tune and localized.

The processed $S$ channel and the dry $M$ channel are then summed back together locally.

### Mathematical Definition
$$M[n] = 0.5(L[n] + R[n])$$
$$S[n] = 0.5(L[n] - R[n])$$
$$y_k[n] = \text{HPF}_{150}\{M[n]\} + \Re\left\{ \left(\text{HPF}_{500}\{S[n]\} + j\mathcal{H}\{\text{HPF}_{500}\{S[n]\}\}\right) e^{j\theta_k} e^{j 2\pi \Delta f_k n / f_s} \right\}$$

### Implementation Logic
1. Create a processing block accepting interleaved stereo `float` buffers or dual mono arrays.
2. Apply $M$ and $S$ matrixing per sample.
3. Pass $M[n]$ through a 150 Hz Direct Form II Biquad HPF.
4. Pass $S[n]$ through a 500 Hz Direct Form II Biquad HPF. This high cutoff prevents limit-cycle ringing in the IIR Hilbert cascades.
5. Apply the IIR Hilbert transform cascades *only* to the filtered $S[n]$.
6. Compute the composite angle using the NCO (for $\Delta f_k$) mathematically combined with the static offset $\theta_k$.
7. Rotate the analytic $S$ signal.
8. Sum the filtered $M[n]$ and the rotated $S[n]$ to produce the final mono node output `out[n] = M_filt + S_rotated`.

-----


# DSP Optimization: Fast Trigonometry on ESP32-S3

**Target Hardware:** ESP32-S3 (Xtensa LX7, single-precision FPU)
**Context:** Real-time spatial audio processing (DQS/DFS) requires continuous phase rotation. Standard C++ math library calls to `sinf()` and `cosf()` rely on Taylor series or CORDIC algorithms that require 40 to 90 clock cycles per call. In a high-speed DSP block loop, this overhead will rapidly exhaust the CPU budget, leaving insufficient headroom for MP3 decoding and LoRa network handling.

Below are the three standard DSP strategies to eliminate standard trigonometric function calls in the inner audio loop, ordered by implementation priority and use-case.

---

## 1. The Recursive Quadrature Oscillator (NCO)
**Use Case:** Dynamic Frequency Shifting (DFS) where phase changes at a *constant rate*.

If the phase angle $\theta$ is changing at a constant rate (which mathematically defines a frequency shift), you do not need to calculate sine and cosine for every sample. You calculate `sinf(omega)` and `cosf(omega)` **once per block** when the frequency parameter changes. Then, for every audio sample, you rotate a complex vector.

### Implementation
```cpp
// Calculated once per block (when frequency changes)
float omega = 2.0f * PI * target_freq_Hz / sample_rate;
float cos_omega = cosf(omega);
float sin_omega = sinf(omega);

// Executed inside the per-sample audio loop
float next_x = osc_x * cos_omega - osc_y * sin_omega;
float next_y = osc_y * cos_omega + osc_x * sin_omega;
osc_x = next_x;
osc_y = next_y;

// Normalization (executed periodically, e.g., every 512 samples to prevent precision drift)
float len = sqrtf(osc_x * osc_x + osc_y * osc_y);
osc_x /= len;
osc_y /= len;
```
*   **Performance Profile:** Operates in roughly 6 clock cycles per sample. 
*   **Memory Cost:** None. Requires only four FPU multiplications and two additions.

---

## 2. Wavetable Synthesis (Lookup Tables)
**Use Case:** Arbitrary Phase Modulation (LFO to DQS phase angle).

If $\theta$ is arbitrary—such as when an LFO is continuously sweeping the DQS phase angle back and forth—an NCO cannot be used. You must map the arbitrary angle $\theta(t)$ to a trigonometric value using a pre-computed array (Lookup Table / LUT) stored in SRAM.

### Implementation
```cpp
class FastTrig {
private:
    static const int LUT_SIZE = 1024;
    float sin_lut[LUT_SIZE];
    const float INDEX_SCALE = (float)LUT_SIZE / (2.0f * PI);

public:
    FastTrig() {
        // Pre-compute during boot
        for (int i = 0; i < LUT_SIZE; i++) {
            sin_lut[i] = sinf((2.0f * PI * i) / LUT_SIZE);
        }
    }

    // Per-sample fast sine approximation
    inline float fast_sin(float phase_rad) {
        // Wrap phase to 0 -> 2*PI
        while (phase_rad < 0.0f) phase_rad += 2.0f * PI;
        while (phase_rad >= 2.0f * PI) phase_rad -= 2.0f * PI;

        // Calculate exact fractional index
        float float_idx = phase_rad * INDEX_SCALE;
        int idx0 = (int)float_idx;
        int idx1 = (idx0 + 1) % LUT_SIZE;
        float frac = float_idx - (float)idx0;

        // Linear interpolation
        return sin_lut[idx0] + frac * (sin_lut[idx1] - sin_lut[idx0]);
    }

    inline float fast_cos(float phase_rad) {
        return fast_sin(phase_rad + (PI / 2.0f));
    }
};
```
*   **Performance Profile:** Roughly 10-15 cycles. Highly deterministic execution time with zero branching in the FPU.
*   **Memory Cost:** A 1024-point `float` array consumes exactly **4 KB of internal SRAM**. 
*   **Audio Quality:** With linear interpolation, the Total Harmonic Distortion (THD) falls well below the noise floor of a 16-bit DAC.

---

## 3. Fast Parabolic Approximation
**Use Case:** Ultra-Low Memory Profile (when SRAM is fully allocated to FreeRTOS or MP3 buffers).

If the 4 KB SRAM cost of a LUT is prohibitive, you can use an algebraic approximation. A highly optimized demoscene algorithm approximates a sine wave using parabolas. For an angle $x$ wrapped between $-\pi$ and $\pi$, the sine curve can be approximated using FPU Multiply-Accumulate (MAC) instructions:

$$\sin(x) \approx \frac{4}{\pi}x - \frac{4}{\pi^2}x|x|$$

### Implementation
```cpp
#ifndef PI
#define PI 3.14159265358979323846f
#endif

inline float approx_sin(float x) {
    // Wrap x to [-PI, PI]
    while (x < -PI) x += 2.0f * PI;
    while (x >  PI) x -= 2.0f * PI;

    // Constants for the parabola
    const float B = 4.0f / PI;
    const float C = -4.0f / (PI * PI);

    // Parabolic approximation
    float y = B * x + C * x * fabsf(x);

    // Optional: Extra smoothing pass for audio quality
    // If the slight harmonic distortion is audible in the Side channel, uncomment this:
    // const float P = 0.225f;
    // y = P * (y * fabsf(y) - y) + y;
    
    return y;
}

inline float approx_cos(float x) {
    return approx_sin(x + (PI / 2.0f));
}
```
*   **Performance Profile:** Extremely fast. Relies strictly on FPU multiplications, addition, and one absolute value (`fabsf`).
*   **Memory Cost:** Zero bytes.
*   **Audio Quality:** The base parabola has slight amplitude error near the peaks. The optional smoothing pass brings the maximum amplitude error down to about 0.001, resulting in phase errors that are completely imperceptible in an ambient outdoor acoustic setting.

