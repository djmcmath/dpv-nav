#line 1 "D:\\Documents\\dpv-nav\\firmware\\src\\sensors\\imu.h"

#pragma once
#include <stdint.h>
#include "../types/types.h"
#include <Arduino.h>
#include <Wire.h>

namespace imu {

struct Vec3i16 { int16_t x, y, z; };
struct Vec3f   { float   x, y, z; };

enum class ImuStatus : uint8_t {
  Ok = 0,
  NotInitialized,
  BusError,
  WhoAmIMismatch,
  DataNotReady,
};

struct ImuConfig {
  // Full-scale ranges (you’ll map to sensor register values in .cpp)
  float accel_g_fullscale;     // e.g. 2, 4, 8, 16
  float gyro_dps_fullscale;    // e.g. 250, 500, 1000, 2000
  float mag_uT_fullscale;      // depends on sensor; ok to ignore initially
  uint16_t sample_hz;          // e.g. 100
};

struct AxisMap {
  // maps logical X/Y/Z to sensor axes with sign
  // Example: +X = sensor Y, +Y = -sensor X, +Z = sensor Z
  int8_t x_axis; // +1=+X, -1=-X, +2=+Y, -2=-Y, +3=+Z, -3=-Z
  int8_t y_axis;
  int8_t z_axis;
};

bool init(const ImuConfig& cfg, const AxisMap& map);

ImuStatus initAccelGyro(TwoWire& wire =  Wire);
ImuStatus initMag(TwoWire& wire = Wire);

ImuStatus readAccelRaw(Vec3i16& out);
ImuStatus readGyroRaw(Vec3i16& out);
ImuStatus readMagRaw(Vec3i16& out);

// Converted units:
ImuStatus readAccel_g(Vec3f& out);        // g
ImuStatus readGyro_rad_s(Vec3f& out);     // rad/s
ImuStatus readMag_uT(Vec3f& out);         // µT (or “sensor units” if you don’t have scale yet)


//    void init();
//    void initMag();
//    mag_reading readMagRaw(int16_t &mx, int16_t &my, int16_t &mz);

}