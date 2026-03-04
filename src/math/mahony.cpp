#include "mahony.h"
#include <math.h>

static inline float invSqrt(float x) {
  return 1.0f / sqrtf(x);
}

static inline imu::Vec3f cross(const imu::Vec3f& a, const imu::Vec3f& b) {
  return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
static inline float dot(const imu::Vec3f& a, const imu::Vec3f& b) {
  return a.x*b.x + a.y*b.y + a.z*b.z;
}
static inline imu::Vec3f add(const imu::Vec3f& a, const imu::Vec3f& b) {
  return {a.x+b.x, a.y+b.y, a.z+b.z};
}
static inline imu::Vec3f sub(const imu::Vec3f& a, const imu::Vec3f& b) {
  return {a.x-b.x, a.y-b.y, a.z-b.z};
}
static inline imu::Vec3f mul(const imu::Vec3f& a, float k) {
  return {a.x*k, a.y*k, a.z*k};
}

void mahonyInit(MahonyState& s) {
  s.q = {1,0,0,0};
  s.integralFB = {0,0,0};
}

void quatNormalize(Quaternion& q) {
  float n = invSqrt(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
  q.w *= n; q.x *= n; q.y *= n; q.z *= n;
}

static inline Quaternion quatMul(const Quaternion& a, const Quaternion& b) {
  return {
    a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
    a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
    a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
    a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w
  };
}

// Body-to-world rotation: v_world = q * v_body * q_conj
static inline imu::Vec3f quatRotate(const Quaternion& q, const imu::Vec3f& v) {
  Quaternion qv{0, v.x, v.y, v.z};
  Quaternion qc{q.w, -q.x, -q.y, -q.z};
  Quaternion r = quatMul(quatMul(q, qv), qc);
  return {r.x, r.y, r.z};
}

// World-to-body rotation: v_body = q_conj * v_world * q
static inline imu::Vec3f quatRotateWorldToBody(const Quaternion& q, const imu::Vec3f& v) {
  Quaternion qc{q.w, -q.x, -q.y, -q.z};
  Quaternion qv{0, v.x, v.y, v.z};
  Quaternion r = quatMul(quatMul(qc, qv), q);
  return {r.x, r.y, r.z};
}

void mahonyUpdate(
  MahonyState& s,
  const MahonyParams& p,
  const imu::Vec3f& gyro,
  const imu::Vec3f& accelIn,
  const imu::Vec3f& magIn,
  float dt
) {
  // Reference frame: NED (North-East-Down)
  //   Body frame X = North, Y = East, Z = Down
  //   Body Z-axis (down) should align with gravity vector
  //   Body X-axis (north) should align with magnetic north

  // 1) Normalize accel (gravity direction in body frame)
  imu::Vec3f a = accelIn;
  float an2 = a.x*a.x + a.y*a.y + a.z*a.z;
  if (an2 < 1e-12f) return; // invalid accel
  float invAn = invSqrt(an2);
  a = mul(a, invAn);

  // 2) Expected accelerometer direction in body frame from current quaternion estimate
  //    Accel measures specific force (reaction to gravity) = {0,0,-1g} when level in NED
  const imu::Vec3f accelRef = {0, 0, -1};
  imu::Vec3f expectedGravity = quatRotateWorldToBody(s.q, accelRef);

  // Roll/pitch error: cross product between measured and expected gravity
  imu::Vec3f eAccel = cross(a, expectedGravity);

  imu::Vec3f e = eAccel;

  // 3) Magnetometer correction for yaw (only if enabled and valid and not in gimbal lock region)
  if (p.useMag) {
    // Disable mag near gimbal lock (pitch > 60°) to avoid destabilization
    // compute sin(pitch) from quaternion to detect gimbal lock
    float sinp = 2.0f * (s.q.w * s.q.y - s.q.z * s.q.x);
    float absSinp = fabsf(sinp);
    if (absSinp < 0.866f) {  // 0.866 ≈ sin(60°), allows ±60° pitch range
      imu::Vec3f m = magIn;
      float mn2 = m.x*m.x + m.y*m.y + m.z*m.z;
      if (mn2 > 1e-12f) {
        float invMn = invSqrt(mn2);
        m = mul(m, invMn);

        // Remove gravity component from mag reading: project to horizontal plane
        // mh_body = m - (m·a)a
        float mDotA = dot(m, a);
        imu::Vec3f mhBody = sub(m, mul(a, mDotA));
        float mh2 = dot(mhBody, mhBody);

        if (mh2 > 1e-12f) {
          mhBody = mul(mhBody, invSqrt(mh2));  // normalize to unit horizontal component

          // Expected magnetic north direction in body frame from current quaternion
          const imu::Vec3f worldNorth = {1, 0, 0};  // NED: north is +X
          imu::Vec3f expectedMagBody = quatRotateWorldToBody(s.q, worldNorth);

          // Project expected field to horizontal plane
          float eMagDotA = dot(expectedMagBody, a);
          imu::Vec3f expectedMhBody = sub(expectedMagBody, mul(a, eMagDotA));
          float expectedMh2 = dot(expectedMhBody, expectedMhBody);

          if (expectedMh2 > 1e-12f) {
            expectedMhBody = mul(expectedMhBody, invSqrt(expectedMh2));  // normalize

            // Yaw error: cross product in horizontal plane
            imu::Vec3f eMag = cross(mhBody, expectedMhBody);
            e = add(e, eMag);
          }
        }
      }
    }
  }

  // 4) Apply proportional feedback and integral feedback to gyro
  imu::Vec3f gyroCorrected = gyro;

  if (p.ki > 0.0f) {
    s.integralFB = add(s.integralFB, mul(e, p.ki * dt));
    gyroCorrected = add(gyroCorrected, s.integralFB);
  }

  gyroCorrected = add(gyroCorrected, mul(e, p.kp));

  // 5) Integrate quaternion rate: q_dot = 0.5 * q * omega_quat
  Quaternion omega{0, gyroCorrected.x, gyroCorrected.y, gyroCorrected.z};
  Quaternion qDot = quatMul(s.q, omega);
  s.q.w += 0.5f * qDot.w * dt;
  s.q.x += 0.5f * qDot.x * dt;
  s.q.y += 0.5f * qDot.y * dt;
  s.q.z += 0.5f * qDot.z * dt;

  quatNormalize(s.q);
}
