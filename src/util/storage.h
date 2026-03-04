#pragma once

#include "../sensors/calib.h"
#include "../types/types.h"

// LittleFS / flash storage for calibration, config, and data logging
// 
// ESP32 Flash Layout:
//   - LittleFS is used for both persistent config/calibration data and runtime logging
//   - All files (logs, calibration JSON, config) stored in LittleFS root directory
//
// Future: Consider using NVS (Non-Volatile Storage) for calibration persistence
// and EEPROM emulation for fast config reads.

namespace storage {

// Save magnetometer calibration to JSON file on LittleFS
// Returns true if successful, false on error
bool saveMagCalibration(const char* filename, const MagCalib& cal);

// Load magnetometer calibration from JSON file on LittleFS
// Returns true if successful, false on error (file not found or parse error)
bool loadMagCalibration(const char* filename, MagCalib& cal);

// Save accelerometer/gyroscope calibration to JSON file
bool saveCalib3(const char* filename, const Calib3& cal);

// Load accelerometer/gyroscope calibration from JSON file
bool loadCalib3(const char* filename, Calib3& cal);

// Helper: Parse magnetometer calibration JSON (internal use)
static bool parseJsonMagCalib(const char* json, MagCalib& cal);

// Helper: Parse Calib3 JSON (internal use)
static bool parseJsonCalib3(const char* json, Calib3& cal);

}  // namespace storage
