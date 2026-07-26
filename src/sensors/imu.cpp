#include <Wire.h>
#include "../board_pins.h"
#include "../drivers/lis3mdl.h"
#include "../types/types.h"
#include <Arduino.h>
#include "imu.h"
#include <math.h>
#include "../config.h"
#include <LittleFS.h>
#include <dpvlink.h>
//#include "i2c.h"

// No more extern globals — each sensor keeps its own internal calibration state,
// set via setXxxCalibration() for a consistent pattern across all three sensors.

namespace imu {


// ---------- I2C addresses (7-bit) ----------
static constexpr uint8_t LSM6DS3_ADDR_SA0_0 = 0x6A; // SA0 low (derived from D4h/D5h patterns) :contentReference[oaicite:3]{index=3}
static constexpr uint8_t LSM6DS3_ADDR_SA0_1 = 0x6B; // SA0 high (common alt; use if 0x6A fails)
//static constexpr uint8_t LIS3MDL_ADDR_DEFAULT = 0x1C; // :contentReference[oaicite:4]{index=4} //May be 19 or 1E instead.
static constexpr uint8_t LIS3MDL_ADDR_DEFAULT = 0x19; // :contentReference[oaicite:4]{index=4}

// ----------- Internal state -----------
static bool gyro_inited = false;
static bool accel_inited = false;
static bool mag_inited = false;
static ImuConfig g_cfg{};
static AxisMap g_accelGyroMap{};  // Axis mapping for LSM6DS3 (accel/gyro)
static AxisMap g_magMap{};        // Axis mapping for LIS3MDL (magnetometer)

static float g_accel_lsb_per_g = 0.0f;     // counts per g
static float g_gyro_lsb_per_dps = 0.0f;    // counts per deg/s
static float g_mag_lsb_per_uT = 0.0f;      // counts per µT (if known)

static TwoWire* gWire = nullptr;
static uint8_t  gLsmAddr = LSM6DS3_ADDR_SA0_0;

// Internal calibration data — set via setXxxCalibration(), used by read functions
static Calib3 g_gyroCalibration = {{0, 0, 0}, {1, 1, 1}};
static Calib3 g_accelCalibration = {{0, 0, 0}, {1, 1, 1}};
static MagCalib g_magCalibration = {
  {0, 0, 0},  // no offset by default
  {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}  // identity soft-iron matrix
};

// ---------- LSM6DS3 registers ----------
static constexpr uint8_t LSM6DS3_WHO_AM_I   = 0x0F; // returns 0x69 :contentReference[oaicite:5]{index=5}
static constexpr uint8_t LSM6DS3_CTRL1_XL   = 0x10;
static constexpr uint8_t LSM6DS3_CTRL2_G    = 0x11;
static constexpr uint8_t LSM6DS3_CTRL3_C    = 0x12;
static constexpr uint8_t LSM6DS3_OUTX_L_G   = 0x22;
static constexpr uint8_t LSM6DS3_OUTX_L_XL  = 0x28;

// CTRL3_C bits (per datasheet naming; common across ST IMUs)
static constexpr uint8_t CTRL3_SW_RESET = 0x01;
static constexpr uint8_t CTRL3_IF_INC   = 0x04; // auto-increment register address
static constexpr uint8_t CTRL3_BDU      = 0x40; // block data update (coherent reads)

// ---------- Low-level helpers ----------
static bool write8(uint8_t addr, uint8_t reg, uint8_t val) {
  gWire->beginTransmission(addr);
  gWire->write(reg);
  gWire->write(val);
  return (gWire->endTransmission() == 0);
}

static bool read8(uint8_t addr, uint8_t reg, uint8_t& val) {
  gWire->beginTransmission(addr);
  gWire->write(reg);
  if (gWire->endTransmission(false) != 0) return false; // repeated start
  if (gWire->requestFrom(addr, (uint8_t)1) != 1) return false;
  val = gWire->read();
  return true;
}

static bool readN(uint8_t addr, uint8_t startReg, uint8_t* buf, size_t n) {
  gWire->beginTransmission(addr);
  gWire->write(startReg);
  if (gWire->endTransmission(false) != 0) return false; // repeated start
  size_t got = gWire->requestFrom(addr, (uint8_t)n);
  if (got != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = gWire->read();
  return true;
}

// Try 0x6A then 0x6B; pick the one that answers WHO_AM_I correctly.
static bool detectLsm6ds3Address() {
  uint8_t who = 0;
  gLsmAddr = LSM6DS3_ADDR_SA0_0;
  if (read8(gLsmAddr, LSM6DS3_WHO_AM_I, who) && who == 0x69) {
    Serial.print("WHO_AM_I match at 0x6A: 0x");
    Serial.println(who, HEX);
    return true;
  }
  Serial.print("0x6A WHO_AM_I: 0x");
  Serial.println(who, HEX);

  gLsmAddr = LSM6DS3_ADDR_SA0_1;
  if (read8(gLsmAddr, LSM6DS3_WHO_AM_I, who) && who == 0x69) {
    Serial.print("WHO_AM_I match at 0x6B: 0x");
    Serial.println(who, HEX);
    return true;
  }
  Serial.print("0x6B WHO_AM_I: 0x");
  Serial.println(who, HEX);

  return false;
}

// ----------- Helpers -----------
static inline float dpsToRad(float dps) { return dps * 0.017453292519943295f; }

static Vec3i16 applyAxisMap(const Vec3i16& v, const AxisMap& map) {
  auto pick = [&](int8_t code) -> int16_t {
    int sign = (code < 0) ? -1 : 1;
    int idx  = (code < 0) ? -code : code;
    int16_t val = 0;
    switch (idx) {
      case 1: val = v.x; break;
      case 2: val = v.y; break;
      case 3: val = v.z; break;
      default: val = 0;  break;
    }
    return (int16_t)(sign * val);
  };

  return { pick(map.x_axis), pick(map.y_axis), pick(map.z_axis) };
}

static Vec3f toFloat(const Vec3i16& v) { return {(float)v.x, (float)v.y, (float)v.z}; }

// ----------- Magnetometer Calibration -----------
// Performs a min/max sweep calibration over a specified duration.
// User must rotate device through all orientations during this time.
// Computes bias as (min + max) / 2 for each axis.
// Sets softIron to identity (no soft iron correction).
void calibrateMagnetometer(MagCalib& out, uint32_t duration_ms) {
  if (!mag_inited) {
    Serial.println("[CAL] Error: Magnetometer not initialized");
    out.bias = {0, 0, 0};
    // Identity matrix
    out.softIron[0][0] = 1; out.softIron[0][1] = 0; out.softIron[0][2] = 0;
    out.softIron[1][0] = 0; out.softIron[1][1] = 1; out.softIron[1][2] = 0;
    out.softIron[2][0] = 0; out.softIron[2][1] = 0; out.softIron[2][2] = 1;
    return;
  }

  Serial.println("[CAL] ========================================");
  Serial.println("[CAL] MAGNETOMETER CALIBRATION");
  Serial.println("[CAL] ========================================");
  Serial.print("[CAL] Duration: ");
  Serial.print(duration_ms / 1000);
  Serial.println(" seconds");
  Serial.println("[CAL] ");
  Serial.println("[CAL] Rotate device SLOWLY through all orientations:");
  Serial.println("[CAL]   - Tumble (rotate around all axes)");
  Serial.println("[CAL]   - Figure-8 patterns");
  Serial.println("[CAL]   - Hold each orientation for 1-2 seconds");
  Serial.println("[CAL] ");
  Serial.println("[CAL] Goal: X, Y, Z coverage all reach 100%");
  Serial.println("[CAL] ----------------------------------------");

  // Initialize min/max with first reading
  Vec3i16 firstRead;
  readMagRaw_SensorFrame(firstRead);  // Use sensor frame for calibration

  int16_t minX = firstRead.x, maxX = firstRead.x;
  int16_t minY = firstRead.y, maxY = firstRead.y;
  int16_t minZ = firstRead.z, maxZ = firstRead.z;

  uint32_t startTime = millis();
  uint32_t sampleCount = 0;
  uint32_t lastProgressUpdate = 0;

  // Expected full-rotation range (approximate, for coverage estimation)
  // For a typical magnetometer in Earth's field (~50 µT), rotating 360° gives:
  // Range ≈ 2 * field_strength * LSB/µT ≈ 2 * 50 * 68.42 ≈ 6800 counts
  const float EXPECTED_RANGE = 6800.0f;

  // Sweep for specified duration
  while (millis() - startTime < duration_ms) {
    Vec3i16 reading;
    if (readMagRaw_SensorFrame(reading) == ImuStatus::Ok) {  // Use sensor frame
      minX = min(minX, reading.x);
      maxX = max(maxX, reading.x);
      minY = min(minY, reading.y);
      maxY = max(maxY, reading.y);
      minZ = min(minZ, reading.z);
      maxZ = max(maxZ, reading.z);
      sampleCount++;

      // Update progress every second
      uint32_t elapsed = millis() - startTime;
      if (elapsed - lastProgressUpdate >= 1000) {
        lastProgressUpdate = elapsed;
        uint32_t remaining = (duration_ms - elapsed) / 1000;

        // Calculate coverage percentages
        float rangeX = maxX - minX;
        float rangeY = maxY - minY;
        float rangeZ = maxZ - minZ;

        int coverageX = min(100, (int)((rangeX / EXPECTED_RANGE) * 100.0f));
        int coverageY = min(100, (int)((rangeY / EXPECTED_RANGE) * 100.0f));
        int coverageZ = min(100, (int)((rangeZ / EXPECTED_RANGE) * 100.0f));

        // Print progress bar style output
        Serial.print("[CAL] ");
        Serial.print(remaining);
        Serial.print("s | Samples: ");
        Serial.print(sampleCount);
        Serial.print(" | Coverage X:");
        Serial.print(coverageX);
        Serial.print("% Y:");
        Serial.print(coverageY);
        Serial.print("% Z:");
        Serial.print(coverageZ);
        Serial.print("%");

        // Add quality indicator
        int avgCoverage = (coverageX + coverageY + coverageZ) / 3;
        if (avgCoverage >= 80) {
          Serial.println(" [EXCELLENT]");
        } else if (avgCoverage >= 60) {
          Serial.println(" [GOOD]");
        } else if (avgCoverage >= 40) {
          Serial.println(" [OK - keep rotating]");
        } else {
          Serial.println(" [POOR - rotate more!]");
        }
      }
    }
    delay(10);  // ~100 Hz sampling
  }

  // Calculate bias as midpoint of min/max range (in sensor frame)
  float biasX = ((float)(minX + maxX)) / 2.0f;
  float biasY = ((float)(minY + maxY)) / 2.0f;
  float biasZ = ((float)(minZ + maxZ)) / 2.0f;

  // Apply axis mapping to convert bias from sensor frame to logical frame
  // This is critical because readMag() applies axis mapping BEFORE calibration,
  // so the bias must be in the same (logical) frame as the readings it corrects.
  Vec3i16 biasSensorInt = {(int16_t)biasX, (int16_t)biasY, (int16_t)biasZ};
  Vec3i16 biasLogical = applyAxisMap(biasSensorInt, g_magMap);

  out.bias = {(float)biasLogical.x, (float)biasLogical.y, (float)biasLogical.z};
  
  // Identity matrix for soft iron (no correction)
  out.softIron[0][0] = 1; out.softIron[0][1] = 0; out.softIron[0][2] = 0;
  out.softIron[1][0] = 0; out.softIron[1][1] = 1; out.softIron[1][2] = 0;
  out.softIron[2][0] = 0; out.softIron[2][1] = 0; out.softIron[2][2] = 1;

  g_magCalibration = out;  // Update internal calibration used by readMag()/readMag_raw_cal()

  // Calculate final coverage statistics
  float rangeX = maxX - minX;
  float rangeY = maxY - minY;
  float rangeZ = maxZ - minZ;
  
  int coverageX = min(100, (int)((rangeX / EXPECTED_RANGE) * 100.0f));
  int coverageY = min(100, (int)((rangeY / EXPECTED_RANGE) * 100.0f));
  int coverageZ = min(100, (int)((rangeZ / EXPECTED_RANGE) * 100.0f));
  int avgCoverage = (coverageX + coverageY + coverageZ) / 3;

  Serial.println("[CAL] ========================================");
  Serial.println("[CAL] CALIBRATION COMPLETE!");
  Serial.println("[CAL] ========================================");
  Serial.print("[CAL] Samples collected: ");
  Serial.println(sampleCount);
  Serial.print("[CAL] Bias (sensor frame): X=");
  Serial.print(biasX, 1);
  Serial.print(" Y=");
  Serial.print(biasY, 1);
  Serial.print(" Z=");
  Serial.println(biasZ, 1);
  Serial.print("[CAL] Bias (logical frame): X=");
  Serial.print(out.bias.x, 1);
  Serial.print(" Y=");
  Serial.print(out.bias.y, 1);
  Serial.print(" Z=");
  Serial.println(out.bias.z, 1);

  Serial.println("[CAL] ");
  Serial.print("[CAL] Final Coverage: X=");
  Serial.print(coverageX);
  Serial.print("% Y=");
  Serial.print(coverageY);
  Serial.print("% Z=");
  Serial.print(coverageZ);
  Serial.print("% (Avg: ");
  Serial.print(avgCoverage);
  Serial.println("%)");

  if (avgCoverage >= 80) {
    Serial.println("[CAL] ✓ EXCELLENT coverage - calibration should be accurate");
  } else if (avgCoverage >= 60) {
    Serial.println("[CAL] ✓ GOOD coverage - calibration acceptable");
  } else if (avgCoverage >= 40) {
    Serial.println("[CAL] ⚠ MODERATE coverage - may need recalibration");
  } else {
    Serial.println("[CAL] ✗ POOR coverage - recalibration recommended!");
  }
  Serial.println("[CAL] ========================================");
  Serial.print("[CAL] Field range (sensor frame) - X:[");
  Serial.print(minX); Serial.print(", ");
  Serial.print(maxX); Serial.print("] Y:[");
  Serial.print(minY); Serial.print(", ");
  Serial.print(maxY); Serial.print("] Z:[");
  Serial.print(minZ); Serial.print(", ");
  Serial.print(maxZ); Serial.println("]");
  Serial.println("[CAL] (Bias converted to logical frame for use with axis mapping)");
}

// ----------- Non-blocking Magnetometer Calibration -----------
// Same min/max sweep as calibrateMagnetometer(), but runs across multiple loop() calls.
// Call magCalNBBegin() once, then magCalNBTick() each loop iteration.
static bool     g_nbCalActive   = false;
static uint32_t g_nbCalStart    = 0;
static uint32_t g_nbCalDuration = 0;
static int16_t  g_nbMinX, g_nbMaxX, g_nbMinY, g_nbMaxY, g_nbMinZ, g_nbMaxZ;
static uint32_t g_nbSamples     = 0;
static MagCalib g_nbResult;

static constexpr float NB_EXPECTED_RANGE = 6800.0f;

bool magCalNBBegin(uint32_t duration_ms) {
  if (!mag_inited) {
    Serial.println("[CAL-NB] Error: mag not initialized");
    return false;
  }
  if (g_nbCalActive) {
    Serial.println("[CAL-NB] Already running — stopping previous");
    g_nbCalActive = false;
  }
  // Seed min/max with first reading
  Vec3i16 first;
  if (readMagRaw_SensorFrame(first) != ImuStatus::Ok) {
    Serial.println("[CAL-NB] Error: initial read failed");
    return false;
  }
  g_nbMinX = g_nbMaxX = first.x;
  g_nbMinY = g_nbMaxY = first.y;
  g_nbMinZ = g_nbMaxZ = first.z;
  g_nbSamples    = 0;
  g_nbCalStart   = millis();
  g_nbCalDuration = duration_ms;
  g_nbCalActive  = true;

  Serial.print("[CAL-NB] Hard-iron sweep started, duration=");
  Serial.print(duration_ms / 1000);
  Serial.println("s — rotate device through all orientations");
  return true;
}

bool magCalNBTick() {
  if (!g_nbCalActive) return false;

  uint32_t elapsed = millis() - g_nbCalStart;
  if (elapsed >= g_nbCalDuration) {
    // Time's up — compute result
    float biasX = ((float)(g_nbMinX + g_nbMaxX)) / 2.0f;
    float biasY = ((float)(g_nbMinY + g_nbMaxY)) / 2.0f;
    float biasZ = ((float)(g_nbMinZ + g_nbMaxZ)) / 2.0f;

    Vec3i16 biasSensorInt = {(int16_t)biasX, (int16_t)biasY, (int16_t)biasZ};
    Vec3i16 biasLogical   = applyAxisMap(biasSensorInt, g_magMap);
    g_nbResult.bias = {(float)biasLogical.x, (float)biasLogical.y, (float)biasLogical.z};
    g_nbResult.softIron[0][0] = 1; g_nbResult.softIron[0][1] = 0; g_nbResult.softIron[0][2] = 0;
    g_nbResult.softIron[1][0] = 0; g_nbResult.softIron[1][1] = 1; g_nbResult.softIron[1][2] = 0;
    g_nbResult.softIron[2][0] = 0; g_nbResult.softIron[2][1] = 0; g_nbResult.softIron[2][2] = 1;
    g_magCalibration = g_nbResult;
    g_nbCalActive = false;

    Serial.printf("[CAL-NB] Done. Samples=%lu bias=(%+.1f,%+.1f,%+.1f) logical\n",
                  (unsigned long)g_nbSamples, g_nbResult.bias.x, g_nbResult.bias.y, g_nbResult.bias.z);
    return true;
  }

  // Read and expand min/max
  Vec3i16 r;
  if (readMagRaw_SensorFrame(r) == ImuStatus::Ok) {
    if (r.x < g_nbMinX) g_nbMinX = r.x; if (r.x > g_nbMaxX) g_nbMaxX = r.x;
    if (r.y < g_nbMinY) g_nbMinY = r.y; if (r.y > g_nbMaxY) g_nbMaxY = r.y;
    if (r.z < g_nbMinZ) g_nbMinZ = r.z; if (r.z > g_nbMaxZ) g_nbMaxZ = r.z;
    g_nbSamples++;
  }
  return false;
}

bool magCalNBIsActive() { return g_nbCalActive; }

void magCalNBGetResult(MagCalib& out) { out = g_nbResult; }

void magCalNBGetProgress(uint32_t& elapsed_ms, uint32_t& remaining_ms,
                         int& covX, int& covY, int& covZ) {
  if (!g_nbCalActive) {
    elapsed_ms = remaining_ms = 0;
    covX = covY = covZ = 0;
    return;
  }
  elapsed_ms   = millis() - g_nbCalStart;
  remaining_ms = (elapsed_ms < g_nbCalDuration) ? (g_nbCalDuration - elapsed_ms) : 0;
  covX = min(100, (int)(((float)(g_nbMaxX - g_nbMinX) / NB_EXPECTED_RANGE) * 100.0f));
  covY = min(100, (int)(((float)(g_nbMaxY - g_nbMinY) / NB_EXPECTED_RANGE) * 100.0f));
  covZ = min(100, (int)(((float)(g_nbMaxZ - g_nbMinZ) / NB_EXPECTED_RANGE) * 100.0f));
}

// ---------------------------------------------------------------------------
// Incremental 2-D ellipse fitter (live solution quality during bin cal)
// ---------------------------------------------------------------------------
// Model: A*xn^2 + C*yn^2 + D*xn + E*yn = 1  (axis-aligned ellipse, XY plane)
// where (xn, yn) = calibration-applied counts / FIT_NORM.
//
// Maintains the 4×4 normal equations incrementally so we can solve at any
// time without re-iterating over stored samples.  Calling magFit2DSolve()
// (once per CalProgressPacket, ~2 Hz) costs only a 4×4 Gauss-Jordan solve —
// microseconds on ESP32.
//
// For baseline cal with identity calibration loaded: fitter sees raw counts
// in µT-normalised space — estimates how elliptical the raw data is.
// For mounted cal with baseline cal loaded: fitter sees base-corrected counts
// — estimates the mounting residual ellipticity and expected heading error.
// ---------------------------------------------------------------------------

static constexpr float FIT_NORM = 4096.0f;   // normalise counts to ~unit range

struct MagFit2D {
    // Running sums for symmetric 4×4 XtX matrix.
    // Feature order: [xn², yn², xn, yn]
    float S_x4,  S_x2y2, S_y4;       // XtX [0,0] [0,1] [1,1]
    float S_x3,  S_x2y;              // XtX [0,2] [0,3]
    float S_xy2, S_y3;               // XtX [1,2] [1,3]
    float S_x2,  S_xy,   S_y2;      // XtX [2,2] [2,3] [3,3] — also Xtb[0..1]
    float S_x,   S_y;               // also Xtb[2..3]
    uint32_t n;

    // Current solution
    float A, C, D, E;               // ellipse coefficients
    float cx_norm, cy_norm;          // ellipse centre (normalised space)
    float rx_norm, ry_norm;          // semi-axes (normalised space)
    float hdg_err_deg;               // estimated heading error from ellipticity
    float alg_rms;                   // algebraic RMS residual (dimensionless)
    bool  valid;                     // true once a valid solution exists

    // Stability: centre shift since previous solve (normalised space)
    float prev_cx, prev_cy;
    float delta;                     // converges toward 0 as data stabilises
};

static MagFit2D g_magFit{};

static void magFit2DReset() {
    memset(&g_magFit, 0, sizeof(g_magFit));
}

// Add one sample.  Apply current g_magCalibration (bias + soft-iron, X and Y
// rows only) before accumulating so the fitter operates in calibrated space.
// With identity cal loaded: fitter sees raw counts. With base cal loaded: sees
// base-corrected residual.  In both cases FIT_NORM normalises to ~unit range.
static void magFit2DAdd(const Vec3i16& raw) {
    // Apply calibration (same transform as readMag(), skipping Z output)
    float xc = raw.x - g_magCalibration.bias.x;
    float yc = raw.y - g_magCalibration.bias.y;
    float zc = raw.z - g_magCalibration.bias.z;
    float xs = g_magCalibration.softIron[0][0]*xc
             + g_magCalibration.softIron[0][1]*yc
             + g_magCalibration.softIron[0][2]*zc;
    float ys = g_magCalibration.softIron[1][0]*xc
             + g_magCalibration.softIron[1][1]*yc
             + g_magCalibration.softIron[1][2]*zc;

    float xn = xs / FIT_NORM;
    float yn = ys / FIT_NORM;
    float x2 = xn*xn, y2 = yn*yn;

    MagFit2D& s = g_magFit;
    s.S_x4   += x2*x2;
    s.S_x2y2 += x2*y2;
    s.S_y4   += y2*y2;
    s.S_x3   += x2*xn;
    s.S_x2y  += x2*yn;
    s.S_xy2  += xn*y2;
    s.S_y3   += y2*yn;
    s.S_x2   += x2;
    s.S_xy   += xn*yn;
    s.S_y2   += y2;
    s.S_x    += xn;
    s.S_y    += yn;
    s.n++;
}

// Solve the 4×4 normal equations by Gauss-Jordan elimination.
// Populates g_magFit solution fields.  Returns true if valid ellipse found.
static bool magFit2DSolve() {
    MagFit2D& s = g_magFit;
    if (s.n < 8) { s.valid = false; return false; }

    // Augmented matrix [XtX | Xtb], row order: A, C, D, E
    // RHS Xtb_i = sum(phi_i * 1) = [S_x2, S_y2, S_x, S_y]
    float M[4][5] = {
        { s.S_x4,   s.S_x2y2, s.S_x3,  s.S_x2y, s.S_x2 },
        { s.S_x2y2, s.S_y4,   s.S_xy2, s.S_y3,  s.S_y2 },
        { s.S_x3,   s.S_xy2,  s.S_x2,  s.S_xy,  s.S_x  },
        { s.S_x2y,  s.S_y3,   s.S_xy,  s.S_y2,  s.S_y  },
    };

    // Gauss-Jordan with partial pivoting
    for (int col = 0; col < 4; col++) {
        int pivot = col;
        for (int row = col+1; row < 4; row++)
            if (fabsf(M[row][col]) > fabsf(M[pivot][col])) pivot = row;
        if (fabsf(M[pivot][col]) < 1e-8f) { s.valid = false; return false; }

        if (pivot != col)
            for (int j = 0; j <= 4; j++) { float t = M[col][j]; M[col][j] = M[pivot][j]; M[pivot][j] = t; }

        float inv = 1.0f / M[col][col];
        for (int j = col; j <= 4; j++) M[col][j] *= inv;

        for (int row = 0; row < 4; row++) {
            if (row == col) continue;
            float f = M[row][col];
            for (int j = col; j <= 4; j++) M[row][j] -= f * M[col][j];
        }
    }

    float A = M[0][4], C = M[1][4], D = M[2][4], E = M[3][4];
    if (A <= 0 || C <= 0) { s.valid = false; return false; }

    float denom = 1.0f + D*D/(4.0f*A) + E*E/(4.0f*C);
    if (denom <= 0) { s.valid = false; return false; }

    // Save previous centre for stability tracking
    s.prev_cx = s.cx_norm;
    s.prev_cy = s.cy_norm;

    s.A = A;  s.C = C;  s.D = D;  s.E = E;
    s.cx_norm = -D / (2.0f*A);
    s.cy_norm = -E / (2.0f*C);
    s.rx_norm = sqrtf(denom / A);
    s.ry_norm = sqrtf(denom / C);

    // Heading error estimate from XY ellipticity (degrees)
    float avg_r = (s.rx_norm + s.ry_norm) * 0.5f;
    float diff  = fabsf(s.rx_norm - s.ry_norm);
    s.hdg_err_deg = atan2f(diff, avg_r) * 57.2957795f;

    // Algebraic RMS residual from accumulated sums
    float SS =
        A*A*s.S_x4 + C*C*s.S_y4 + D*D*s.S_x2 + E*E*s.S_y2
      + 2.0f*A*C*s.S_x2y2 + 2.0f*A*D*s.S_x3 + 2.0f*A*E*s.S_x2y
      + 2.0f*C*D*s.S_xy2  + 2.0f*C*E*s.S_y3  + 2.0f*D*E*s.S_xy
      - 2.0f*A*s.S_x2 - 2.0f*C*s.S_y2 - 2.0f*D*s.S_x - 2.0f*E*s.S_y
      + (float)s.n;
    s.alg_rms = (SS > 0) ? sqrtf(SS / (float)s.n) : 0.0f;

    // Centre shift from previous solve
    float dcx = s.cx_norm - s.prev_cx, dcy = s.cy_norm - s.prev_cy;
    s.delta = sqrtf(dcx*dcx + dcy*dcy);

    s.valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// Bin-Aware Magnetometer Calibration Collection
// ---------------------------------------------------------------------------
// Collects raw mag samples bucketed into orientation bins (heading × elevation).
// Once a bin reaches the green threshold, further samples are rejected for that bin.
// This prevents oversampling of easy orientations and produces a more balanced fit.

// Maximum total samples we ever store across all bins
static constexpr int BIN_CAL_MAX_SAMPLES = 60 * MAG_CAL_BIN_GREEN_THRESHOLD * 2;  // 1800

struct BinCalSample {
    int16_t x, y, z;        // mag: raw logical-frame counts (post-axis-map, from readMagRaw())
    float   ax, ay, az;     // accel: calibrated, g (logical frame, same axis-map as mag)
    float   gx, gy, gz;     // gyro: calibrated, rad/s (logical frame, same axis-map as mag)
};

static bool          g_binCalActive    = false;
static bool          g_binCalMounted   = false;
static int           g_binCalBinCount  = 0;         // 60 or 36
static uint8_t       g_binCounts[60]   = {};         // samples per bin
static BinCalSample* g_binSamples      = nullptr;   // heap-allocated
static int           g_binSampleCount  = 0;
static int           g_lastComputedBin = -1;         // current device orientation (bin index), updated every tick
static float         g_lastPitchDeg    = 0.0f;       // actual pitch at last tick (for display readout)
static float         g_lastHdgDeg      = 0.0f;       // actual heading at last tick (for display readout)

// Per-bin last-accepted sample for runtime duplicate rejection.
// Prevents the same sensor reading from being counted multiple times when
// the magnetometer hasn't updated between AHRS loop iterations.
static BinCalSample g_binLastSample[60] = {};
static bool         g_binHasSample[60]  = {};

// ---------------------------------------------------------------------------
// Baseline cal: ROUGH_SCAN-only, no live grid (retired two-pass design).
//
// Three prior spikes tried to make a *live* grid trustworthy pre-calibration:
// (1) binning on the raw magnetometer vector against a Fibonacci lattice
// (broke screen locality — see git history), (2) against a fixed body axis
// (restored locality but was never gravity-referenced, and any
// elevation/azimuth parameterization has an unavoidable pole singularity
// regardless of which axis it's measured from), and (3) a two-pass handoff
// (ROUGH_SCAN gathers raw spread, hands a rough hard-iron bias to the
// existing Mahony/AHRS pipeline, then COLLECT resumes the original grid) —
// see docs/baseline-cal-two-pass.md for the full diagnostic history. On
// hardware, (3) still failed: no correlation between physical orientation
// and the highlighted box, consistent with AHRS filter lag/hysteresis under
// fast rotation, not a mapping bug.
//
// Current design (divemap/docs/architecture/baseline-cal-coverage-feedback-plan.md):
// stop trying to predict/display completion live. Baseline stays in
// ROUGH_SCAN for its entire session — honest per-axis range bars, sample
// count, live fit stats, no spatial promise. The diver ends the session
// themselves (magBinCalFinishBaseline) once it feels sufficient; the 9-axis
// CSV (mag + accel + gyro) uploads for real grading and coverage-gap
// feedback on the server, where a trustworthy heading can be computed
// *after* a real fit exists instead of needing one to already exist.
// ---------------------------------------------------------------------------

enum class BinCalSubPhase : uint8_t { ROUGH_SCAN, COLLECT };
static BinCalSubPhase g_binCalSubPhase = BinCalSubPhase::COLLECT;

// Map a sample's orientation to a bin index, or -1 if out of range for mounted
static int getBinIndex(float pitch_deg, float heading_deg, bool isMounted) {
    if (heading_deg < 0.0f) heading_deg += 360.0f;
    if (heading_deg >= 360.0f) heading_deg -= 360.0f;
    int hdgSector = (int)(heading_deg / 30.0f);
    if (hdgSector < 0) hdgSector = 0;
    if (hdgSector > 11) hdgSector = 11;

    int elevBand;
    if (isMounted) {
        // 3 bands: 0 = nose-up (≥+30°), 1 = level (-30°..+30°), 2 = nose-down (<-30°)
        // MAG_CAL_ELEV_L1 = -30.0f, so -MAG_CAL_ELEV_L1 = +30.0f
        if (pitch_deg >= -MAG_CAL_ELEV_L1)      elevBand = 0;  // pitch ≥ +30
        else if (pitch_deg >= MAG_CAL_ELEV_L1)  elevBand = 1;  // pitch ≥ -30 (level)
        else                                    elevBand = 2;  // pitch < -30
    } else {
        // 5 bands: 0=high2(>60), 1=high1(30-60), 2=level(-30-+30), 3=low1(-60..-30), 4=low2(<-60)
        if (pitch_deg >= MAG_CAL_ELEV_H2)        elevBand = 0;
        else if (pitch_deg >= MAG_CAL_ELEV_H1)   elevBand = 1;
        else if (pitch_deg >= MAG_CAL_ELEV_L1)   elevBand = 2;
        else if (pitch_deg >= MAG_CAL_ELEV_L2)   elevBand = 3;
        else                                      elevBand = 4;
    }

    int sectors = isMounted ? MAG_CAL_MOUNTED_HDG_SECTORS : MAG_CAL_BASELINE_HDG_SECTORS;
    return elevBand * sectors + hdgSector;
}

// Running min/max per axis — tracked during baseline ROUGH_SCAN, purely to
// render the per-axis coverage bars (magBinCalGetProgress's cov_x/y/z). Not
// soft-iron-aware and never applied as a calibration; the real bias/soft-iron
// fit (calibration-processor) only ever sees the raw stored samples.
static int16_t g_roughMin[3];
static int16_t g_roughMax[3];
static int     g_roughScanSampleCount = 0;

// Baseline only: set by magBinCalFinishBaseline() when the diver declares the
// session done. Baseline has no bin/grid completion concept anymore, so this
// is the only source of truth for magBinCalIsComplete() in that mode.
static bool g_baselineUserDone = false;

void magBinCalBegin(bool isMounted) {
    magBinCalEnd();  // clean up any previous run

    g_binCalMounted   = isMounted;
    g_binCalBinCount  = isMounted ? MAG_CAL_MOUNTED_BINS : MAG_CAL_BASELINE_BINS;
    g_binCalSubPhase  = isMounted ? BinCalSubPhase::COLLECT : BinCalSubPhase::ROUGH_SCAN;
    g_binSampleCount  = 0;
    memset(g_binCounts,     0, sizeof(g_binCounts));
    memset(g_binHasSample,  0, sizeof(g_binHasSample));
    memset(g_binLastSample, 0, sizeof(g_binLastSample));

    g_roughMin[0] = g_roughMin[1] = g_roughMin[2] = INT16_MAX;
    g_roughMax[0] = g_roughMax[1] = g_roughMax[2] = INT16_MIN;
    g_roughScanSampleCount = 0;
    g_baselineUserDone = false;

    g_binSamples = (BinCalSample*)malloc(BIN_CAL_MAX_SAMPLES * sizeof(BinCalSample));
    if (!g_binSamples) {
        Serial.println("[BIN_CAL] ERROR: malloc failed for sample buffer");
        return;
    }

    magFit2DReset();  // reset incremental fitter for fresh session
    g_binCalActive = true;
    Serial.printf("[BIN_CAL] Started %s cal (%s), %d bins, max %d samples\n",
                  isMounted ? "mounted" : "baseline",
                  g_binCalSubPhase == BinCalSubPhase::ROUGH_SCAN ? "rough scan" : "collect",
                  g_binCalBinCount, BIN_CAL_MAX_SAMPLES);
}

bool magBinCalTick(float pitch_deg, float heading_deg, const Vec3i16& rawMagSensor,
                    const Vec3f& accelCal, const Vec3f& gyroCal) {
    if (!g_binCalActive || !g_binSamples) return false;

    g_lastPitchDeg = pitch_deg;
    g_lastHdgDeg   = heading_deg;

    if (g_binCalSubPhase == BinCalSubPhase::ROUGH_SCAN) {
        g_lastComputedBin = -1;  // no grid in this phase

        if (rawMagSensor.x < g_roughMin[0]) g_roughMin[0] = rawMagSensor.x;
        if (rawMagSensor.y < g_roughMin[1]) g_roughMin[1] = rawMagSensor.y;
        if (rawMagSensor.z < g_roughMin[2]) g_roughMin[2] = rawMagSensor.z;
        if (rawMagSensor.x > g_roughMax[0]) g_roughMax[0] = rawMagSensor.x;
        if (rawMagSensor.y > g_roughMax[1]) g_roughMax[1] = rawMagSensor.y;
        if (rawMagSensor.z > g_roughMax[2]) g_roughMax[2] = rawMagSensor.z;

        // Exact-duplicate rejection against the single most recent sample —
        // there's no per-bin history in this phase, just one running "last".
        static BinCalSample s_lastRough{};
        static bool         s_hasLastRough = false;
        if (s_hasLastRough && s_lastRough.x == rawMagSensor.x &&
            s_lastRough.y == rawMagSensor.y && s_lastRough.z == rawMagSensor.z) {
            return false;
        }
        if (g_binSampleCount >= BIN_CAL_MAX_SAMPLES) return false;

        BinCalSample s = { rawMagSensor.x, rawMagSensor.y, rawMagSensor.z,
                            accelCal.x, accelCal.y, accelCal.z,
                            gyroCal.x, gyroCal.y, gyroCal.z };
        g_binSamples[g_binSampleCount++] = s;
        s_lastRough    = s;
        s_hasLastRough = true;
        g_roughScanSampleCount++;

        magFit2DAdd(rawMagSensor);
        return true;
    }

    // --- COLLECT: unchanged from the original AHRS-based grid ---
    int bin = getBinIndex(pitch_deg, heading_deg, g_binCalMounted);
    g_lastComputedBin = bin;  // always track current orientation, even if sample rejected
    if (bin < 0 || bin >= g_binCalBinCount) return false;

    // Reject if bin is already green
    if (g_binCounts[bin] >= MAG_CAL_BIN_GREEN_THRESHOLD) return false;

    // Reject exact duplicate — the magnetometer ODR (~80 Hz) is slower than the AHRS
    // loop (100 Hz), so consecutive reads often return the same raw value.
    if (g_binHasSample[bin]) {
        const BinCalSample& prev = g_binLastSample[bin];
        if (prev.x == rawMagSensor.x && prev.y == rawMagSensor.y && prev.z == rawMagSensor.z) {
            return false;
        }
    }

    // Reject if sample buffer is full
    if (g_binSampleCount >= BIN_CAL_MAX_SAMPLES) return false;

    BinCalSample s = { rawMagSensor.x, rawMagSensor.y, rawMagSensor.z,
                        accelCal.x, accelCal.y, accelCal.z,
                        gyroCal.x, gyroCal.y, gyroCal.z };
    g_binSamples[g_binSampleCount++] = s;
    g_binLastSample[bin]  = s;
    g_binHasSample[bin]   = true;
    g_binCounts[bin]++;

    // Feed accepted sample to incremental 2-D fitter (applies current cal internally)
    magFit2DAdd(rawMagSensor);
    return true;
}

bool magBinCalIsActive() { return g_binCalActive; }

bool magBinCalFinishBaseline() {
    if (!g_binCalActive || g_binCalMounted) return false;
    if (g_roughScanSampleCount < MAG_CAL_ROUGH_SCAN_MIN_SAMPLES) {
        Serial.printf("[BIN_CAL] Finish-baseline ignored: only %d samples (need %d)\n",
                      g_roughScanSampleCount, MAG_CAL_ROUGH_SCAN_MIN_SAMPLES);
        return false;
    }

    g_baselineUserDone = true;
    Serial.printf("[BIN_CAL] Baseline collection finished by diver: %d samples, "
                  "uploading for server-side grading\n", g_roughScanSampleCount);
    return true;
}

bool magBinCalIsComplete() {
    if (!g_binCalActive) return false;

    if (!g_binCalMounted) {
        // Baseline has no bin/grid completion concept anymore -- it's a diver
        // decision (magBinCalFinishBaseline), not auto-detected coverage.
        return g_baselineUserDone;
    }

    // Mounted: row-weighted grid completion, unchanged.
    const int sectors = MAG_CAL_BASELINE_HDG_SECTORS;  // 12
    const int rows    = MAG_CAL_MOUNTED_ELEV_BANDS;

    for (int r = 0; r < rows; r++) {
        // row 1 = level, rows 0/2 = +/-30 deg
        int required = (r == 1) ? MAG_CAL_SECTORS_LEVEL : MAG_CAL_SECTORS_MID;

        int greenInRow = 0;
        for (int c = 0; c < sectors; c++) {
            if (g_binCounts[r * sectors + c] >= MAG_CAL_BIN_GREEN_THRESHOLD) greenInRow++;
        }
        if (greenInRow < required) return false;
    }
    return true;
}

void magBinCalGetProgress(CalProgressPacket& pkt) {
    pkt.cal_type   = g_binCalMounted ? (uint8_t)CalType::MOUNTED : (uint8_t)CalType::BASELINE;
    pkt.phase      = (g_binCalSubPhase == BinCalSubPhase::ROUGH_SCAN)
                   ? (uint8_t)CalPhase::ROUGH_SCAN : (uint8_t)CalPhase::COLLECT;
    pkt.bins_total = (uint8_t)g_binCalBinCount;
    pkt.complete   = magBinCalIsComplete();
    pkt.sample_count = (uint16_t)g_binSampleCount;

    uint8_t green = 0;
    for (int i = 0; i < g_binCalBinCount; i++) {
        uint8_t cnt = g_binCounts[i];
        pkt.bin_counts[i] = cnt;
        if (cnt >= MAG_CAL_BIN_GREEN_THRESHOLD) green++;
    }
    // Zero-fill remaining slots (for baseline → 60, mounted → 36, rest = 0)
    for (int i = g_binCalBinCount; i < 60; i++) {
        pkt.bin_counts[i] = 0;
    }
    pkt.bins_green    = green;
    pkt.current_bin   = (g_lastComputedBin >= 0 && g_lastComputedBin < g_binCalBinCount)
                        ? (int8_t)g_lastComputedBin : (int8_t)-1;
    pkt.cur_pitch_deg = g_lastPitchDeg;
    pkt.cur_hdg_deg   = g_lastHdgDeg;

    if (g_binCalSubPhase == BinCalSubPhase::ROUGH_SCAN && g_roughScanSampleCount > 0) {
        // fmaxf(0, ...) guards the pre-first-sample state where max<min (sentinel
        // INT16_MAX/MIN) — negative-to-uint8_t conversion is UB, so clamp first.
        pkt.cov_x = (uint8_t)fmaxf(0.0f, fminf(100.0f, ((float)(g_roughMax[0] - g_roughMin[0]) / MAG_CAL_ROUGH_SCAN_EXPECTED_RANGE) * 100.0f));
        pkt.cov_y = (uint8_t)fmaxf(0.0f, fminf(100.0f, ((float)(g_roughMax[1] - g_roughMin[1]) / MAG_CAL_ROUGH_SCAN_EXPECTED_RANGE) * 100.0f));
        pkt.cov_z = (uint8_t)fmaxf(0.0f, fminf(100.0f, ((float)(g_roughMax[2] - g_roughMin[2]) / MAG_CAL_ROUGH_SCAN_EXPECTED_RANGE) * 100.0f));
    } else {
        pkt.cov_x = pkt.cov_y = pkt.cov_z = 0;
    }

    // Update incremental fit and populate quality fields.
    // magFit2DSolve() runs a 4×4 Gauss-Jordan solve on the accumulated sums —
    // cheap enough to call every time GetProgress is polled (~2 Hz).
    magFit2DSolve();
    pkt.fit_valid       = g_magFit.valid;
    pkt.fit_hdg_err_deg = g_magFit.hdg_err_deg;
    // Convert normalised-space delta to µT for a device-independent unit on display side.
    // FIT_NORM = 4096 counts; g_mag_lsb_per_uT ≈ 68.42 LSB/µT.
    pkt.fit_delta = (g_mag_lsb_per_uT > 0)
                  ? g_magFit.delta * FIT_NORM / g_mag_lsb_per_uT
                  : 0.0f;
}

void magBinCalDumpCSV(void* filePtr) {
    if (!g_binSamples || !filePtr) return;
    File& f = *reinterpret_cast<File*>(filePtr);
    f.println("mx,my,mz,ax,ay,az,gx,gy,gz");
    for (int i = 0; i < g_binSampleCount; i++) {
        const BinCalSample& s = g_binSamples[i];
        f.printf("%d,%d,%d,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f\n",
                 s.x, s.y, s.z, (double)s.ax, (double)s.ay, (double)s.az,
                 (double)s.gx, (double)s.gy, (double)s.gz);
    }
    Serial.printf("[BIN_CAL] Wrote %d samples to CSV\n", g_binSampleCount);
}

void magBinCalEnd() {
    if (g_binSamples) {
        free(g_binSamples);
        g_binSamples = nullptr;
    }
    magFit2DReset();
    g_binCalActive    = false;
    g_binCalSubPhase  = BinCalSubPhase::COLLECT;
    g_binSampleCount  = 0;
    g_binCalBinCount  = 0;
    g_roughScanSampleCount = 0;
    g_baselineUserDone = false;
    g_lastComputedBin = -1;
    g_lastPitchDeg    = 0.0f;
    g_lastHdgDeg      = 0.0f;
    memset(g_binCounts,     0, sizeof(g_binCounts));
    memset(g_binHasSample,  0, sizeof(g_binHasSample));
    memset(g_binLastSample, 0, sizeof(g_binLastSample));
}

// ----------- Gyroscope Calibration -----------
// Calibrates gyroscope bias by sampling at rest
void calibrateGyroscope(Calib3& out, uint32_t duration_ms) {
  if (!gyro_inited) {
    Serial.println("[CAL] Error: Gyroscope not initialized");
    out.bias = {0, 0, 0};
    out.scale = {1, 1, 1};
    return;
  }

  Serial.println("[CAL] ========================================");
  Serial.println("[CAL] GYROSCOPE CALIBRATION");
  Serial.println("[CAL] ========================================");
  Serial.println("[CAL] Place device on stable, level surface");
  Serial.println("[CAL] Do NOT move or rotate the device");
  Serial.print("[CAL] Starting in 5 seconds...");
  delay(5000);
  Serial.println(" Go!");

  // Initialize accumulators
  double sumX = 0, sumY = 0, sumZ = 0;
  uint32_t sampleCount = 0;
  uint32_t startTime = millis();

  // Sample for specified duration
  while (millis() - startTime < duration_ms) {
    Vec3i16 reading;
    if (readGyroRaw(reading) == ImuStatus::Ok) {
      sumX += reading.x;
      sumY += reading.y;
      sumZ += reading.z;
      sampleCount++;
    }
    delay(10);  // ~100 Hz sampling
  }

  // Calculate average (bias)
  if (sampleCount > 0) {
    out.bias.x = (float)(sumX / (double)sampleCount);
    out.bias.y = (float)(sumY / (double)sampleCount);
    out.bias.z = (float)(sumZ / (double)sampleCount);
  } else {
    out.bias = {0, 0, 0};
  }

  // Scale is identity (no scaling calibration for gyro currently)
  out.scale = {1, 1, 1};

  g_gyroCalibration = out;

  Serial.println("[CAL] Gyroscope calibration complete!");
  Serial.print("[CAL] Samples collected: ");
  Serial.println(sampleCount);
  Serial.print("[CAL] Gyro bias (raw counts): X=");
  Serial.print(out.bias.x, 1);
  Serial.print(" Y=");
  Serial.print(out.bias.y, 1);
  Serial.print(" Z=");
  Serial.println(out.bias.z, 1);
  Serial.print("[CAL] Gyro bias (rad/s): X=");
  Serial.print((out.bias.x / g_gyro_lsb_per_dps) * 0.017453292519943295f, 6);
  Serial.print(" Y=");
  Serial.print((out.bias.y / g_gyro_lsb_per_dps) * 0.017453292519943295f, 6);
  Serial.print(" Z=");
  Serial.println((out.bias.z / g_gyro_lsb_per_dps) * 0.017453292519943295f, 6);
}

// ----------- Accelerometer Calibration -----------
// Calibrates accelerometer by sampling at 6 orientations (±X, ±Y, ±Z)
void calibrateAccelerometer(Calib3& out, uint32_t sample_duration_ms) {
  if (!accel_inited) {
    Serial.println("[CAL] Error: Accelerometer not initialized");
    out.bias = {0, 0, 0};
    out.scale = {1, 1, 1};
    return;
  }

  Serial.println("[CAL] ========================================");
  Serial.println("[CAL] ACCELEROMETER CALIBRATION");
  Serial.println("[CAL] ========================================");
  Serial.println("[CAL] You will be asked to orient the device in 6 directions.");
  Serial.println("[CAL] For each direction: place device, wait for sampling, then proceed.");
  delay(3000);

  // Arrays to store average readings for each orientation
  // Calibration measures LOGICAL (NED frame) axes after axis mapping
  // NED frame: +X=forward/north, +Y=right/east, +Z=down
  // Place device with each NED axis pointing UP (against gravity)
  struct {
    Vec3f avg;
    const char* name;
  } measurements[6] = {
    { {0, 0, 0}, "Forward edge UP (NED +X up)" },
    { {0, 0, 0}, "Back edge UP (NED -X up)" },
    { {0, 0, 0}, "RIGHT edge UP (NED +Y up)" },
    { {0, 0, 0}, "LEFT edge UP (NED -Y up)" },
    { {0, 0, 0}, "BOTTOM side UP (NED +Z up - upside down)" },
    { {0, 0, 0}, "TOP side UP (NED -Z up - normal)" }
  };

  // Calibrate each axis
  for (int i = 0; i < 6; i++) {
    Serial.println("[CAL] ----------------------------------------");
    Serial.print("[CAL] Orientation ");
    Serial.print(i + 1);
    Serial.print(" of 6: ");
    Serial.println(measurements[i].name);
    Serial.print("[CAL] Place device with this axis UP, then press any key or wait 2 second...");
    delay(2500);
    Serial.println(" Sampling!");

    // Sample for specified duration
    double sumX = 0, sumY = 0, sumZ = 0;
    uint32_t sampleCount = 0;
    uint32_t startTime = millis();

    while (millis() - startTime < sample_duration_ms) {
      Vec3i16 reading;
      if (readAccelRaw(reading) == ImuStatus::Ok) {
        sumX += reading.x;
        sumY += reading.y;
        sumZ += reading.z;
        sampleCount++;
      }
      delay(10);
    }

    // Store average
    if (sampleCount > 0) {
      measurements[i].avg.x = (float)(sumX / (double)sampleCount);
      measurements[i].avg.y = (float)(sumY / (double)sampleCount);
      measurements[i].avg.z = (float)(sumZ / (double)sampleCount);
    }

    Serial.print("[CAL] Samples: ");
    Serial.print(sampleCount);
    Serial.print(" | Raw avg: X=");
    Serial.print(measurements[i].avg.x, 0);
    Serial.print(" Y=");
    Serial.print(measurements[i].avg.y, 0);
    Serial.print(" Z=");
    Serial.println(measurements[i].avg.z, 0);
  }

  // Calculate bias and scale from the 6 measurements
  // Each axis should show +/-1g when pointing up/down
  // bias = (max + min) / 2 (in raw counts)
  // scale = g_accel_lsb_per_g / (max - bias) (dimensionless correction factor)
  // Expected range is ±1g = ±g_accel_lsb_per_g counts

  // X axis
  float xMax = max(measurements[0].avg.x, measurements[1].avg.x);
  float xMin = min(measurements[0].avg.x, measurements[1].avg.x);
  out.bias.x = (xMax + xMin) / 2.0f;
  out.scale.x = g_accel_lsb_per_g / ((xMax - xMin) / 2.0f);  // Dimensionless

  // Y axis
  float yMax = max(measurements[2].avg.y, measurements[3].avg.y);
  float yMin = min(measurements[2].avg.y, measurements[3].avg.y);
  out.bias.y = (yMax + yMin) / 2.0f;
  out.scale.y = g_accel_lsb_per_g / ((yMax - yMin) / 2.0f);  // Dimensionless

  // Z axis
  float zMax = max(measurements[4].avg.z, measurements[5].avg.z);
  float zMin = min(measurements[4].avg.z, measurements[5].avg.z);
  out.bias.z = (zMax + zMin) / 2.0f;
  out.scale.z = g_accel_lsb_per_g / ((zMax - zMin) / 2.0f);  // Dimensionless

  g_accelCalibration = out;

  Serial.println("[CAL] ========================================");
  Serial.println("[CAL] ACCELEROMETER CALIBRATION COMPLETE");
  Serial.println("[CAL] ========================================");
  Serial.print("[CAL] Bias (raw counts): X=");
  Serial.print(out.bias.x, 1);
  Serial.print(" Y=");
  Serial.print(out.bias.y, 1);
  Serial.print(" Z=");
  Serial.println(out.bias.z, 1);
  Serial.print("[CAL] Bias (m/s²): X=");
  Serial.print(out.bias.x, 6);
  Serial.print(" Y=");
  Serial.print(out.bias.y, 6);
  Serial.print(" Z=");
  Serial.println(out.bias.z, 6);
  Serial.print("[CAL] Scale: X=");
  Serial.print(out.scale.x, 6);
  Serial.print(" Y=");
  Serial.print(out.scale.y, 6);
  Serial.print(" Z=");
  Serial.println(out.scale.z, 6);
}


// -------------- Init --------------
bool init(const ImuConfig& cfg, const AxisMap& accelGyroMap, const AxisMap& magMap) {
  g_cfg = cfg;
  g_accelGyroMap = accelGyroMap;
  g_magMap = magMap;

  Serial.println("Initializing IMU ...");

  // You can call each init separately too, but this is convenient.
  ImuStatus ag_return_val = initAccelGyro();
  if (ag_return_val != ImuStatus::Ok) {
    Serial.print("Error initializing IMU accel/gyro: ");
    Serial.println((int)ag_return_val);
    return false;
  }
  ImuStatus mag_return_val = initMag();
  if (mag_return_val != ImuStatus::Ok) {
    Serial.print("Error initializing IMU magnetometer: ");
    Serial.println((int)mag_return_val);
    return false;
  }
  
  //inited = true;
  return true;
}

ImuStatus initAccelGyro(TwoWire& wire) {
  gWire = &wire;

  // Initialize I2C bus with correct pins
  gWire->begin(SDA_PIN, SCL_PIN);
  delay(50);

  Serial.print("Scanning for LSM6DS3 at 0x6A or 0x6B...");
  if (!detectLsm6ds3Address()) {
    Serial.println(" FAILED");
    return ImuStatus::WhoAmIMismatch;
  }
  Serial.print(" Found at 0x");
  Serial.println(gLsmAddr, HEX);

  // Soft reset
  if (!write8(gLsmAddr, LSM6DS3_CTRL3_C, CTRL3_SW_RESET)) return ImuStatus::BusError;
  delay(50);

  // Enable register auto-increment + block data update
  // (Read-modify-write is safer than blasting a constant)
  uint8_t ctrl3 = 0;
  if (!read8(gLsmAddr, LSM6DS3_CTRL3_C, ctrl3)) return ImuStatus::BusError;
  ctrl3 |= (CTRL3_IF_INC | CTRL3_BDU);
  if (!write8(gLsmAddr, LSM6DS3_CTRL3_C, ctrl3)) return ImuStatus::BusError;

  // --- Set accel config (CTRL1_XL) ---
  // ODR_XL=104 Hz (0100), FS_XL=±4g (10), BW_XL=100 Hz (10)
  // => 0b0100_10_10 = 0x4A
  if (!write8(gLsmAddr, LSM6DS3_CTRL1_XL, 0x4A)) return ImuStatus::BusError;
  
  // Set accel sensitivity: ±4g range = 122 LSB/g (from datasheet)
  g_accel_lsb_per_g = 8192.0f;

  // --- Set gyro config (CTRL2_G) ---
  // ODR_G=104 Hz (0100), FS_G=±500 dps (01), FS_125=0
  // => 0b0100_01_00 = 0x44
  if (!write8(gLsmAddr, LSM6DS3_CTRL2_G, 0x44)) return ImuStatus::BusError;
  
  // Set gyro sensitivity: ±500 dps range = 65.5 LSB/dps (from datasheet)
  g_gyro_lsb_per_dps = 65.5f;

  accel_inited = true;
  gyro_inited = true;
  return ImuStatus::Ok;
}

ImuStatus initMag(TwoWire& wire) {
  // Optional: sanity check WHO_AM_I
  wire.begin(SDA_PIN, SCL_PIN);
  wire.beginTransmission(LIS3MDL_ADDR);
  wire.write(LIS3MDL_REG_WHO_AM_I);
  wire.endTransmission(false);
  wire.requestFrom((int)LIS3MDL_ADDR, 1);
  uint8_t who = wire.available() ? wire.read() : 0xFF;
  //Serial.print("LIS3MDL WHO_AM_I (expect 0x3D): 0x");
  //Serial.println(who, HEX);

  // Basic configuration:
  // CTRL_REG1: ultra-high-performance on X/Y, 10 Hz (for now), temp disabled
  // 0b01100000 = 0x60: TEMP_EN=0, OM=11 (UHP), DO=000 (0.625 Hz) – but let's bump to 10 Hz
  // The datasheet uses different encoding; 0x70 gives DO ~20 Hz w/ UHP.
  magWrite(LIS3MDL_REG_CTRL_REG1, 0x70); // UHP XY, ~20 Hz

  // CTRL_REG2: full-scale ±4 gauss (0x00) is fine
  magWrite(LIS3MDL_REG_CTRL_REG2, 0x00);

  // CTRL_REG3: continuous-conversion mode (MD[1:0] = 00)
  magWrite(LIS3MDL_REG_CTRL_REG3, 0x00);

  // CTRL_REG4: ultra-high-performance on Z
  magWrite(LIS3MDL_REG_CTRL_REG4, 0x0C);

  // CTRL_REG5: enable BDU (block data update) — prevents reading partial
  // register updates mid-conversion, which causes byte tearing
  magWrite(LIS3MDL_REG_CTRL_REG5, 0x40);

  delay(20); // give it a moment

  g_mag_lsb_per_uT = 6842.0f / 100.0f; // 6842 LSB/gauss = 6842/100 LSB/µT for ±4 gauss FS in UHP mode
  mag_inited = true;
  return ImuStatus::Ok;
}

// ----------- Raw reads (sensor frame, NO axis mapping) -----------
// These return data in the sensor's native coordinate frame for diagnostics
ImuStatus readAccelRaw_SensorFrame(Vec3i16& vecOut) {
  if (!accel_inited) return ImuStatus::NotInitialized;
  uint8_t b[6];
  if (!readN(gLsmAddr, LSM6DS3_OUTX_L_XL, b, 6)) return ImuStatus::BusError;

  vecOut.x = (int16_t)((uint16_t)b[1] << 8 | b[0]);
  vecOut.y = (int16_t)((uint16_t)b[3] << 8 | b[2]);
  vecOut.z = (int16_t)((uint16_t)b[5] << 8 | b[4]);

  return ImuStatus::Ok;
}

ImuStatus readGyroRaw_SensorFrame(Vec3i16& vecOut) {
  if (!gyro_inited) return ImuStatus::NotInitialized;
  uint8_t b[6];
  if (!readN(gLsmAddr, LSM6DS3_OUTX_L_G, b, 6)) return ImuStatus::BusError;

  vecOut.x = (int16_t)((uint16_t)b[1] << 8 | b[0]);
  vecOut.y = (int16_t)((uint16_t)b[3] << 8 | b[2]);
  vecOut.z = (int16_t)((uint16_t)b[5] << 8 | b[4]);

  return ImuStatus::Ok;
}

ImuStatus readMagRaw_SensorFrame(Vec3i16& vecOut) {
  if (!mag_inited) return ImuStatus::NotInitialized;
  uint8_t buffer[6];
  magRead(LIS3MDL_REG_OUT_X_L | 0x80, buffer, 6); // 0x80 for auto-increment

  // Little-endian: low byte first
  vecOut.x = (int16_t)(buffer[1] << 8 | buffer[0]);
  vecOut.y = (int16_t)(buffer[3] << 8 | buffer[2]);
  vecOut.z = (int16_t)(buffer[5] << 8 | buffer[4]);

  return ImuStatus::Ok;
}

// ----------- Raw reads (WITH axis mapping) -----------
ImuStatus readAccelRaw(Vec3i16& vecOut) {
  Vec3i16 sensorFrame;
  ImuStatus s = readAccelRaw_SensorFrame(sensorFrame);
  if (s != ImuStatus::Ok) return s;

  // Apply axis mapping to convert from sensor frame to logical frame (NED)
  vecOut = applyAxisMap(sensorFrame, g_accelGyroMap);
  return ImuStatus::Ok;
}

ImuStatus readGyroRaw(Vec3i16& vecOut) {
  Vec3i16 sensorFrame;
  ImuStatus s = readGyroRaw_SensorFrame(sensorFrame);
  if (s != ImuStatus::Ok) return s;

  // Apply axis mapping to convert from sensor frame to logical frame (NED)
  vecOut = applyAxisMap(sensorFrame, g_accelGyroMap);
  return ImuStatus::Ok;
}

ImuStatus readMagRaw(Vec3i16& vecOut) {
  Vec3i16 sensorFrame;
  ImuStatus s = readMagRaw_SensorFrame(sensorFrame);
  if (s != ImuStatus::Ok) return s;

  // Apply axis mapping to convert from sensor frame to logical frame (NED)
  vecOut = applyAxisMap(sensorFrame, g_magMap);
  return ImuStatus::Ok;
}

// ----------- Unit reads -----------
ImuStatus readAccel_g(Vec3f& out) {
  Vec3i16 raw;
  ImuStatus s = readAccelRaw(raw);
  if (s != ImuStatus::Ok) return s;

  Vec3f f = toFloat(raw);

  // Apply calibration in raw counts domain, then convert to g
  // Formula: ((raw - bias) * scale) / g_accel_lsb_per_g
  // Where bias is in counts, scale is dimensionless, result is in g
  out.x = ((f.x - g_accelCalibration.bias.x) * g_accelCalibration.scale.x) / g_accel_lsb_per_g;
  out.y = ((f.y - g_accelCalibration.bias.y) * g_accelCalibration.scale.y) / g_accel_lsb_per_g;
  out.z = ((f.z - g_accelCalibration.bias.z) * g_accelCalibration.scale.z) / g_accel_lsb_per_g;

  return ImuStatus::Ok;
}

// Read accelerometer with both raw (uncalibrated) and calibrated outputs
ImuStatus readAccel_g_raw_cal(Vec3f& rawOut, Vec3f& calOut) {
  Vec3i16 raw;
  ImuStatus s = readAccelRaw(raw);
  if (s != ImuStatus::Ok) return s;

  Vec3f f = toFloat(raw);

  // Raw (uncalibrated, just converted to g)
  rawOut = { f.x / g_accel_lsb_per_g, f.y / g_accel_lsb_per_g, f.z / g_accel_lsb_per_g };

  // Calibrated: ((raw - bias) * scale) / g_accel_lsb_per_g (result in g)
  calOut.x = ((f.x - g_accelCalibration.bias.x) * g_accelCalibration.scale.x) / g_accel_lsb_per_g;
  calOut.y = ((f.y - g_accelCalibration.bias.y) * g_accelCalibration.scale.y) / g_accel_lsb_per_g;
  calOut.z = ((f.z - g_accelCalibration.bias.z) * g_accelCalibration.scale.z) / g_accel_lsb_per_g;

  return ImuStatus::Ok;
}

ImuStatus readAccel_mps2(Vec3f& out) {
  Vec3f gVals;
  ImuStatus s = readAccel_g(gVals);
  if (s != ImuStatus::Ok) return s;

  const float GRAVITY = 9.81f;  // m/s²

  out = { gVals.x * GRAVITY, gVals.y * GRAVITY, gVals.z * GRAVITY };
  return ImuStatus::Ok;
}

ImuStatus readGyro_rad_s(Vec3f& out) {
  Vec3i16 raw;
  ImuStatus s = readGyroRaw(raw);
  if (s != ImuStatus::Ok) return s;

  Vec3f f = toFloat(raw);
  // Apply bias subtraction in raw counts, then convert to dps then rad/s
  float dps_x = (f.x - g_gyroCalibration.bias.x) / g_gyro_lsb_per_dps;
  float dps_y = (f.y - g_gyroCalibration.bias.y) / g_gyro_lsb_per_dps;
  float dps_z = (f.z - g_gyroCalibration.bias.z) / g_gyro_lsb_per_dps;

  out = { dpsToRad(dps_x), dpsToRad(dps_y), dpsToRad(dps_z) };
  return ImuStatus::Ok;
}

// Read gyroscope with both raw (uncalibrated) and calibrated outputs
ImuStatus readGyro_rad_s_raw_cal(Vec3f& rawOut, Vec3f& calOut) {
  Vec3i16 raw;
  ImuStatus s = readGyroRaw(raw);
  if (s != ImuStatus::Ok) return s;

  Vec3f f = toFloat(raw);
  float dps_x = f.x / g_gyro_lsb_per_dps;
  float dps_y = f.y / g_gyro_lsb_per_dps;
  float dps_z = f.z / g_gyro_lsb_per_dps;

  // Raw (uncalibrated, just converted to rad/s)
  rawOut = { dpsToRad(dps_x), dpsToRad(dps_y), dpsToRad(dps_z) };
  
  // Calibrated: apply bias subtraction
  float cal_dps_x = dps_x - (g_gyroCalibration.bias.x / g_gyro_lsb_per_dps);
  float cal_dps_y = dps_y - (g_gyroCalibration.bias.y / g_gyro_lsb_per_dps);
  float cal_dps_z = dps_z - (g_gyroCalibration.bias.z / g_gyro_lsb_per_dps);
  calOut = { dpsToRad(cal_dps_x), dpsToRad(cal_dps_y), dpsToRad(cal_dps_z) };

  return ImuStatus::Ok;
}

ImuStatus readMag_uT(Vec3f& out) {
  Vec3i16 raw;
  ImuStatus s = readMagRaw(raw);
  if (s != ImuStatus::Ok) return s;

  Vec3f f = toFloat(raw);
  out = { f.x / g_mag_lsb_per_uT, f.y / g_mag_lsb_per_uT, f.z / g_mag_lsb_per_uT };
  return ImuStatus::Ok;
}

// ----------- Calibration setters -----------
// Set the calibration data used by all read functions.
// Call these after loading from LittleFS or after running a calibrate function.
void setAccelCalibration(const Calib3& cal) {
  g_accelCalibration = cal;
}

void setGyroCalibration(const Calib3& cal) {
  g_gyroCalibration = cal;
}

void setMagCalibration(const MagCalib& cal) {
  g_magCalibration = cal;
}

// Read magnetometer with both raw (uncalibrated) and calibrated outputs
// rawOut: raw sensor values converted to µT (no calibration applied)
// calOut: with environmental offset (hard-iron) + soft-iron correction applied
ImuStatus readMag_raw_cal(Vec3f& rawOut, Vec3f& calOut) {
  Vec3i16 rawSensor;
  ImuStatus s = readMagRaw(rawSensor);
  if (s != ImuStatus::Ok) return s;

  // Convert raw to float in µT (uncalibrated)
  Vec3f rawFloat = toFloat(rawSensor);
  rawOut = { rawFloat.x / g_mag_lsb_per_uT, rawFloat.y / g_mag_lsb_per_uT, rawFloat.z / g_mag_lsb_per_uT };
  //rawOut = { rawFloat.x / g_mag_lsb_per_uT, rawFloat.y / g_mag_lsb_per_uT, rawFloat.z / g_mag_lsb_per_uT };

  // Apply hard-iron offset (bias subtraction)
  Vec3f hardIronCorrected = {
    rawFloat.x - g_magCalibration.bias.x,
    rawFloat.y - g_magCalibration.bias.y,
    rawFloat.z - g_magCalibration.bias.z
  };

  // Apply soft-iron correction (3x3 matrix multiplication)
  Vec3f corrected = {
    g_magCalibration.softIron[0][0] * hardIronCorrected.x +
    g_magCalibration.softIron[0][1] * hardIronCorrected.y +
    g_magCalibration.softIron[0][2] * hardIronCorrected.z,

    g_magCalibration.softIron[1][0] * hardIronCorrected.x +
    g_magCalibration.softIron[1][1] * hardIronCorrected.y +
    g_magCalibration.softIron[1][2] * hardIronCorrected.z,

    g_magCalibration.softIron[2][0] * hardIronCorrected.x +
    g_magCalibration.softIron[2][1] * hardIronCorrected.y +
    g_magCalibration.softIron[2][2] * hardIronCorrected.z
  };
  
  // Convert corrected magnetic field to µT
  calOut = { corrected.x / g_mag_lsb_per_uT, corrected.y / g_mag_lsb_per_uT, corrected.z / g_mag_lsb_per_uT };

  return ImuStatus::Ok;
}

// Read magnetometer with calibration applied (environmental offsets + soft-iron correction)
// Returns calibrated magnetic field as Vec3f
ImuStatus readMag(Vec3f& out) {
  Vec3i16 raw;
  ImuStatus s = readMagRaw(raw);
  if (s != ImuStatus::Ok) return s;

  // Convert raw to float
  Vec3f rawFloat = toFloat(raw);

  // Apply hard-iron offset (bias subtraction)
  Vec3f hardIronCorrected = {
    rawFloat.x - g_magCalibration.bias.x,
    rawFloat.y - g_magCalibration.bias.y,
    rawFloat.z - g_magCalibration.bias.z
  };

  // Apply soft-iron correction (3x3 matrix multiplication)
  out = {
    g_magCalibration.softIron[0][0] * hardIronCorrected.x +
    g_magCalibration.softIron[0][1] * hardIronCorrected.y +
    g_magCalibration.softIron[0][2] * hardIronCorrected.z,

    g_magCalibration.softIron[1][0] * hardIronCorrected.x +
    g_magCalibration.softIron[1][1] * hardIronCorrected.y +
    g_magCalibration.softIron[1][2] * hardIronCorrected.z,

    g_magCalibration.softIron[2][0] * hardIronCorrected.x +
    g_magCalibration.softIron[2][1] * hardIronCorrected.y +
    g_magCalibration.softIron[2][2] * hardIronCorrected.z
  };

  return ImuStatus::Ok;
}

} // namespace imu
