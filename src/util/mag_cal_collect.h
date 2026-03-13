#pragma once

#include <cstdint>

// Magnetometer calibration data collection
// Collects raw magnetometer samples to CSV file for offline processing

namespace mag_cal {

// Start collecting magnetometer samples to CSV file
// User should rotate device through all orientations during collection
// Returns true if collection started successfully
bool startCollection(uint32_t duration_ms = 30000);

// Stop collection and close file
void stopCollection();

// Check if collection is active
bool isCollecting();

// Dump collected CSV file to serial terminal
// User can copy-paste output to save locally
void dumpToSerial();

// Delete calibration data file
void clearData();

// Log a single magnetometer sample (call from main loop when isCollecting() returns true)
// Returns true if sample was logged successfully
bool logSample();

// Progress queries (valid while isCollecting() returns true)
uint32_t getElapsedMs();
uint32_t getRemainingMs();
uint32_t getSampleCount();
uint32_t getDurationMs();

// Spatial coverage quality (0–100%).
// Returns min_axis_span / max_axis_span × 100: all three axes must grow
// equally for a high score, indicating spherical rotation.
// Returns 0 until max span exceeds 2000 LSB (prevents false 100% from
// early flat-plane rotation where all octants fill almost immediately).
uint8_t getSpatialCoverage();

}  // namespace mag_cal
