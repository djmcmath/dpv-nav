# AHRS Orientation Update

## Overview

The `math::updateOrientation()` function integrates calibrated sensor data (gyroscope, accelerometer, magnetometer) into the AHRS (Attitude and Heading Reference System) quaternion using the Mahony complementary filter.

## What It Does

1. Takes calibrated sensor readings (gyro/accel/mag)
2. Calls the Mahony filter to fuse sensor data
3. Updates the global AHRS quaternion (`ahrs.q`)
4. Provides accurate orientation (roll, pitch, yaw) at high update rates

## Function Signature

```cpp
namespace math {
  void updateOrientation(const Vec3& gyro_rad_s, const Vec3& accel_mss, const Vec3& mag, float dt);
}
```

**Parameters:**
- `gyro_rad_s` - Angular velocity in rad/s (from calibrated gyroscope)
- `accel_mss` - Linear acceleration in m/s² (from calibrated accelerometer, or normalized)
- `mag` - Magnetic field (any units, should be normalized)
- `dt` - Delta time in seconds since last update

## Basic Usage

```cpp
#include "./math/orientation.h"

void loop() {
  static uint32_t lastMicros = micros();
  uint32_t now = micros();
  float dt = (now - lastMicros) / 1e6f;
  lastMicros = now;

  // Read calibrated sensor data
  imu::Vec3f gyroRead, accelRead, magRead;
  imu::readGyro_rad_s(gyroRead);
  imu::readAccel_g(accelRead);
  imu::readMag(magRead);  // Already calibrated

  // Update orientation
  math::updateOrientation(gyroRead, accelRead, magRead, dt);

  // Extract Euler angles
  Euler attitude = quatToEulerRad(ahrs.q);
  float roll_deg = attitude.roll * 180.0f / M_PI;
  float pitch_deg = attitude.pitch * 180.0f / M_PI;
  float yaw_deg = headingDegFromYawRad(attitude.yaw, 0.0f);

  Serial.print("Roll: ");
  Serial.print(roll_deg);
  Serial.print(" Pitch: ");
  Serial.print(pitch_deg);
  Serial.print(" Heading: ");
  Serial.println(yaw_deg);

  delay(10);  // 100 Hz update rate
}
```

## Data Flow

```
Calibrated Gyro (rad/s)  \
Calibrated Accel (m/s²)   → updateOrientation() → Mahony Filter → AHRS Quaternion
Calibrated Mag (normalized)/                                    ↓
                                                    quatToEulerRad()
                                                         ↓
                                                   Roll, Pitch, Yaw
```

## How the Mahony Filter Works

The Mahony filter is a complementary filter that:

1. **Fast path (Gyroscope)**: Integrates angular velocity to predict orientation
   - High bandwidth, fast response
   - Drifts over time

2. **Slow path (Accelerometer + Magnetometer)**: Corrects for drift
   - Low bandwidth, stable over time
   - Slow to respond to motion

3. **Feedback**: Error between predicted and measured gravity/heading corrects gyro bias

**Tuning Parameters** (in main.cpp):
```cpp
MahonyParams params{ 
  .kp = 2.0f,      // Proportional gain (higher = faster correction, but more oscillation)
  .ki = 0.0f,      // Integral gain (for persistent gyro bias correction)
  .useMag = true   // Enable magnetometer for yaw correction
};
```

## Output: Euler Angles

After calling `updateOrientation()`, the global quaternion `ahrs.q` contains the orientation. Extract as Euler angles:

```cpp
Euler attitude = quatToEulerRad(ahrs.q);

// Access angles in radians
float roll_rad = attitude.roll;      // Rotation around X-axis
float pitch_rad = attitude.pitch;    // Rotation around Y-axis
float yaw_rad = attitude.yaw;        // Rotation around Z-axis

// Convert to degrees
float roll_deg = roll_rad * 180.0f / M_PI;
float pitch_deg = pitch_rad * 180.0f / M_PI;
float yaw_deg = yaw_rad * 180.0f / M_PI;
```

### Angle Definitions

- **Roll (φ)**: Rotation around X-axis (forward/back tilt)
  - Positive: Right wing up
  - Range: -π to π radians (-180° to 180°)

- **Pitch (θ)**: Rotation around Y-axis (up/down tilt)
  - Positive: Nose up
  - Range: -π/2 to π/2 radians (-90° to 90°)

- **Yaw (ψ)**: Rotation around Z-axis (heading)
  - Positive: Clockwise when viewed from above
  - Range: -π to π radians (-180° to 180°)

### Heading Conversion

Convert yaw (radians) to compass heading (degrees, 0-360°):

```cpp
float heading_deg = headingDegFromYawRad(attitude.yaw, 0.0f);
//                                                       ^^^
//                                         Magnetic declination in degrees
//                                         (optional, usually 0 unless you need
//                                          adjustment for your location)
```

**With magnetic declination:**
```cpp
// At New York: ~13° West declination
float heading_deg = headingDegFromYawRad(attitude.yaw, -13.0f);

// At London: ~2° West declination  
float heading_deg = headingDegFromYawRad(attitude.yaw, -2.0f);

// At Tokyo: ~7° East declination
float heading_deg = headingDegFromYawRad(attitude.yaw, 7.0f);
```

## Update Rate Considerations

**Recommended update rate: 100 Hz** (10 ms between updates)

```cpp
// At 100 Hz:
static uint32_t lastMicros = micros();
uint32_t now = micros();
float dt = (now - lastMicros) / 1e6f;  // ~0.01 seconds
lastMicros = now;

math::updateOrientation(gyro, accel, mag, dt);
```

**Effect of update rate:**
- **Too slow (<20 Hz)**: Poor heading tracking, sluggish response
- **Too fast (>200 Hz)**: Minimal benefit, wastes CPU
- **Variable dt**: Filter handles it, but constant dt is slightly better

## Integration with Logging

Log the Euler angles along with sensor data:

```cpp
logging::LogEntry entry{
  .timestamp_ms = millis(),
  .mag_raw = magRaw,
  .accel_raw = accelRaw,
  .gyro_raw = gyroRaw,
  .mag_cal = magRead,
  .accel_cal = accelRead,
  .gyro_cal = gyroRead,
  .heading_deg = heading_deg,    // From Mahony filter
  .roll_deg = roll_deg,          // From Mahony filter
  .pitch_deg = pitch_deg         // From Mahony filter
};

logging::logEntry(entry);
```

## Troubleshooting

### Issue: Heading drifts over time
**Cause**: Magnetometer not calibrated or disabled
**Fix**: 
- Run magnetometer calibration
- Ensure `params.useMag = true`
- Check mag calibration offsets are correct

### Issue: Large oscillations in roll/pitch
**Cause**: Mahony gain (kp) too high
**Fix**: Reduce `params.kp` (try 1.0 instead of 2.0)

### Issue: Slow response to motion
**Cause**: Mahony gain (kp) too low
**Fix**: Increase `params.kp` (try 3.0 instead of 2.0)

### Issue: Roll/pitch unstable when stationary
**Cause**: Accelerometer has high noise or axis is being moved
**Fix**: 
- Re-calibrate accelerometer
- Ensure device is on stable surface
- Reduce magnetometer weight if mag is noisy

### Issue: Yaw doesn't respond to rotation
**Cause**: Magnetometer not calibrated or magnetometer disabled
**Fix**:
- Ensure `params.useMag = true`
- Run magnetometer calibration
- Check for local magnetic interference (magnets, metal objects)

## Advanced: Quaternion Access

For advanced applications, you can directly access the quaternion:

```cpp
// Global AHRS state (declared in main.cpp, extern in orientation.cpp)
extern MahonyState ahrs;

// Quaternion components (w=scalar, x/y/z=vector)
float qw = ahrs.q.w;
float qx = ahrs.q.x;
float qy = ahrs.q.y;
float qz = ahrs.q.z;

// Rotate a body-frame vector to world-frame:
Vec3 bodyVector = {1, 0, 0};
Vec3 worldVector = quatRotate(ahrs.q, bodyVector);
```

## Future Enhancements

1. **Adaptive Gains**: Adjust Mahony gains based on sensor noise estimates
2. **Gyro Bias Estimation**: Use integral feedback (ki > 0) to track and correct gyro drift
3. **Magnetometer Rejection**: Disable mag updates when heading not trusted
4. **Outdoor GPS Integration**: Fuse GPS heading for outdoor navigation
5. **Motion Detection**: Switch to gyro-only mode during high acceleration
