# Calibrated Magnetometer Reading

## Overview

A new `imu::readMag()` function provides magnetometer readings with automatic calibration applied. This function handles both hard-iron offsets (from the min/max calibration) and soft-iron matrix correction.

## Usage

### Setup (once, after calibration)

```cpp
// In main.cpp setup(), after running calibration:
imu::calibrateMagnetometer(magCal, 30000);   // 30-second calibration sweep
imu::setMagCalibration(magCal);              // Configure IMU module to use these offsets
```

### Reading Calibrated Data (in your loop)

```cpp
imu::Vec3f magCalibrated;

if (imu::readMag(magCalibrated) == imu::ImuStatus::Ok) {
  // magCalibrated is now the corrected magnetic field
  Serial.print("Mag X: ");
  Serial.print(magCalibrated.x);
  Serial.print(" Y: ");
  Serial.print(magCalibrated.y);
  Serial.print(" Z: ");
  Serial.println(magCalibrated.z);
}
```

## Function Details

### `void imu::setMagCalibration(const MagCalib& cal)`

Sets the calibration parameters to use for all subsequent `readMag()` calls.

**Parameters:**
- `cal` - MagCalib struct containing:
  - `bias` (Vec3f) - Hard-iron offset for each axis
  - `softIron` (3×3 matrix) - Soft-iron distortion correction matrix

**Example:**
```cpp
MagCalib myCal;
imu::calibrateMagnetometer(myCal, 30000);
imu::setMagCalibration(myCal);
```

### `ImuStatus imu::readMag(Vec3f& out)`

Reads the raw magnetometer and applies all calibration corrections.

**Parameters:**
- `out` (Vec3f&) - Output buffer for calibrated magnetic field

**Returns:**
- `ImuStatus::Ok` - Success
- `ImuStatus::NotInitialized` - Magnetometer not initialized
- `ImuStatus::BusError` - I2C communication error

**Process:**
1. Read raw magnetometer data (Vec3i16)
2. Convert to float
3. Apply hard-iron offset: `value - bias`
4. Apply soft-iron matrix: multiply by 3×3 correction matrix
5. Return calibrated Vec3f

**Example:**
```cpp
imu::Vec3f magCal;
if (imu::readMag(magCal) == imu::ImuStatus::Ok) {
  // Use magCal (calibrated data)
}
```

## What Gets Corrected

### Hard-Iron Correction (Bias Subtraction)
From the calibration process, each axis has a bias offset:
```
corrected.x = raw.x - bias.x
corrected.y = raw.y - bias.y
corrected.z = raw.z - bias.z
```

This removes fixed magnetic sources (magnetized metal, nearby magnets).

### Soft-Iron Correction (Matrix Multiplication)
The 3×3 matrix corrects for magnetic field distortions that vary with orientation:
```
[corrected]   [m00 m01 m02]   [hard_iron_corrected.x]
[corrected] = [m10 m11 m12] × [hard_iron_corrected.y]
[corrected]   [m20 m21 m22]   [hard_iron_corrected.z]
```

Currently set to identity (no soft-iron correction), but ready for future enhancement.

## Data Flow

```
readMagRaw()           → Vec3i16 (raw counts from sensor)
    ↓ (convert)
Vec3f (float counts)
    ↓ (subtract bias)
Hard-iron corrected
    ↓ (multiply by 3×3 matrix)
readMag() result → Vec3f (fully calibrated)
```

## Default Calibration

If `setMagCalibration()` is never called, the default is:
- **Bias**: (0, 0, 0) - no offset
- **Soft-Iron**: Identity matrix - no distortion correction

This means `readMag()` behaves like a raw float conversion until explicitly configured.

## Example: Complete Integration

```cpp
#include "./sensors/imu.h"
#include "./sensors/calib.h"

// Global calibration
MagCalib magCal{{0,0,0}, {{1,0,0},{0,1,0},{0,0,1}}};

void setup() {
  Serial.begin(115200);
  
  // Initialize sensors
  if (!imu::init(imuConfig, imuAxisMap)) {
    Serial.println("IMU init failed");
    while(1);
  }
  
  // Run calibration
  Serial.println("Rotate device for 30 seconds...");
  imu::calibrateMagnetometer(magCal, 30000);
  
  // Install calibration
  imu::setMagCalibration(magCal);
  
  Serial.print("Calibration offsets: X=");
  Serial.print(magCal.bias.x);
  Serial.print(" Y=");
  Serial.print(magCal.bias.y);
  Serial.print(" Z=");
  Serial.println(magCal.bias.z);
}

void loop() {
  imu::Vec3f magCal;
  
  if (imu::readMag(magCal) == imu::ImuStatus::Ok) {
    // Use calibrated data for heading calculation
    float headingRad = atan2(magCal.y, magCal.x);
    float headingDeg = headingRad * 180.0f / PI;
    
    Serial.print("Heading: ");
    Serial.print(headingDeg);
    Serial.println(" degrees");
  }
  
  delay(100);
}
```

## Comparison with Other Functions

| Function | Input | Output | Applies Calibration |
|----------|-------|--------|---------------------|
| `readMagRaw()` | - | Vec3i16 | ❌ Raw counts only |
| `readMag_uT()` | - | Vec3f | ❌ Unit conversion only |
| `readMag()` | - | Vec3f | ✅ Full calibration applied |
| `applyMagCalib()` (in calib.cpp) | Vec3f raw | Vec3f calibrated | ✅ Manual application |

## Thread Safety

The calibration is stored as a static global in imu.cpp. If `setMagCalibration()` is called from multiple tasks/threads, ensure proper synchronization (not critical for single-threaded ESP32 loop).

## Future Enhancements

1. **Persistence** - Save/load calibration from flash
2. **Dynamic Updates** - Update calibration based on in-motion statistics
3. **Per-axis Scales** - Add scale factors for ±X/Y/Z ranges
4. **Temperature Compensation** - Adjust bias based on sensor temperature
5. **Validation** - Check for inadequate calibration (warn if data looks wrong)
