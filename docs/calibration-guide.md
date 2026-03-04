# Sensor Calibration Guide

## Overview

DPV-Nav calibrates three sensors: magnetometer, gyroscope, and accelerometer. Calibration data is persisted to LittleFS as JSON files, so calibration only runs on first boot (or when files are deleted). On subsequent boots, saved calibration loads in under a second.

## Calibration Files

```
/calib/mag_cal.json     — Magnetometer (hard-iron bias + soft-iron 3×3 matrix)
/calib/gyro_cal.json    — Gyroscope (bias + scale)
/calib/accel_cal.json   — Accelerometer (bias + scale)
```

### JSON Formats

**MagCalib:**
```json
{
  "type": "MagCalib",
  "bias": { "x": 45.2, "y": -28.7, "z": 15.4 },
  "softIron": [
    [1.0, 0.0, 0.0],
    [0.0, 1.0, 0.0],
    [0.0, 0.0, 1.0]
  ]
}
```

**Calib3 (gyro/accel):**
```json
{
  "type": "Calib3",
  "bias": { "x": 0.1, "y": -0.05, "z": 0.03 },
  "scale": { "x": 1.0, "y": 1.0, "z": 1.0 }
}
```

## Boot Sequence

The calibration load/run logic is in [nav_main.cpp](../src/nav_main.cpp):

```
For each sensor (mag, gyro, accel):
  1. Try loading from LittleFS
  2. If found → apply immediately, skip calibration
  3. If not found → run interactive calibration, save to LittleFS
```

## Magnetometer Calibration

**Method:** Min/max sweep (hard-iron only)

**Procedure:**
1. System prints: "Rotate device through all orientations for 30 seconds..."
2. Slowly rotate the device in all directions (figure-8 patterns work well)
3. Cover all axes: roll, pitch, yaw, and diagonal orientations
4. After 30 seconds, bias is computed as midpoint: `bias = (min + max) / 2`

**What it corrects:**
- Hard-iron distortions (fixed magnetic sources in/near device)
- Sensor manufacturing offset
- Local field variations

**What it does NOT correct:**
- Soft-iron distortions (cross-axis coupling from ferromagnetic materials) — see [mag-calibration-workflow.md](mag-calibration-workflow.md) for full soft-iron calibration

**Triggering recalibration:**
- Delete `/calib/mag_cal.json` from LittleFS and reboot
- Long-press BTN2 (2 seconds) on the display device

**Rotation tips:**
- Slow, deliberate rotation (~1 rotation per 2 seconds)
- Cover all 3 axes plus diagonals
- Keep away from ferrous objects during calibration
- Avoid fast jerky movements

### Calibrated Magnetometer Reading

After calibration, `imu::readMag(out)` returns fully calibrated data:

```
readMagRaw() → Vec3i16 (raw counts)
    → convert to float
    → subtract hard-iron bias
    → multiply by soft-iron 3×3 matrix
    → readMag() result (calibrated Vec3f)
```

If `setMagCalibration()` was never called, `readMag()` returns raw float conversion (bias=0, softIron=identity).

## Gyroscope Calibration

**Method:** Stationary bias sampling

**Procedure:**
1. Place device on a stable, level surface
2. Keep completely still — no movement or vibration
3. System samples for 10 seconds at ~100 Hz
4. Average of all samples = bias offset
5. Scale set to 1.0 (no scaling correction)

**Typical bias values:**
- Good: ±10 raw counts (±0.0002 rad/s)
- Acceptable: ±50 raw counts (±0.0008 rad/s)
- Bad (device moved): >200 raw counts

## Accelerometer Calibration

**Method:** 6-point orientation sequence

**Procedure:**
1. System prompts for 6 orientations in sequence: X+, X-, Y+, Y-, Z+, Z- pointing up
2. For each orientation: hold device steady for 2.5 seconds while system samples
3. System computes: `bias = (max + min) / 2`, `scale = 9.81 / (max - bias)` per axis

**Typical values:**
- Bias: ±50 raw counts (±0.004 m/s²)
- Scale: 0.98 – 1.02 per axis

## Storage API

```cpp
#include "util/storage.h"

// Magnetometer
storage::saveMagCalibration("/calib/mag_cal.json", magCal);  // returns bool
storage::loadMagCalibration("/calib/mag_cal.json", magCal);  // returns bool

// Gyro / Accel
storage::saveCalib3("/calib/gyro_cal.json", gyroCal);
storage::loadCalib3("/calib/gyro_cal.json", gyroCal);
```

Files are stored on LittleFS (ESP32 internal flash). Total calibration data is <1 KB.

## Forcing Recalibration

1. **Delete files from LittleFS** — reboot triggers fresh calibration
2. **BTN2 long press** — triggers mag recalibration from display device
3. **Serial commands** — use `LittleFS.remove("/calib/mag_cal.json")` etc.

## Troubleshooting

| Problem | Cause | Fix |
|---------|-------|-----|
| Heading drifts after cal | Soft-iron not corrected | Run full soft-iron workflow ([mag-calibration-workflow.md](mag-calibration-workflow.md)) |
| Very large mag offsets (>500) | Strong local magnetic source | Move away from motors/magnets during cal |
| Gyro bias very large (>500) | Device moved during cal | Repeat on stable surface, no vibration |
| Accel scale way off (>1.1) | Wrong orientation or sensor issue | Verify axis labeling, check sensor |
| Inconsistent cal values | Poor rotation coverage | Use figure-8 pattern, cover all axes |
| "LittleFS mount failed" | Flash partition not configured | Check `board_build.filesystem = littlefs` in platformio.ini |

## Speed Calibration (Flow Sensor)

Not automated. Requires field testing:
1. Mark a known distance on the bottom (100–200 m of pre-measured line)
2. Drive straight at typical cruising speed
3. Tune `FLOW_K_FACTOR` and `FLOW_CROSS_SECTION_M2` in [config.h](../src/config.h) until integrated distance matches GPS or known distance
