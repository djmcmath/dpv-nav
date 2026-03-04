#pragma once

namespace flow {

struct FlowConfig {
  float k_factor;          // Pulses per liter (freq_hz = k_factor * flow_lpm)
  float cross_section_m2;  // Intake cross-section area for speed conversion (m²)
  float avg_period_s;      // EMA time constant for smoothing (seconds)
};

// Initialize interrupt-based pulse counting on the flow sensor pin
void init(const FlowConfig& cfg);

// Update flow config at runtime (for calibration)
void setConfig(const FlowConfig& cfg);

// Call periodically to recompute flow rate from accumulated pulses
void update();

// Current pulse frequency (Hz) — useful for debug
float getFrequency_hz();

// Current flow rate (L/min)
float getFlowRate_lpm();

// Convert flow rate to speed using configured cross-section area
float flowToSpeed_ms(float flow_lpm);

// Current speed (m/s) — convenience
float getSpeed_ms();

// Pulse count since last update — useful for debug
uint32_t getLastPulseCount();

}  // namespace flow
