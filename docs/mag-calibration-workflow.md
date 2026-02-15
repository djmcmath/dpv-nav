# Magnetometer Calibration Workflow

This guide describes the end-to-end workflow for performing soft-iron magnetometer calibration for the DPV-Nav system.

## Overview

Proper magnetometer calibration requires:
1. **Hard-iron calibration** - Removes constant magnetic offset (nearby ferromagnetic materials)
2. **Soft-iron calibration** - Corrects for directional distortion (magnetically permeable materials)

The simple min/max calibration only handles hard-iron. For ±5° heading accuracy on a DPV with variable magnetic signature, you need soft-iron calibration using ellipsoid fitting.

## Calibration Workflow

### 1. Data Collection (ESP32)

Mount the magnetometer on the DPV and collect raw samples while rotating through all orientations.

**Add to `main.cpp`:**
```cpp
#include "./util/mag_cal_collect.h"

// In setup():
void setup() {
  // ... existing init code ...

  Serial.println("Commands:");
  Serial.println("  'start_cal' - Start mag calibration data collection (30 sec)");
  Serial.println("  'dump_cal'  - Dump collected data to serial");
  Serial.println("  'clear_cal' - Clear calibration data");
}

// In loop():
void loop() {
  // Check for serial commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd == "start_cal") {
      mag_cal::startCollection(30000);  // 30 seconds
    } else if (cmd == "dump_cal") {
      mag_cal::dumpToSerial();
    } else if (cmd == "clear_cal") {
      mag_cal::clearData();
    }
  }

  // If collecting, log sample at high rate
  if (mag_cal::isCollecting()) {
    mag_cal::logSample();
    delay(10);  // ~100 Hz sampling
    return;  // Skip normal processing during calibration
  }

  // ... existing main loop code ...
}
```

**Collection procedure:**
1. Connect ESP32 via serial terminal
2. Type `start_cal` and press Enter
3. **Immediately start rotating the DPV:**
   - Drive in large circles (left and right)
   - Tilt nose up and down while circling
   - Roll left and right while circling
   - Goal: Trace a sphere in 3D space with the magnetometer
4. Continue for full 30 seconds
5. System will auto-stop and save data to SPIFFS

**Important:** The more complete your rotation coverage, the better the calibration. Try to hit all possible orientations during the run.

### 2. Data Export (ESP32 → PC)

Dump the collected data from SPIFFS to serial terminal:

1. In serial terminal, type `dump_cal` and press Enter
2. System will print CSV data between `--- START DATA ---` and `--- END DATA ---` markers
3. **Copy all lines between markers** (including header: `X,Y,Z`)
4. Paste into a text file on your PC: `mag_samples.csv`

**Verify data quality:**
- Should have ~3000 samples (30 sec × 100 Hz)
- X, Y, Z values should vary widely (covering full rotation range)
- If samples are clustered (not much variation), re-run with better rotation coverage

### 3. Ellipsoid Fitting (Python on PC)

Run the Python calibration script to compute soft-iron matrix:

```bash
cd d:\Documents\dpv-nav\tools
python mag_calibration.py mag_samples.csv
```

**Requirements:** `numpy`, `scipy`
```bash
pip install numpy scipy
```

**Output:**
```
MAGNETOMETER CALIBRATION RESULTS
====================================================================

Hard-Iron Offset (bias):
  X:  1600.50
  Y: -2782.50
  Z:  6671.00

Soft-Iron Correction Matrix:
  [ 0.950000,  0.012000, -0.008000]
  [ 0.012000,  1.020000,  0.005000]
  [-0.008000,  0.005000,  1.030000]

Diagnostics:
  Number of samples: 3000
  Ellipsoid radii: [57.2, 59.8, 60.1]
  Average radius: 59.0
  Radii variation: 2.15%
```

**Quality check:**
- Radii variation should be <10% (ideally <5%)
- If >10%, rotation coverage was poor - recollect data
- Soft-iron matrix should be close to identity (diagonal ~1.0, off-diagonal ~0.0)
- Large deviations (>0.2 on diagonals) suggest strong magnetic distortion from DPV

Script saves `calib_mag_cal.json` with both bias and soft-iron matrix.

### 4. Upload Calibration (PC → ESP32)

**Option A: Via SPIFFS Upload (Recommended for field use)**

Using Arduino IDE or PlatformIO SPIFFS uploader:
1. Place `calib_mag_cal.json` in `data/` folder
2. Upload SPIFFS image to ESP32
3. File will be at `/calib_mag_cal.json` on SPIFFS
4. Reboot ESP32 - `storage::loadMagCalibration()` will load it automatically

**Option B: Hardcode in `main.cpp` (Quick bench test)**

Copy values from JSON into `main.cpp`:
```cpp
MagCalib magCal{
  {1600.5f, -2782.5f, 6671.0f},  // bias
  {
    { 0.950000f,  0.012000f, -0.008000f},  // softIron row 0
    { 0.012000f,  1.020000f,  0.005000f},  // softIron row 1
    {-0.008000f,  0.005000f,  1.030000f}   // softIron row 2
  }
};
```
Recompile and upload firmware.

**Option C: Serial Upload (Future enhancement)**

Add serial command to receive JSON over serial and save to SPIFFS. Not yet implemented.

### 5. Verification

After loading new calibration:

1. Place device in known orientation (e.g., pointing magnetic north)
2. Read heading from serial output
3. Rotate device 90° clockwise (east), verify heading increases by ~90°
4. Rotate device 90° counter-clockwise (west), verify heading decreases by ~90°
5. Repeat at different orientations

**Expected accuracy:** ±5° heading error across all orientations if calibration is good.

**If accuracy is poor:**
- Check that soft-iron matrix was loaded correctly (print at startup)
- Verify rotation coverage was complete during data collection
- Ensure DPV magnetic signature hasn't changed (new equipment, battery position, etc.)
- Re-run calibration with more samples and better rotation coverage

## Troubleshooting

### "Radii variation: 15%" - Poor calibration quality

**Cause:** Incomplete rotation during data collection. Magnetometer didn't sample all orientations evenly.

**Fix:** Re-run calibration with emphasis on:
- Smooth, continuous rotation (don't jerk or stop)
- Equal time in all orientations (don't favor certain angles)
- Full pitch range (nose up/down)
- Full roll range (left/right tilt)
- Full yaw range (complete circles)

### Heading still drifts after calibration

**Possible causes:**
1. **Magnetic environment changed** - Calibration is specific to the magnetic signature at time of collection. If you add equipment or rearrange DPV, recalibrate.
2. **Mahony gain too high** - Lower `kp` in `MahonyParams` (try 0.01-0.05)
3. **Sensor noise** - Cheap sensors have inherent noise. Add low-pass filter to heading output.
4. **Dynamic magnetic fields** - Motors, servos, or electrical noise can create time-varying fields that calibration can't fix

### Data dump shows very few samples

**Cause:** Collection stopped early or logging failed.

**Fix:**
- Verify SPIFFS is mounted and has free space
- Check serial terminal for error messages during collection
- Increase `delay()` in log loop if I2C bus is saturated

## Advanced: Multi-Environment Calibration

For DPVs used in different configurations (different gear mounted), you may need multiple calibration profiles:

1. Collect data in each configuration (e.g., "light gear", "heavy gear", "sidemount tanks")
2. Run Python script for each: `mag_calibration.py mag_samples_light.csv`
3. Save each as separate JSON: `calib_mag_cal_light.json`, `calib_mag_cal_heavy.json`
4. Add serial command to switch between profiles at runtime
5. Load appropriate calibration based on current configuration

## Theory: Why Ellipsoid Fitting?

The magnetometer measures Earth's magnetic field vector, which has constant magnitude (~50-60 µT depending on location). If you rotate the sensor through all orientations, the raw readings should form a **sphere** centered at origin.

However, magnetic distortions cause:
- **Hard-iron:** Sphere shifts away from origin (constant offset)
- **Soft-iron:** Sphere deforms into ellipsoid (directional scaling/rotation)

Simple min/max calibration only fixes hard-iron (centers the ellipsoid) but doesn't reshape it into a sphere.

**Ellipsoid fitting:**
1. Fits ellipsoid equation to raw data points
2. Extracts center (hard-iron offset)
3. Extracts principal axes and radii (soft-iron distortion)
4. Computes transformation matrix to reshape ellipsoid → sphere

The soft-iron matrix applies this transformation to all future readings, giving you a calibrated sphere that provides accurate heading in all orientations.

## References

- Freescale AN4246: "Calibrating an eCompass in the Presence of Hard and Soft-Iron Interference"
- Magneto calibration tool: https://github.com/psiphi75/magneto
- ST AN4509: "Ellipsoid fitting for magnetometer calibration"
