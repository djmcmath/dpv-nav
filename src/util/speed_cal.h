#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Speed calibration history — stores up to 6 k-factor measurements on
// LittleFS (/speed_cal.json).  The active k-factor is the average of all
// stored measurements.  New measurements append; when full the oldest is
// dropped.  The nav device calls these functions; the display device never
// uses this module.
// ---------------------------------------------------------------------------

namespace speed_cal {

constexpr int HISTORY_SIZE = 6;

struct History {
    float   k_values[HISTORY_SIZE];
    uint8_t count;  // number of valid entries (0..HISTORY_SIZE)
};

// Load history from LittleFS.  Returns empty history if file not found.
History load();

// Save history to LittleFS.
void save(const History& h);

// Add a new k-factor measurement.  Appends while count < HISTORY_SIZE;
// when full, shifts oldest out and appends the new value at the end.
void addMeasurement(History& h, float k_value);

// Return the average k-factor of all stored measurements.
// Returns default_k when history is empty.
float averageK(const History& h, float default_k);

// Clear all stored measurements (does not write to flash; caller must save()).
void reset(History& h);

}  // namespace speed_cal
