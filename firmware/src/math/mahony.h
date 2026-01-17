#pragma once
#include <stdint.h>

struct Vec3 { float x, y, z; };

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
  Vec3 integralFB;   // integral feedback (gyro bias estimate-ish)
};

void mahonyInit(MahonyState& s);
void mahonyUpdate(
  MahonyState& s,
  const MahonyParams& p,
  const Vec3& gyroRad_s,     // rad/s
  const Vec3& accel,         // any units; will be normalized internally
  const Vec3& mag,           // any units; will be normalized internally
  float dtSeconds
);

void quatNormalize(Quaternion& q);
