#pragma once

#include <cstdint>
#include <cstddef>
#include "../types/types.h"
#include "../sensors/imu.h"

namespace logging {

// Log entry contains both raw and calibrated data
struct LogEntry {
  uint32_t timestamp_ms;    // milliseconds since start
  
  // Raw sensor readings
  imu::Vec3f mag_raw;
  imu::Vec3f accel_raw;
  imu::Vec3f gyro_raw;
  
  // Calibrated sensor readings
  imu::Vec3f mag_cal;
  imu::Vec3f accel_cal;
  imu::Vec3f gyro_cal;
  
  // Processed data
  float heading_deg;        // heading in degrees
  float roll_deg;           // roll angle
  float pitch_deg;          // pitch angle
};

// Initialize logging system (mounts SPIFFS, creates log file)
// Returns true if successful
bool init();

// Shutdown logging system (closes file, unmounts SPIFFS)
void shutdown();

// Log a data entry to file
// Returns true if successful, false if error or not initialized
bool logEntry(const LogEntry& entry);

// Get current log file path
const char* getLogPath();

// Get number of bytes written to current log
uint32_t getBytesLogged();

// Check if logging is active
bool isLogging();

// Close current log and start a new one
// Returns true if successful
bool rotateLog();

}  // namespace logging
