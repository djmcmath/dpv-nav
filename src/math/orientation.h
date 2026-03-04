#pragma once
#include "mahony.h"
#include "../types/types.h"

struct Euler { float yaw, pitch, roll; }; // radians

Euler quatToEulerRad(const Quaternion& q);

// heading in degrees [0,360)
float headingDegFromYawRad(float yawRad, float declinationDeg = 0.0f);

// Update AHRS orientation using calibrated sensor data
// Must call with calibrated sensor readings and delta time
// gyro: angular velocity in rad/s
// accel: acceleration in m/s² (or normalized)
// mag: magnetic field (normalized or in any units)
// dt: delta time in seconds
// This updates the global AHRS quaternion via the Mahony filter
namespace math {
  void updateOrientation(const imu::Vec3f& gyro_rad_s, const imu::Vec3f& accel_mss, const imu::Vec3f& mag, float dt);
}
