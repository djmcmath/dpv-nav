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

// Returns the last raw $PGTOP sentence received (empty string if none yet).
// Populated by update() before parse() so the raw bytes are always visible.
const char* getLastPgtopSentence();

// Returns the number of GPGSV sentences cached from the most recent 1 Hz group (0–4).
int getGsvLineCount();
// Returns the raw GPGSV sentence at index idx, or "" if out of range.
const char* getGsvLine(int idx);

// When enabled, every complete NMEA sentence is printed to Serial with [NMEA] prefix.
// Useful for confirming which sentences the module is actually emitting.
void setRawNmeaDebug(bool enable);

}
