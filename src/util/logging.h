#pragma once

#include <cstdint>
#include "../sensors/imu.h"
#include "log_names.h"

namespace logging {

// Longest log filename (log_names::NAME_BUF_LEN) and its full path under /logs.
// net/log_sync.cpp sizes its own buffers off these.
static constexpr size_t LOG_NAME_MAX = log_names::NAME_BUF_LEN;
static constexpr size_t LOG_PATH_MAX = 32;

// MID is appended as 3 rather than inserted between LOW and HIGH on purpose: the
// numeric value is persisted in NVS (`log_level`) and shipped in NavPacket's
// 2-bit FLAG_LOG_LEVEL field, and the two boards are flashed separately. Renumber
// HIGH and a saved setting — or a display board still running the previous build —
// silently means a different level. The diver-facing cycle order is still
// OFF -> LOW -> MID -> HIGH; see cycleLevel().
enum class LogLevel : uint8_t { LEVEL_OFF = 0, LEVEL_LOW, LEVEL_HIGH, LEVEL_MID };

// All data needed for a log entry. mag_cal/pitch_deg/roll_deg are written at MID
// as well as HIGH; the raw and accel/gyro fields are HIGH-only. Everything past
// the common set is ignored at LOW.
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
    // All levels: LIS3MDL die temperature, degrees C. NaN (logged as "nan")
    // when the read fails, rather than 0, which would read as a real value.
    float   mag_temp_c;
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

// The level the diver has selected. Changes the instant cycleLevel() is called,
// which is what the menu label and the NavPacket log-level flags report — the
// file it names may not be open yet (see LOG_COMMIT_DELAY_MS).
LogLevel getLevel();

// The level actually in effect: what the open file's schema is, or LEVEL_OFF
// while nothing is open. Equals getLevel() once a selection has settled.
LogLevel getActiveLevel();

// Cycle OFF -> LOW -> MID -> HIGH -> OFF. Selecting OFF closes the open file at once.
// Selecting LOW or HIGH only records the choice and (re)starts the settle timer;
// tick() opens the file once it has held still for LOG_COMMIT_DELAY_MS.
void cycleLevel();

// Set level directly (e.g. to restore from NVS on boot). Takes effect at once —
// a level restored from NVS was already committed on a previous power cycle.
// No-op if already at that level.
void setLevel(LogLevel level);

// Call from the main loop. Opens the file for a pending LOW/HIGH selection once
// it has been stable for LOG_COMMIT_DELAY_MS. (OFF never gets this far.)
void tick();

// Log one entry. No-op if level == OFF. Respects the per-level rate limit.
void log(const LogData& d);

// Log one entry immediately, bypassing the rate limit. Use for rare events
// (e.g., waypoint position corrections) that must not be silently dropped.
void logImmediate(const LogData& d);

// Full path of the file currently open for writing, or "" when none is.
// net/log_sync.cpp uses this to leave the in-progress log alone: uploading a
// partial file would land one upload now and a second, byte-different one
// when the dive ends, which the server can't dedupe and the assembler would
// see as two overlapping runs.
const char* currentPath();

// True if level != OFF.
bool isLogging();

// Path of the open log file, or "" if none is open (logging off, or still
// inside the LOG_COMMIT_DELAY_MS settle window).
const char* getLogPath();

}  // namespace logging
