#pragma once
#include <stdint.h>
#include "../types/types.h"

struct Quaternion {
  float w, x, y, z;
};

struct MahonyParams {
  float kp;          // proportional gain (e.g. 2.0)
  float ki;          // integral gain (e.g. 0.0 to start)
  bool useMag;       // start true, but allow disabling if mag unhealthy
};

struct MahonyState {
  Quaternion q;      // orientation
  imu::Vec3f integralFB;   // integral feedback (gyro bias estimate-ish)
};

void mahonyInit(MahonyState& s);
void mahonyUpdate(
  MahonyState& s,
  const MahonyParams& p,
  const imu::Vec3f& gyroRad_s,     // rad/s
  const imu::Vec3f& accel,         // any units; will be normalized internally
  const imu::Vec3f& mag,           // any units; will be normalized internally
  float dtSeconds
);

void quatNormalize(Quaternion& q);
