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
MahonyParams mahonyParams{ .kp = 1.0f, .ki = 0.002f, .useMag = true };
```

| Parameter | Default | Range | Effect |
|-----------|---------|-------|--------|
| `kp` | 1.0 | 0.5–2.0 | Proportional gain — higher = faster correction from accel/mag, lower = smoother gyro-dominated response |
| `ki` | 0.002 | 0.0–0.01 | Integral gain — builds gyro bias estimate over time. Too high causes windup/oscillation |
| `useMag` | true | — | Enable magnetometer for yaw/heading correction |

### Update Loop

Called every iteration in the nav device main loop (~100 Hz):

```cpp
imu::Vec3f magNED = { mag.x, -mag.y, mag.z };  // Fix mag Y to NED frame
mahonyUpdate(ahrs, mahonyParams, gyro, accel, magNED, dt);
```

> **CRITICAL — Sensor Frame Consistency:** All three sensor inputs must be in the **same right-handed NED body frame**. On this board, the LIS3MDL magnetometer has an inverted Y-axis relative to the LSM6DS33 accel/gyro. After axis mapping and calibration, the mag output has Y in a left-handed convention (positive = Left). The `mag.y` negation above corrects this before passing to the filter.
>
> **If the frames don't match, the filter converges to `360° - true_heading` instead of `true_heading`**, and rotating right will decrease indicated heading. This is a stable but wrong heading (not drift), making it hard to diagnose. The cross-product error term inside the filter drives the quaternion to a mirror-image equilibrium when mag and accel Y axes have opposite sign conventions.
>
> See the "Magnetometer Coordinate Frame" section in [CLAUDE.md](../CLAUDE.md) for full details and rules to prevent re-breaking this.

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
| **Stable but wrong heading** (consistent offset, not drift) | Mag/accel Y-axis sign mismatch — filter converges to `360°-true` | Ensure `magNED` negates mag.y before `mahonyUpdate()`. See CLAUDE.md "Magnetometer Coordinate Frame" |
| **Rotating right decreases heading** | Same cause as above — mag Y flipped vs accel Y | Same fix: negate mag.y before filter input |
| Wildly fluctuating mag at rest | LIS3MDL BDU not enabled (byte tearing) | Ensure CTRL_REG5 = 0x40 is written during mag init |
| ki > 0 causes heading to overshoot and oscillate | Integral windup from sustained error | Reduce ki (try 0.001), or set to 0 while debugging other issues |
