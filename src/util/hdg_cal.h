#pragma once

// Fourier-series heading correction.
//
// Coefficients are computed offline (tools/fourier_fit.py) from a set of
// (actual, indicated) heading pairs and stored in /hdg_fourier.json.
// The nav device loads coefficients at boot and applies the correction at runtime:
//
//   correction(θ) = c[0] + c[1]cos(θ) + c[2]sin(θ) + c[3]cos(2θ) + ...
//   heading_corrected = heading_indicated + correction(heading_indicated)
//
// Storage: /hdg_fourier.json on LittleFS (nav device only — written offline, uploaded)
// Sample data collected during CAL > Hdg cal is saved to /hdg_samples.csv for
// offline processing by fourier_fit.py.

namespace hdg_cal {

static constexpr int  MAX_HARMONICS = 4;
static constexpr int  MAX_COEFFS    = 2 * MAX_HARMONICS + 1;  // DC + N*(cos,sin)
static constexpr const char* FILE_PATH         = "/hdg_fourier.json";
static constexpr const char* SAMPLES_FILE_PATH = "/hdg_samples.csv";

struct HdgCal {
    int   n;                 // number of harmonics (1..MAX_HARMONICS)
    float c[MAX_COEFFS];     // [DC, cos1, sin1, cos2, sin2, ...]
};

// Load Fourier coefficients from /hdg_fourier.json.  Returns false if absent/invalid.
bool load(HdgCal& cal);

// Apply Fourier correction to a raw indicated heading.
// headingDeg is pre-correction (0-360); returns corrected heading (0-360).
// Returns headingDeg unchanged if n is out of range.
float apply(float headingDeg, const HdgCal& cal);

}  // namespace hdg_cal
