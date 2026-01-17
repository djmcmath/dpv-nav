#line 1 "D:\\Documents\\dpv-nav\\firmware\\src\\sensors\\imu.cpp"
#include <Wire.h>
#include "../board_pins.h"
#include "../drivers/lis3mdl.h"
#include "../types/types.h"
#include <Arduino.h>
#include "imu.h"
#include <math.h>
//#include "i2c.h"

namespace imu {

// ----------- Internal state -----------
static bool g_inited = false;
static ImuConfig g_cfg{};
static AxisMap g_map{};

static float g_accel_lsb_per_g = 0.0f;     // counts per g
static float g_gyro_lsb_per_dps = 0.0f;    // counts per deg/s
static float g_mag_lsb_per_uT = 0.0f;      // counts per µT (if known)

static TwoWire* gWire = nullptr;
static uint8_t  gLsmAddr = LSM6DS3_ADDR_SA0_0;

// ---------- I2C addresses (7-bit) ----------
static constexpr uint8_t LSM6DS3_ADDR_SA0_0 = 0x6A; // SA0 low (derived from D4h/D5h patterns) :contentReference[oaicite:3]{index=3}
static constexpr uint8_t LSM6DS3_ADDR_SA0_1 = 0x6B; // SA0 high (common alt; use if 0x6A fails)
static constexpr uint8_t LIS3MDL_ADDR_DEFAULT = 0x1C; // :contentReference[oaicite:4]{index=4}

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
  if (read8(gLsmAddr, LSM6DS3_WHO_AM_I, who) && who == 0x69) return true; // :contentReference[oaicite:6]{index=6}

  gLsmAddr = LSM6DS3_ADDR_SA0_1;
  if (read8(gLsmAddr, LSM6DS3_WHO_AM_I, who) && who == 0x69) return true; // :contentReference[oaicite:7]{index=7}

  return false;
}

// ----------- Helpers -----------
static inline float dpsToRad(float dps) { return dps * 0.017453292519943295f; }

static Vec3i16 applyAxisMap(const Vec3i16& v) {
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

  return { pick(g_map.x_axis), pick(g_map.y_axis), pick(g_map.z_axis) };
}

static Vec3f toFloat(const Vec3i16& v) { return {(float)v.x, (float)v.y, (float)v.z}; }


// -------------- Init --------------
bool init(const ImuConfig& cfg, const AxisMap& map) {
  g_cfg = cfg;
  g_map = map;

  // You can call each init separately too, but this is convenient.
  if (initAccel() != ImuStatus::Ok) return false;
  if (initGyro()  != ImuStatus::Ok) return false;
  if (initMag()   != ImuStatus::Ok) return false;

  g_inited = true;
  return true;

  
}

ImuStatus initAccelGyro() {
  gWire = &wire;

  if (!detectLsm6ds3Address()) return false;

  // Soft reset
  if (!write8(gLsmAddr, LSM6DS3_CTRL3_C, CTRL3_SW_RESET)) return false;
  delay(50);

  // Enable register auto-increment + block data update
  // (Read-modify-write is safer than blasting a constant)
  uint8_t ctrl3 = 0;
  if (!read8(gLsmAddr, LSM6DS3_CTRL3_C, ctrl3)) return false;
  ctrl3 |= (CTRL3_IF_INC | CTRL3_BDU);
  if (!write8(gLsmAddr, LSM6DS3_CTRL3_C, ctrl3)) return false;

  // --- Set accel config (CTRL1_XL) ---
  // ODR_XL=104 Hz (0100), FS_XL=±4g (10), BW_XL=100 Hz (10)
  // => 0b0100_10_10 = 0x4A
  if (!write8(gLsmAddr, LSM6DS3_CTRL1_XL, 0x4A)) return false;

  // --- Set gyro config (CTRL2_G) ---
  // ODR_G=104 Hz (0100), FS_G=±500 dps (01), FS_125=0
  // => 0b0100_01_00 = 0x44
  if (!write8(gLsmAddr, LSM6DS3_CTRL2_G, 0x44)) return false;

  return true;
  return ImuStatus::Ok;
}

ImuStatus initMag() {
  // Optional: sanity check WHO_AM_I
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.beginTransmission(LIS3MDL_ADDR);
  Wire.write(LIS3MDL_REG_WHO_AM_I);
  Wire.endTransmission(false);
  Wire.requestFrom((int)LIS3MDL_ADDR, 1);
  uint8_t who = Wire.available() ? Wire.read() : 0xFF;

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

  delay(20); // give it a moment

  g_mag_lsb_per_uT = 1.0f; // placeholder
  return ImuStatus::Ok;
}

// ----------- Raw reads -----------
ImuStatus readAccelRaw(Vec3i16& out) {
  if (!g_inited) return ImuStatus::NotInitialized;
  uint8_t b[6];
  if (!readN(gLsmAddr, LSM6DS3_OUTX_L_XL, b, 6)) return false;

  out.x = (int16_t)((uint16_t)b[1] << 8 | b[0]);
  out.y = (int16_t)((uint16_t)b[3] << 8 | b[2]);
  out.z = (int16_t)((uint16_t)b[5] << 8 | b[4]);
  return ImuStatus::Ok;
}

ImuStatus readGyroRaw(Vec3i16& out) {
  if (!g_inited) return ImuStatus::NotInitialized;
  uint8_t b[6];
  if (!readN(gLsmAddr, LSM6DS3_OUTX_L_G, b, 6)) return false;

  out.x = (int16_t)((uint16_t)b[1] << 8 | b[0]);
  out.y = (int16_t)((uint16_t)b[3] << 8 | b[2]);
  out.z = (int16_t)((uint16_t)b[5] << 8 | b[4]);
  return ImuStatus::Ok;
}

ImuStatus readMagRaw(int16_t &mx, int16_t &my, int16_t &mz) {
  uint8_t buffer[6];
  magRead(LIS3MDL_REG_OUT_X_L | 0x80, buffer, 6); // 0x80 for auto-increment

  // Little-endian: low byte first
  mx = (int16_t)(buffer[1] << 8 | buffer[0]);
  my = (int16_t)(buffer[3] << 8 | buffer[2]);
  mz = (int16_t)(buffer[5] << 8 | buffer[4]);

  // Create the return object
  mag_reading reading;
  reading.x = mx;
  reading.y = my;
  reading.z = mz;
  return reading;

    if (!g_inited) return ImuStatus::NotInitialized;
  out = {0,0,0}; // TODO (or call your existing code)
  out = applyAxisMap(out);
  return ImuStatus::Ok;
}

// ----------- Unit reads -----------
ImuStatus readAccel_g(Vec3f& out) {
  Vec3i16 raw;
  ImuStatus s = readAccelRaw(raw);
  if (s != ImuStatus::Ok) return s;

  Vec3f f = toFloat(raw);
  out = { f.x / g_accel_lsb_per_g, f.y / g_accel_lsb_per_g, f.z / g_accel_lsb_per_g };
  return ImuStatus::Ok;
}

ImuStatus readGyro_rad_s(Vec3f& out) {
  Vec3i16 raw;
  ImuStatus s = readGyroRaw(raw);
  if (s != ImuStatus::Ok) return s;

  Vec3f f = toFloat(raw);
  float dps_x = f.x / g_gyro_lsb_per_dps;
  float dps_y = f.y / g_gyro_lsb_per_dps;
  float dps_z = f.z / g_gyro_lsb_per_dps;

  out = { dpsToRad(dps_x), dpsToRad(dps_y), dpsToRad(dps_z) };
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

} // namespace imu
