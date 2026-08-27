
#pragma once
#include <stdint.h>
#include "../types/types.h"
#include <Arduino.h>
#include <Wire.h>
#include "calib.h"  // For MagCalib struct
#include "../config.h"  // MAG_CAL_ROLL_SECTORS

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

// Read back the mag calibration currently in effect. Needed by the axis_test
// frame capture, whose fixture has to record the exact bias/soft-iron the
// captured counts were taken under -- a fixture that silently depends on
// whatever calibration happens to be installed later is not ground truth.
void getMagCalibration(MagCalib& out);

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
//   Gap-fill — GAP_FILL: a *second* baseline pass that patches the cells the
//     server flagged thin or empty. It gets the live grid baseline can't have,
//     because it runs only with a good baseline cal already installed: bins
//     are assigned algebraically per sample by util/mag_cal_orient.h (a
//     verbatim port of the server's own math -- no Mahony, no filter lag, no
//     pole singularity), so the highlighted cell is the same cell the website
//     drew. Read mag_cal_orient.h before touching this path; the axis
//     convention is the trap.
//
// Usage:
//   magBinCalBegin(BinCalMode::BASELINE);              // -> ROUGH_SCAN
//   magBinCalBegin(BinCalMode::MOUNTED);               // -> COLLECT
//   magBinCalBegin(BinCalMode::GAP_FILL, targets60);   // -> GAP_FILL (targets required)
//   // in main loop:
//   magBinCalTick(pitch_deg, heading_deg, rawMag, accelCal, gyroCal);  // add one sample
//   magBinCalFinishBaseline();           // baseline/gap-fill: diver declares "done", uploads for grading
//   if (magBinCalIsComplete()) { ... }   // mounted: all bins green; gap-fill: every target satisfied
//                                        // (or diver finished); baseline: finish requested
//   // retrieve:
//   magBinCalGetProgress(pkt);           // fill CalProgressPacket
//   magBinCalDumpCSV(file);              // write samples to open LittleFS File
//   magBinCalEnd();                      // clean up

enum class BinCalMode : uint8_t { BASELINE = 0, MOUNTED = 1, GAP_FILL = 2 };

// Start a collection session. Returns false (and starts nothing) if the mode's
// preconditions aren't met, so the caller can put a real message on screen
// instead of silently collecting garbage.
//
// `targets60` is required for GAP_FILL and ignored otherwise: 60 per-cell
// status codes (0=ok, 1=thin, 2=empty, 3=over) in elev-major order, exactly as
// GET /api/device/calibrations/targets returned them. GAP_FILL refuses to
// start without them -- a gap-fill session with no idea which cells are gaps
// is just an unguided baseline collection wearing a guided UI, which is worse
// than refusing, because the diver would trust it.
//
// `rollTargets60x4`/`hasRollTargets` are optional and GAP_FILL-only: the same
// statuses one axis finer (MAG_CAL_ROLL_SECTORS entries per cell), from
// cal_sync::loadRollTargets(). When present, a roll sector the *server*
// already considers ok/over is treated as satisfied from the start of the
// session -- the persistent widget doesn't make the diver re-collect roll
// coverage a prior accepted upload already has. Absent (nullptr or
// hasRollTargets=false) is a normal, supported case (older server, or a
// calibration predating roll coverage): the widget then falls back to
// session-local-only tracking, exactly as gap-fill behaved before roll
// existed.
bool   magBinCalBegin(BinCalMode mode, const uint8_t* targets60 = nullptr,
                       const uint8_t (*rollTargets60x4)[MAG_CAL_ROLL_SECTORS] = nullptr,
                       bool hasRollTargets = false);
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
// Gap-fill: true when every targeted cell has reached its cap, or the diver
// called magBinCalFinishBaseline() -- whichever comes first. Auto-completion
// matters here in a way it doesn't for baseline: the diver is working a
// finite, known list of cells, so "you're done" is a fact the device can
// actually establish rather than a guess.
bool   magBinCalIsComplete();
// Baseline and gap-fill: diver declares "good enough" and ends collection (no
// phase change -- neither mode ever switches phase). No-op (returns false) if
// too few samples have been collected yet, or if called for mounted cal.
bool   magBinCalFinishBaseline();
// Gap-fill only: how many targeted cells are still short of their cap (and how
// many there were to begin with). Both zero outside GAP_FILL. Drives the
// on-screen "3 of 11 cells left" line, which is the number the diver is
// actually working against.
void   magBinCalGapFillProgress(int& remainingOut, int& totalOut);
// Fill CalProgressPacket with current bin state
void   magBinCalGetProgress(struct CalProgressPacket& pkt);
// Write raw samples as CSV to an already-open File object
// Format: mx,my,mz,ax,ay,az,gx,gy,gz (mag = raw sensor counts, accel = g, gyro = rad/s; one sample per line)
void   magBinCalDumpCSV(void* filePtr);  // void* to avoid #include <LittleFS.h> here
void   magBinCalEnd();

}