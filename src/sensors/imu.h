
#pragma once
#include <stdint.h>
#include "../types/types.h"
#include <Arduino.h>
#include <Wire.h>
#include "calib.h"  // For MagCalib struct

namespace imu {

//struct Vec3i16 { int16_t x, y, z; };
//struct Vec3f   { float   x, y, z; };

enum class ImuStatus : uint8_t {
  Ok = 0,
  NotInitialized,
  BusError,
  WhoAmIMismatch,
  DataNotReady,
  Error,
};



bool init(const ImuConfig& cfg, const AxisMap& accelGyroMap, const AxisMap& magMap);

ImuStatus initAccelGyro(TwoWire& wire =  Wire);
ImuStatus initMag(TwoWire& wire = Wire);

ImuStatus readAccelRaw(Vec3i16& out);
ImuStatus readGyroRaw(Vec3i16& out);
ImuStatus readMagRaw(Vec3i16& out);

// Read sensors in their native/sensor frame (no axis mapping applied)
// Use these for diagnostics to determine correct axis mapping
ImuStatus readAccelRaw_SensorFrame(Vec3i16& out);
ImuStatus readGyroRaw_SensorFrame(Vec3i16& out);
ImuStatus readMagRaw_SensorFrame(Vec3i16& out);

// Converted units (calibrated only):
ImuStatus readAccel_g(Vec3f& out);        // g
ImuStatus readGyro_rad_s(Vec3f& out);     // rad/s
ImuStatus readMag_uT(Vec3f& out);         // µT (or "sensor units" if you don't have scale yet)

// Converted units with both raw and calibrated outputs:
ImuStatus readAccel_g_raw_cal(Vec3f& rawOut, Vec3f& calOut);  // g (raw = uncalibrated, cal = with bias/scale applied)
ImuStatus readGyro_rad_s_raw_cal(Vec3f& rawOut, Vec3f& calOut);  // rad/s (raw = uncalibrated, cal = with bias applied)
ImuStatus readMag_raw_cal(Vec3f& rawOut, Vec3f& calOut);  // calibrated mag with environmental + hard/soft-iron correction

// Calibrated magnetometer read:
ImuStatus readMag(Vec3f& out);            // Mag with hard/soft-iron calibration applied

// --- Calibration setters ---
// Set the calibration data used by all read functions.
// Call after loading from LittleFS or after running a calibrate function.
void setAccelCalibration(const Calib3& cal);
void setGyroCalibration(const Calib3& cal);
void setMagCalibration(const MagCalib& cal);

// --- Calibration routines ---
// Each writes result to `out` AND updates the internal calibration state,
// so subsequent reads are immediately calibrated.
void calibrateMagnetometer(MagCalib& out, uint32_t duration_ms = 30000);
void calibrateGyroscope(Calib3& out, uint32_t duration_ms = 10000);
void calibrateAccelerometer(Calib3& out, uint32_t sample_duration_ms = 1500);

}