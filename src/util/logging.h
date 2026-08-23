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
    float lat;            // current latitude (GPS or DR-estimated)
    float lon;            // current longitude (GPS or DR-estimated)
    char  pos_src;        // 'G' = GPS, 'W' = waypoint snap, 'E' = DR estimate, 'M' = diver mark
    uint8_t gps_satellites; // satellites tracked (0 if no GPS fix)
    float   gps_hdop;       // HDOP (0.0 if no GPS fix)
    float   depth_m;        // depth below surface, meters (0 if sensor absent)
    float   water_temp_c;   // water temperature, degrees C (0 if sensor absent)
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

// Set level directly (e.g. to restore from NVS on boot). No-op if already at that level.
void setLevel(LogLevel level);

// Log one entry. No-op if level == OFF. Respects the per-level rate limit.
void log(const LogData& d);

// Log one entry immediately, bypassing the rate limit. Use for rare events
// (e.g., waypoint position corrections) that must not be silently dropped.
void logImmediate(const LogData& d);

// True if level != OFF.
bool isLogging();

}  // namespace logging
