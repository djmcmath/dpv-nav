#pragma once

#include <cstdint>
#include "../sensors/imu.h"

namespace logging {

enum class LogLevel : uint8_t { LEVEL_OFF = 0, LEVEL_LOW, LEVEL_HIGH };

// All data needed for a log entry. HIGH-only fields are ignored when level == LOW.
struct LogData {
    uint32_t timestamp_ms;
    float heading_deg;
    float speed_ms;
    bool  gpsSpeed;       // true = GPS, false = flowmeter
    float pos_x_m;
    float pos_y_m;
    bool  gpsPos;         // true = GPS position, false = DR estimate
    // HIGH-level fields
    imu::Vec3f mag_raw, accel_raw, gyro_raw;
    imu::Vec3f mag_cal, accel_cal, gyro_cal;
    float pitch_deg, roll_deg;
};

// Initialize logging system: mount LittleFS (if not already mounted),
// clean up oldest logs if free space is below threshold, determine next
// sequential file number. Starts in OFF state — no file is opened until
// cycleLevel() moves away from OFF.
bool init();

// Shutdown logging system (close file if open).
void shutdown();

// Current log level.
LogLevel getLevel();

// Cycle OFF -> LOW -> HIGH -> OFF. Opens/closes files as needed.
void cycleLevel();

// Log one entry. No-op if level == OFF.
void log(const LogData& d);

// True if level != OFF.
bool isLogging();

}  // namespace logging
