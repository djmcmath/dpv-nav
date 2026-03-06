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

}
