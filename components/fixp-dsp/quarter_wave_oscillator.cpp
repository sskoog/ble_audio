#include <Arduino.h>
#include <math.h>

class QuarterWaveOscillator {
private:
    float sine_lut[256];

public:
    QuarterWaveOscillator() {
        // Pre-compute the first quadrant (0 to PI/2) during setup()
        // Adding 0.5 centers the phase bin to minimize interpolation error
        for (int i = 0; i < 256; i++) {
            float theta = (i + 0.5f) * (PI / 512.0f);
            sine_lut[i] = sinf(theta);
        }
    }

    // Fetches both Sine and Cosine concurrently using a 10-bit phase (0-1023)
    inline void get_trig(uint16_t phase, float &out_sin, float &out_cos) {
        out_sin = compute_wave(phase);
        out_cos = compute_wave(phase + 256); // 90-degree phase shift
    }

private:
    inline float compute_wave(uint16_t phase) {
        phase &= 1023;                 // Restrict to 10-bit boundary [0, 1023]
        uint8_t quadrant = phase >> 8; // Extract top 2 bits for quadrant [0, 3]
        uint8_t index = phase & 255;   // Extract lower 8 bits for array index [0, 255]

        switch (quadrant) {
            case 0: return sine_lut[index];                // Quadrant I
            case 1: return sine_lut[255 - index];          // Quadrant II (Time reversed)
            case 2: return -sine_lut[index];               // Quadrant III (Amplitude inverted)
            case 3: return -sine_lut[255 - index];         // Quadrant IV (Reversed & inverted)
            default: return 0.0f;
        }
    }
};