#pragma once

#include "../types/types.h"

namespace gps {

// Initialize GPS on Serial2. Returns true if serial port opened successfully.
// Does NOT block waiting for a fix — the GPS streams data continuously.
bool init();

// Call every loop iteration. Reads available NMEA bytes (non-blocking).
// Returns true when a complete sentence has been parsed.
bool update();

// Returns the latest parsed fix data.
GpsFix getFix();

// Convenience: true if last parsed sentence had a valid fix.
bool hasFix();

}
