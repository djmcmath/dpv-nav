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
// CRITICAL: All three sensor vectors must be in the SAME right-handed NED body frame.
// The filter uses cross products between accel and mag vectors — if their coordinate
// frames differ (e.g. one axis is flipped in mag but not accel), the heading will
// converge to 360°-true instead of true heading, and rotation sense will be reversed.
// See nav_main.cpp for the magNED correction that handles the LIS3MDL Y-axis flip.
void mahonyUpdate(
  MahonyState& s,
  const MahonyParams& p,
  const imu::Vec3f& gyroRad_s,     // rad/s, NED body frame
  const imu::Vec3f& accel,         // any units, NED body frame; normalized internally
  const imu::Vec3f& mag,           // any units, NED body frame; normalized internally
  float dtSeconds
);

void quatNormalize(Quaternion& q);
