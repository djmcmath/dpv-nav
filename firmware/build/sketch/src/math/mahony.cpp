#line 1 "D:\\Documents\\dpv-nav\\firmware\\src\\math\\mahony.cpp"
#include "mahony.h"
#include <math.h>

static inline float invSqrt(float x) {
  return 1.0f / sqrtf(x);
}

static inline Vec3 cross(const Vec3& a, const Vec3& b) {
  return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
static inline float dot(const Vec3& a, const Vec3& b) {
  return a.x*b.x + a.y*b.y + a.z*b.z;
}
static inline Vec3 add(const Vec3& a, const Vec3& b) {
  return {a.x+b.x, a.y+b.y, a.z+b.z};
}
static inline Vec3 sub(const Vec3& a, const Vec3& b) {
  return {a.x-b.x, a.y-b.y, a.z-b.z};
}
static inline Vec3 mul(const Vec3& a, float k) {
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

static inline Vec3 quatRotate(const Quaternion& q, const Vec3& v) {
  // v' = q * (0,v) * q_conj
  Quaternion qv{0, v.x, v.y, v.z};
  Quaternion qc{q.w, -q.x, -q.y, -q.z};
  Quaternion r = quatMul(quatMul(q, qv), qc);
  return {r.x, r.y, r.z};
}

void mahonyUpdate(
  MahonyState& s,
  const MahonyParams& p,
  const Vec3& gyro,
  const Vec3& accelIn,
  const Vec3& magIn,
  float dt
) {
  // 1) Normalize accel (gravity direction)
  Vec3 a = accelIn;
  float an2 = a.x*a.x + a.y*a.y + a.z*a.z;
  if (an2 < 1e-12f) return; // invalid accel
  float invAn = invSqrt(an2);
  a = mul(a, invAn);

  // 2) Estimate gravity direction from quaternion (what "down" should be)
  // In body frame, gravity points approximately (0,0,1) if using NED vs ENU conventions.
  // We'll derive expected gravity by rotating world "down" into body frame.
  // Choose worldDown = (0,0,1) for a common IMU body frame where +Z points up; adjust if needed.
  const Vec3 worldDown = {0, 0, 1};
  Vec3 vDown = quatRotate(s.q, worldDown); // expected accel direction in body frame

  // Error is cross between measured and expected gravity
  Vec3 e = cross(a, vDown);

  // 3) Optional magnetometer correction (yaw)
  if (p.useMag) {
    Vec3 m = magIn;
    float mn2 = m.x*m.x + m.y*m.y + m.z*m.z;
    if (mn2 > 1e-12f) {
      float invMn = invSqrt(mn2);
      m = mul(m, invMn);

      // Project mag into horizontal plane using measured "down" (a)
      // mh = m - (m·a)a
      Vec3 mh = sub(m, mul(a, dot(m, a)));
      float mhn2 = dot(mh, mh);
      if (mhn2 > 1e-12f) {
        mh = mul(mh, invSqrt(mhn2));

        // Expected magnetic direction: take worldNorth = (1,0,0) and rotate into body,
        // then project to horizontal plane similarly.
        const Vec3 worldNorth = {1, 0, 0};
        Vec3 vN = quatRotate(s.q, worldNorth);
        vN = sub(vN, mul(a, dot(vN, a)));
        float vNn2 = dot(vN, vN);
        if (vNn2 > 1e-12f) {
          vN = mul(vN, invSqrt(vNn2));
          Vec3 eMag = cross(mh, vN);
          e = add(e, eMag);
        }
      }
    }
  }

  // 4) Apply feedback to gyro
  Vec3 gyroCorrected = gyro;

  if (p.ki > 0.0f) {
    s.integralFB = add(s.integralFB, mul(e, p.ki * dt));
    gyroCorrected = add(gyroCorrected, s.integralFB);
  }

  gyroCorrected = add(gyroCorrected, mul(e, p.kp));

  // 5) Integrate quaternion rate: q_dot = 0.5 * q * omegaQuat
  Quaternion omega{0, gyroCorrected.x, gyroCorrected.y, gyroCorrected.z};
  Quaternion qDot = quatMul(s.q, omega);
  s.q.w += 0.5f * qDot.w * dt;
  s.q.x += 0.5f * qDot.x * dt;
  s.q.y += 0.5f * qDot.y * dt;
  s.q.z += 0.5f * qDot.z * dt;

  quatNormalize(s.q);
}
