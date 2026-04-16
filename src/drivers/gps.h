#pragma once

#include "../types/types.h"

namespace gps {

// Initialize GPS on Serial2. Returns true if serial port opened successfully.
// Does NOT block waiting for a fix — the GPS streams data continuously.
bool init();

// Enable or disable GPS processing. When disabled, update() is a no-op
// and getFix() returns an empty fix. Does not power off the GPS module.
void setEnabled(bool enable);

// Call every loop iteration. Reads available NMEA bytes (non-blocking).
// Returns true when a complete sentence has been parsed.
bool update();

// Returns the latest parsed fix data.
GpsFix getFix();

// Convenience: true if last parsed sentence had a valid fix.
bool hasFix();

// Compute a 0–4 signal quality bar count from a GpsFix.
// Uses HDOP, fix type (2D/3D), and satellite count.
// Scoring thresholds and weights are configured in config.h (GPS_SCORE_*/GPS_WEIGHT_*).
uint8_t computeSignalBars(const GpsFix& fix);

}
