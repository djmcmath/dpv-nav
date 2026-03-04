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

}  // namespace mag_cal
