# Accelerometer and Gyroscope Calibration

## Overview

The DPV-Nav system now includes automated calibration for both the accelerometer and gyroscope. These functions output calibrated data in real physical units:
- **Accelerometer**: m/s² (meters per second squared)
- **Gyroscope**: rad/s (radians per second)

## Gyroscope Calibration

### What It Does

Captures gyroscope bias (zero-rate offset) when the device is at rest. This offset is subtracted from all subsequent gyroscope readings.

### How It Works

```
1. Place device on stable, level surface
2. Keep completely still (no movement or rotation)
3. Sample for 10 seconds (default, configurable)
4. Average all samples to get bias offset
5. Scale is set to 1.0 (no scaling for gyro currently)
```

### Calibration Command

```cpp
#include "./sensors/imu.h"

Calib3 gyroCal;
imu::calibrateGyroscope(gyroCal, 10000);  // 10 second calibration
```

**Parameters:**
- `gyroCal` - Output struct for calibration data
- `duration_ms` - Sampling duration (default: 10000 = 10 seconds)

### Serial Output Example

```
[CAL] ========================================
[CAL] GYROSCOPE CALIBRATION
[CAL] ========================================
[CAL] Place device on stable, level surface
[CAL] Do NOT move or rotate the device
[CAL] Starting in 2 seconds... Go!
[CAL] Gyroscope calibration complete!
[CAL] Samples collected: 1000
[CAL] Gyro bias (raw counts): X=2.5 Y=-1.3 Z=0.8
[CAL] Gyro bias (rad/s): X=0.000038 Y=-0.000020 Z=0.000012
```

### Typical Bias Values

- **Good**: ±10 raw counts (±0.0002 rad/s)
- **Acceptable**: ±50 raw counts (±0.0008 rad/s)
- **Bad**: >200 raw counts (device was moved during calibration)

## Accelerometer Calibration

### What It Does

Captures accelerometer bias and scale using a 6-point orientation sequence. The device is oriented in each cardinal direction (±X, ±Y, ±Z) to experience gravity along each axis. This allows calculation of per-axis bias and scale factors.

Output is in m/s² units.

### How It Works

For each of 6 orientations:

```
1. Print instruction (e.g., "Place with X+ axis UP")
2. Wait 1-2 seconds for user to position device
3. Sample for 1-2 seconds
4. Store average reading
```

After all 6 orientations:

```
5. Calculate bias = (max + min) / 2 for each axis
6. Calculate scale = 9.81 / (max - bias) for each axis
```

The scale factor converts raw counts to m/s² (where gravity = 9.81 m/s²).

### Calibration Command

```cpp
#include "./sensors/imu.h"

Calib3 accelCal;
imu::calibrateAccelerometer(accelCal, 1500);  // 1.5 sec per orientation
```

**Parameters:**
- `accelCal` - Output struct for calibration data
- `sample_duration_ms` - Sampling duration per orientation (default: 1500 = 1.5 seconds)

### Orientation Sequence

The calibration prompts for these 6 orientations in order:

1. **X+ (right)** - Right side of device pointing up
2. **X- (left)** - Left side of device pointing up
3. **Y+ (forward)** - Front of device pointing up
4. **Y- (back)** - Back of device pointing up
5. **Z+ (up)** - Top of device pointing up
6. **Z- (down)** - Bottom of device pointing up

### Serial Output Example

```
[CAL] ========================================
[CAL] ACCELEROMETER CALIBRATION
[CAL] ========================================
[CAL] You will be asked to orient the device in 6 directions.
[CAL] For each direction: place device, wait for sampling, then proceed.

[CAL] ----------------------------------------
[CAL] Orientation 1 of 6: X+ (right)
[CAL] Place device with this axis UP, then press any key or wait 1 second... Sampling!
[CAL] Samples: 150 | Raw avg: X=11850 Y=45 Z=15

[CAL] ----------------------------------------
[CAL] Orientation 2 of 6: X- (left)
[CAL] Place device with this axis UP, then press any key or wait 1 second... Sampling!
[CAL] Samples: 150 | Raw avg: X=-11756 Y=32 Z=-22

... (orientations 3-6) ...

[CAL] ========================================
[CAL] ACCELEROMETER CALIBRATION COMPLETE
[CAL] ========================================
[CAL] Bias (raw counts): X=47.0 Y=38.5 Z=-3.5
[CAL] Bias (m/s²): X=0.003764 Y=0.003086 Z=-0.000281
[CAL] Scale: X=0.998765 Y=1.001234 Z=0.999456
```

### Typical Values

**Bias:**
- Good: ±50 raw counts (±0.004 m/s²)
- Acceptable: ±200 raw counts (±0.016 m/s²)

**Scale (for each axis):**
- Expected range: 0.95 - 1.05
- Good: 0.98 - 1.02
- If way off (e.g., 1.2): Possible sensor damage or axis confusion

## Using Calibrated Data

### Gyroscope

```cpp
Calib3 gyroCal{{0,0,0},{1,1,1}};

// Load or generate calibration
imu::calibrateGyroscope(gyroCal, 10000);

// Read calibrated gyroscope data (rad/s)
imu::Vec3f gyroRaw;
imu::readGyro_rad_s(gyroRaw);

// Apply calibration bias subtraction
imu::Vec3f gyroCal_result = applyCalib(gyroRaw, gyroCal);
```

### Accelerometer

```cpp
Calib3 accelCal{{0,0,0},{1,1,1}};

// Load or generate calibration
imu::calibrateAccelerometer(accelCal, 1500);

// Read raw accelerometer data
imu::Vec3i16 accelRaw;
imu::readAccelRaw(accelRaw);

// Convert to float and apply calibration
imu::Vec3f accelFloat = {(float)accelRaw.x, (float)accelRaw.y, (float)accelRaw.z};
imu::Vec3f accelCal_result = applyCalib(accelFloat, accelCal);
// Result is now in m/s² (with gravity = 9.81 m/s²)
```

## Integration with Persistence

Use the storage module to save/load calibrations:

```cpp
#include "./util/storage.h"

Calib3 gyroCal, accelCal;

// Load from flash, or calibrate if not found
if (!storage::loadCalib3("gyro_cal.json", gyroCal)) {
  imu::calibrateGyroscope(gyroCal);
  storage::saveCalib3("gyro_cal.json", gyroCal);
}

if (!storage::loadCalib3("accel_cal.json", accelCal)) {
  imu::calibrateAccelerometer(accelCal);
  storage::saveCalib3("accel_cal.json", accelCal);
}
```

## Calibration Tips

### Gyroscope Calibration

✅ **Do:**
- Place device on a solid, flat surface
- Ensure complete stillness (no vibration)
- Calibrate before first use or after temperature change
- Repeat in quiet environment without vibration sources

❌ **Don't:**
- Move the device during sampling
- Place on vibrating surface (near speakers, motors)
- Calibrate while device is warming up

### Accelerometer Calibration

✅ **Do:**
- Hold device steady in each orientation for 1-2 seconds
- Use a level surface or reference
- Ensure axis understanding (label your device beforehand)
- Keep sampling duration at least 1 second for good averaging

❌ **Don't:**
- Rotate device while sampling an orientation
- Confuse axes - label X, Y, Z clearly beforehand
- Sample too quickly (less than 1 second)
- Calibrate in high-vibration environment

### Both

✅ **Do:**
- Calibrate at the operating location if possible
- Save calibration to flash (won't lose on power loss)
- Document calibration date/location
- Re-calibrate after physical impact or temperature extreme

❌ **Don't:**
- Skip calibration (output will be inaccurate)
- Reuse old calibrations across hardware revisions
- Ignore warning messages during calibration
- Leave device unattended during multi-minute calibration

## Troubleshooting

### Issue: Gyro bias values are very large (>500 raw counts)
**Cause**: Device was moved or on vibrating surface
**Fix**: Repeat calibration with device on stable surface, completely still

### Issue: Accel scale values way off (e.g., 1.3 or 0.6)
**Cause**: Incorrect axis orientation or sensor damaged
**Fix**: Double-check axis mapping in code, verify sensor is functional

### Issue: Can't remember which direction is which
**Cause**: Device orientation not labeled
**Fix**: Before calibration, physically label the device: "X→", "←X", "Y↑", "↓Y", "Z⊙", "⊗Z"

### Issue: Accel calibration gives different values each time
**Cause**: Sampling too fast or environment too noisy
**Fix**: Increase sample_duration_ms (e.g., 2000 instead of 1500)

## Advanced: Manual Calibration Values

You can manually set calibration if you've captured values elsewhere:

```cpp
Calib3 gyroCal{
  .bias = {2.5, -1.3, 0.8},      // raw counts at zero rotation
  .scale = {1.0, 1.0, 1.0}       // no scaling for gyro
};

Calib3 accelCal{
  .bias = {47.0, 38.5, -3.5},    // raw counts at zero acceleration
  .scale = {0.9988, 1.0012, 0.9995}  // scale to m/s²
};

storage::saveCalib3("gyro_cal.json", gyroCal);
storage::saveCalib3("accel_cal.json", accelCal);
```

## Future Enhancements

1. **Temperature Compensation** - Adjust bias based on sensor temperature
2. **In-motion Calibration** - Refine calibration during normal operation
3. **Validation** - Check calibration quality and warn if poor
4. **Interactive Calibration** - Visual guidance on which orientation next
5. **Per-axis Selection** - Calibrate only needed axes
