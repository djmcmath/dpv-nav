
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

// --- Accelerometer orientation classification ---
// Human-readable labels for the 6 accelerometer calibration orientations, in
// logical (post-axis-map) NED frame. Index matches classifyAccelOrientation().
extern const char* const kAccelOrientationNames[6];

// Classifies which of the 6 axis-up orientations the device is currently
// held in, from raw logical-frame accel counts (readAccelRaw() output — NOT
// g-converted, since this must work before any accel calibration exists).
// Returns -1 if no single axis reads clearly dominant (device tilted between
// orientations, upside-down at an angle, or moving).
// Used both by calibrateAccelerometer() (to live-verify each step) and by
// the `accel_orient` serial command (to let a diver check orientation
// labels against the physical device before running a real calibration).
int classifyAccelOrientation(const Vec3i16& accelRawLogical);

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
// Baseline and mounted cal now run in permanently different phases (see
// divemap/docs/architecture/baseline-cal-coverage-feedback-plan.md; the
// two-pass ROUGH_SCAN->COLLECT handoff docs/baseline-cal-two-pass.md
// describes is retired):
//   Baseline — ROUGH_SCAN only, forever. No grid: AHRS heading is
//     untrustworthy pre-calibration (chicken-and-egg) and any live
//     (elevation,azimuth) grid has an unavoidable pole singularity anyway
//     (see baseline-cal-two-pass.md's diagnostic history for why this was
//     tried and abandoned). Feedback stays honest: per-axis raw-range bars,
//     sample count, live fit stats. The diver declares "done" themselves
//     (magBinCalFinishBaseline()) — there's no auto-detected completion —
//     which uploads the 9-axis CSV for real server-side grading and
//     coverage-gap feedback.
//   Mounted — COLLECT only, unchanged: the familiar grid, auto-completes
//     when all bins are green. Its narrower ±30° range mostly sidesteps the
//     pole problem that forced baseline off this design.
//
// Usage:
//   magBinCalBegin(isMounted);           // start collection (baseline -> ROUGH_SCAN; mounted -> COLLECT)
//   // in main loop:
//   magBinCalTick(pitch_deg, heading_deg, rawMag, accelCal, gyroCal);  // add one sample
//   magBinCalFinishBaseline();           // baseline only: diver declares "done", uploads for grading
//   if (magBinCalIsComplete()) { ... }   // mounted: all bins green; baseline: finish requested
//   // retrieve:
//   magBinCalGetProgress(pkt);           // fill CalProgressPacket
//   magBinCalDumpCSV(file);              // write samples to open LittleFS File
//   magBinCalEnd();                      // clean up

void   magBinCalBegin(bool isMounted);
// Add one sample; pitch_deg from AHRS, heading_deg from AHRS, rawMag in logical frame (post-axis-map).
// rawMagLogical must be the output of readMagRaw() — NOT readMagRaw_SensorFrame().  The calibration
// JSON is applied in logical frame at runtime, so samples stored here must also be in logical frame.
// accelCal/gyroCal are the calibrated readAccel_g_raw_cal()/readGyro_rad_s_raw_cal() outputs (g,
// rad/s; same logical frame as rawMagLogical) — logged (not used for binning) so the server can
// reconstruct a trustworthy per-sample orientation after a real mag fit exists. See
// divemap/docs/architecture/baseline-cal-coverage-feedback-plan.md.
// Returns true if sample was accepted (bin not yet green in COLLECT; always accepted in ROUGH_SCAN
// subject to the sample cap and exact-duplicate rejection)
bool   magBinCalTick(float pitch_deg, float heading_deg, const Vec3i16& rawMagLogical,
                      const Vec3f& accelCal, const Vec3f& gyroCal);
bool   magBinCalIsActive();
// Mounted: true when all bins green. Baseline: true once the diver has called
// magBinCalFinishBaseline() (no bin/grid concept applies to baseline anymore).
bool   magBinCalIsComplete();
// Baseline only: diver declares "good enough" and ends collection (no phase change --
// baseline never leaves ROUGH_SCAN). No-op (returns false) if too few samples have been
// collected yet, or if called for mounted cal.
bool   magBinCalFinishBaseline();
// Fill CalProgressPacket with current bin state
void   magBinCalGetProgress(struct CalProgressPacket& pkt);
// Write raw samples as CSV to an already-open File object
// Format: mx,my,mz,ax,ay,az,gx,gy,gz (mag = raw sensor counts, accel = g, gyro = rad/s; one sample per line)
void   magBinCalDumpCSV(void* filePtr);  // void* to avoid #include <LittleFS.h> here
void   magBinCalEnd();

}