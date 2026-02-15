#include "orientation.h"
#include "mahony.h"
#include <math.h>

Euler quatToEulerRad(const Quaternion& q) {
  // yaw (Z), pitch (Y), roll (X) - common aerospace convention
  Euler e{0,0,0};

  float sinr_cosp = 2.0f * (q.w*q.x + q.y*q.z);
  float cosr_cosp = 1.0f - 2.0f * (q.x*q.x + q.y*q.y);
  e.roll = atan2f(sinr_cosp, cosr_cosp);

  float sinp = 2.0f * (q.w*q.y - q.z*q.x);
  if (fabsf(sinp) >= 1.0f) e.pitch = copysignf((float)M_PI/2.0f, sinp);
  else e.pitch = asinf(sinp);

  float siny_cosp = 2.0f * (q.w*q.z + q.x*q.y);
  float cosy_cosp = 1.0f - 2.0f * (q.y*q.y + q.z*q.z);
  e.yaw = atan2f(siny_cosp, cosy_cosp);

  return e;
}

float headingDegFromYawRad(float yawRad, float declinationDeg) {
  float deg = yawRad * 57.2957795f + declinationDeg;
  while (deg < 0) deg += 360.0f;
  while (deg >= 360.0f) deg -= 360.0f;
  return deg;
}

// Forward declarations of global AHRS state (defined in main.cpp)
extern MahonyState ahrs;
extern MahonyParams params;


namespace math {

// Update AHRS orientation using calibrated sensor data
// Integrates gyro/accel/mag readings into quaternion via Mahony filter
void updateOrientation(const imu::Vec3f& gyro_rad_s, const imu::Vec3f& accel_mss, const imu::Vec3f& mag, float dt) {
  // Call the Mahony filter with calibrated sensor data
  mahonyUpdate(ahrs, params, gyro_rad_s, accel_mss, mag, dt);
}

}  // namespace math
