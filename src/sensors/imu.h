
#pragma once
#include <stdint.h>
#include "../types/types.h"
#include <Arduino.h>
#include <Wire.h>
#include "calib.h"  // For MagCalib struct

// Forward declaration — CalProgressPacket defined in dpvlink.h
struct CalProgressPacket;

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

// --- Calibration routines (blocking) ---
// Each writes result to `out` AND updates the internal calibration state,
// so subsequent reads are immediately calibrated.
void calibrateMagnetometer(MagCalib& out, uint32_t duration_ms = 30000);
void calibrateGyroscope(Calib3& out, uint32_t duration_ms = 10000);
void calibrateAccelerometer(Calib3& out, uint32_t sample_duration_ms = 1500);

// --- Non-blocking magnetometer calibration (hard-iron sweep, legacy) ---
// Use these from a main loop so other work (NavPacket sends, display) keeps running.
// Call magCalNBBegin() once, then magCalNBTick() each loop iteration.
// magCalNBTick() returns true when calibration is complete; call magCalNBGetResult() then.
bool   magCalNBBegin(uint32_t duration_ms);
bool   magCalNBTick();   // returns true when done
bool   magCalNBIsActive();
void   magCalNBGetResult(MagCalib& out);
// Progress: elapsed_ms, remaining_ms, coverage 0-100 per axis (min of 3 = overall)
void   magCalNBGetProgress(uint32_t& elapsed_ms, uint32_t& remaining_ms,
                           int& covX, int& covY, int& covZ);

// --- Bin-aware magnetometer calibration collection ---
// Used for both Baseline cal (60 bins, full sphere) and Mounted cal (36 bins, limited range).
// Samples are collected from raw sensor + AHRS pitch/heading to bin them by orientation.
// Once a bin is green (>= MAG_CAL_BIN_GREEN_THRESHOLD samples), new samples are
// rejected for that bin to avoid skewing the fit with duplicate data.
// Caller provides pitch_deg (from AHRS accel) and heading_deg (from AHRS yaw).
//
// Usage:
//   magBinCalBegin(isMounted);           // start collection
//   // in main loop:
//   magBinCalTick(pitch_deg, heading_deg, rawMag);  // add one sample
//   if (magBinCalIsComplete()) { ... }   // all bins green
//   // retrieve:
//   magBinCalGetProgress(pkt);           // fill CalProgressPacket
//   magBinCalDumpCSV(file);              // write samples to open LittleFS File
//   magBinCalEnd();                      // clean up

void   magBinCalBegin(bool isMounted);
// Add one sample; pitch_deg from AHRS, heading_deg from AHRS, rawMag in logical frame (post-axis-map).
// Must be the output of readMagRaw() — NOT readMagRaw_SensorFrame().  The calibration JSON is
// applied in logical frame at runtime, so samples stored here must also be in logical frame.
// Returns true if sample was accepted (bin not yet green)
bool   magBinCalTick(float pitch_deg, float heading_deg, const Vec3i16& rawMagLogical);
bool   magBinCalIsActive();
bool   magBinCalIsComplete();   // true when all bins green
// Fill CalProgressPacket with current bin state
void   magBinCalGetProgress(struct CalProgressPacket& pkt);
// Write raw samples as CSV to an already-open File object
// Format: mx,my,mz (raw sensor counts, one per line)
void   magBinCalDumpCSV(void* filePtr);  // void* to avoid #include <LittleFS.h> here
void   magBinCalEnd();

}