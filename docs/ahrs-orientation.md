# AHRS Orientation System

## Overview

The AHRS (Attitude and Heading Reference System) fuses gyroscope, accelerometer, and magnetometer data into a stable orientation estimate using the Mahony complementary filter. The output is a quaternion that can be converted to Euler angles (roll, pitch, yaw) and compass heading.

## Mahony Filter

The Mahony filter is a complementary filter that combines:
- **Fast path (gyroscope):** Integrates angular velocity for rapid orientation tracking. Drifts over time.
- **Slow path (accel + mag):** Corrects drift using gravity direction (accel) and magnetic north (mag). Stable but slow.
- **Feedback:** Error between predicted and measured gravity/heading corrects gyro bias.

### Parameters

Configured in [nav_main.cpp](../src/nav_main.cpp):

```cpp
MahonyState ahrs;
MahonyParams params{ .kp = 1.0f, .ki = 0.005f, .useMag = true };
```

| Parameter | Default | Effect |
|-----------|---------|--------|
| `kp` | 1.0 | Proportional gain — higher = faster correction, more oscillation |
| `ki` | 0.005 | Integral gain — corrects persistent gyro bias over time |
| `useMag` | true | Enable magnetometer for yaw/heading correction |

### Update Loop

Called every iteration in the nav device main loop (~100 Hz):

```cpp
mahonyUpdate(ahrs, params, gyro_rad_s, accel, mag, dt);
```

## Euler Angle Extraction

```cpp
Euler euler = quatToEulerRad(ahrs.q);
float headingDeg = headingDegFromYawRad(euler.yaw);  // 0-360°
float pitchDeg   = euler.pitch * (180.0f / M_PI);
float rollDeg    = euler.roll  * (180.0f / M_PI);
```

### Angle Definitions

| Angle | Axis | Positive direction | Range |
|-------|------|--------------------|-------|
| Roll | X (forward) | Right side up | -180° to +180° |
| Pitch | Y (lateral) | Nose up | -90° to +90° |
| Yaw | Z (vertical) | Clockwise from above | -180° to +180° |

**Heading** is yaw converted to 0-360° compass convention.

### Magnetic Declination

```cpp
// Optional: adjust heading for local magnetic declination
float heading = headingDegFromYawRad(euler.yaw, declinationDeg);
// declinationDeg: positive=East, negative=West
```

Currently not applied (heading is magnetic, not true). `FLAG_TRUE_HEADING` in NavPacket is reserved for when declination is implemented.

## Update Rate

**Recommended: 100 Hz** (10 ms between updates)

| Rate | Effect |
|------|--------|
| <20 Hz | Poor heading tracking, sluggish response |
| 100 Hz | Good balance of accuracy and CPU usage |
| >200 Hz | Minimal benefit, wastes CPU |

Variable dt is handled correctly by the filter, but constant dt is slightly better.

## Quaternion Access

For advanced use, access the quaternion directly:

```cpp
float qw = ahrs.q.w;  // scalar
float qx = ahrs.q.x;  // vector components
float qy = ahrs.q.y;
float qz = ahrs.q.z;
```

The quaternion representation is singularity-free (no gimbal lock), unlike Euler angles which can experience gimbal lock at pitch = ±90°.

## Troubleshooting

| Problem | Cause | Fix |
|---------|-------|-----|
| Heading drifts over time | Mag not calibrated or disabled | Run mag calibration, ensure `useMag = true` |
| Large roll/pitch oscillations | kp too high | Reduce `kp` (try 0.5) |
| Slow response to motion | kp too low | Increase `kp` (try 2.0) |
| Roll/pitch unstable when still | Accel noise or poor cal | Re-calibrate accelerometer |
| Yaw doesn't respond | Mag disabled or uncalibrated | Enable `useMag`, run mag calibration |
