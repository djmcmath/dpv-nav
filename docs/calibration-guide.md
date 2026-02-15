# Magnetometer Calibration Guide

## Overview

The DPV-Nav system now includes automatic magnetometer calibration during initialization. This calibration compensates for the local magnetic field environment and orientation-dependent offsets in the magnetometer.

## Calibration Method: Min/Max Sweep

The calibration uses a simple but effective **min/max sweep** approach:

1. **Sample Duration**: Device rotates through all orientations for 30 seconds
2. **Tracking**: For each axis, track the minimum and maximum raw readings
3. **Offset Calculation**: Bias = (min + max) / 2
4. **Result**: Stores bias offset in `MagCalib` struct

### Why This Works

- The magnetometer reads the vector sum of Earth's field + local distortions
- By sampling all orientations, you capture the full range of these distortions
- The midpoint of the range gives the best offset for centering the data
- This removes **hard-iron offsets** (static field distortions from nearby metal)

## Usage

### Automatic Calibration (Recommended)

The system automatically calibrates on startup:

```cpp
// In firmware/src/main.cpp setup():
imu::calibrateMagnetometer(magCal, 30000);  // 30 second calibration
```

**During calibration:**
1. Device will print: `[CAL] Rotate device through all orientations for 30 seconds...`
2. Slowly rotate the device in all directions (figure-8 patterns work well)
3. After 30 seconds, calibration completes and offsets are displayed
4. Example output:
   ```
   [CAL] Starting magnetometer calibration...
   [CAL] Rotate device through all orientations for 30 seconds...
   [CAL] Calibration complete!
   [CAL] Samples collected: 3000
   [CAL] Magnetic field offsets: X=45.2 Y=-32.1 Z=18.5
   [CAL] Field range - X:[-150, 240] Y:[-180, 90] Z:[-50, 130]
   ```

### Manual Calibration (For Testing)

You can call calibration anytime after IMU initialization:

```cpp
MagCalib newCal;
imu::calibrateMagnetometer(newCal, 15000);  // 15 second calibration
```

## What Gets Calibrated

### ✓ Corrected For:
- **Hard-iron distortions** - Fixed magnetic sources (magnets in device, nearby metal)
- **Local field variations** - Regional magnetic field anomalies
- **Temperature effects** - Bias shifts due to sensor temperature
- **Manufacturing offset** - Inherent sensor biases

### ✗ Not Corrected For (Future):
- **Soft-iron distortions** - Ferromagnetic materials that distort field (cross-axis coupling)
- **Inclination effects** - Depending on latitude (Earth's field tilts with latitude)
- **Time-varying fields** - Moving metal objects

## Calibration Quality Indicators

Good calibration produces:
- **Range spread**: 300-500 counts typical (larger range = better sensitivity)
- **Sample count**: ~3000 samples for 30 seconds @ ~100 Hz
- **Offset symmetry**: min/max should be relatively balanced around zero after offset applied
- **Reproducibility**: Re-running should give similar offsets (±5 counts)

**Warning signs:**
- Very small range (<100 counts) - weak/no rotation during calibration
- Asymmetric ranges (e.g., X: [-300, 50]) - rotation not thorough
- No change between runs - sensor may be stuck or not reading

## Rotation Technique

For best results:

1. **Hold device steady** - Avoid translational motion, only rotate
2. **Slow, deliberate rotation** - ~1 rotation per 2 seconds
3. **Cover all axes**: 
   - Roll (rotate around long axis)
   - Pitch (rotate forward/back)
   - Yaw (rotate left/right)
   - Diagonal (corner to corner through space)
4. **8-figure pattern**: Figure-8 patterns in 3D space are effective
5. **Keep away from metals**: Move away from ferrous objects during calibration

**Poor calibration technique:**
- ❌ Fast jerky movements
- ❌ Only rotating one axis
- ❌ Holding near metallic objects
- ❌ Calibrating in high EM noise area (near motors, WiFi routers)

## Using Calibration Results

After calibration, the `magCal` struct contains:

```cpp
MagCalib magCal;
// .bias = {X_offset, Y_offset, Z_offset}
// .softIron = identity matrix (3x3)
```

Apply calibration to raw readings:

```cpp
imu::Vec3f magRaw = {(float)raw.x, (float)raw.y, (float)raw.z};
imu::Vec3f magCalibrated = {
  magRaw.x - magCal.bias.x,
  magRaw.y - magCal.bias.y,
  magRaw.z - magCal.bias.z
};
```

Or use the helper function in [calib.cpp](../firmware/src/sensors/calib.cpp):

```cpp
#include "./sensors/calib.h"
imu::Vec3f magFloat = {(float)magRawRead.x, (float)magRawRead.y, (float)magRawRead.z};
imu::Vec3f magCal_out = applyMagCalib(magFloat, magCal);
```

## Saving Calibration

Currently, calibration is recomputed on every startup. For production use, save to flash:

```cpp
// TODO: Implement in storage.cpp
void saveCalibration(const MagCalib& cal);
void loadCalibration(MagCalib& cal);
```

Then in setup():
```cpp
MagCalib magCal;
if (!loadCalibration(magCal)) {
  // First time or corrupted: do fresh calibration
  imu::calibrateMagnetometer(magCal, 30000);
  saveCalibration(magCal);
}
```

## Troubleshooting

### Issue: Inconsistent Calibration Values
**Cause**: Rotation pattern incomplete
**Fix**: Ensure figure-8 pattern that covers all orientations

### Issue: Very Large Offsets (>500 counts)
**Cause**: Strong local magnetic distortion (magnetized object, electric motor nearby)
**Fix**: 
- Move away from magnetic sources
- Check for stray magnets in housing
- Verify sensor isn't magnetized from ESD

### Issue: Calibration Never Completes
**Cause**: Magnetometer not initialized
**Fix**: Ensure `imu::init()` completes successfully before calling `calibrateMagnetometer()`

### Issue: Heading Still Drifts After Calibration
**Cause**: Soft-iron distortions not corrected
**Fix**: 
- Implementation of soft-iron matrix correction (future feature)
- Manual rotation adjustment (currently must be done in code)

## Next Steps

1. **Implement persistence**: Save/load calibration from flash
2. **Soft-iron correction**: Compute full 3×3 correction matrix
3. **Automated detection**: Detect inadequate rotation and prompt user
4. **Multi-location calibration**: Store multiple calibrations for different environments
5. **Gyroscope calibration**: Implement similar min/max for gyro bias


2 - Speed calibration

- Shallow runs with known distance marked on the bottom.  I prefer a run of pre-measured line. 

- Drive 100–200 m straight at typical cruising speed.

- Tune flow_factor such that integrated distance matches GPS.
