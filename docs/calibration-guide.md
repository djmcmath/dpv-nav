# Sensor Calibration Guide

## Overview

DPV-Nav calibrates four sensors/subsystems: magnetometer, gyroscope, accelerometer, and heading. Calibration data is persisted to LittleFS as JSON files on the nav device. On boot, saved calibration loads in under a second; if files are missing, blocking calibration runs automatically (except for heading, which is optional).

## Calibration Files

```
/mag_base.json      — Baseline magnetometer (hard-iron + soft-iron 3×3), off-scooter cal
/mag_mount.json     — Mounted magnetometer correction (hard-iron + soft-iron 3×3), on-scooter
/mag_cal.json       — Legacy single-stage mag cal (loaded as fallback if base/mount absent)
/gyro_cal.json      — Gyroscope bias
/accel_cal.json     — Accelerometer bias + scale
/hdg_cal.json       — 4-point heading calibration (optional)
/speed_cal.json     — Flow sensor k-factor history (rolling 6-run average)
```

### JSON Formats

**MagCalib** (base and mount files):
```json
{
  "bias": { "x": 45.2, "y": -28.7, "z": 15.4 },
  "softIron": [
    [1.02, 0.01, -0.003],
    [0.01, 0.98, 0.002],
    [-0.003, 0.002, 1.01]
  ]
}
```

**Calib3** (gyro/accel):
```json
{
  "bias": { "x": 0.1, "y": -0.05, "z": 0.03 },
  "scale": { "x": 1.0, "y": 1.0, "z": 1.0 }
}
```

**HdgCal** (4-point heading):
```json
{ "h0": 2.3, "h1": 87.6, "h2": 183.1, "h3": 268.9 }
```
`h0`–`h3` are the indicated (AHRS-reported) headings when aligned to N/E/S/W respectively.

## Boot Sequence

```
For mag: try two-stage chain (mag_base.json + mag_mount.json)
          → fall back to legacy mag_cal.json
          → fall back to blocking 90s min/max sweep (last resort)
For gyro: load gyro_cal.json or run 10s stationary bias cal
For accel: load accel_cal.json or run 6-point orientation cal
For hdg: load hdg_cal.json — silently skip if absent (optional)
```

## Magnetometer Calibration

### Two-stage calibration chain

The preferred calibration workflow uses two stages, each producing a JSON file that is the result of offline ellipsoid fitting (see [mag-calibration-workflow.md](mag-calibration-workflow.md)):

| Stage | Menu item | File | When to do |
|-------|-----------|------|-----------|
| Baseline | CAL > Baseline | `/mag_base.json` | First time, or after hardware change. Device off the DPV. Covers full sphere. |
| Mounted | CAL > Mounted | `/mag_mount.json` | After baseline, with device installed on DPV. Corrects for DPV magnetic signature. |

Both calibrations collect raw samples using a **bin-aware** approach: the screen shows a heading × elevation grid, and bins fill green as adequate samples are collected. When all required bins are green, the calibration is complete and the sample CSV is saved to LittleFS for offline processing.

### Running Baseline Calibration

1. **Remove device from DPV** and take it somewhere with low magnetic interference.
2. Open menu (BTN1) → **CAL > Baseline**.
3. The display switches to a bin-coverage grid screen. Rotate the device through all orientations — roll, pitch, yaw, and diagonals.
4. Fill all bins green (the grid fills from left to right; level row must be 100%, tilted rows ~90%).
5. When all required bins are green the display shows "DONE" briefly, then CSV data is saved to `/mag_baseline_samples.csv` on LittleFS.
6. Export the CSV and run the Python offline fitting tool: `python tools/mag_calibration.py mag_baseline_samples.csv`
7. Upload the generated `mag_base.json` to LittleFS.

### Running Mounted Calibration

1. **Mount device on DPV** in its normal installed position (all motors, batteries, and other magnetic sources present and at operational distance).
2. Open menu (BTN1) → **CAL > Mounted**.
3. Rotate the DPV through heading and tilt orientations. DPV operation is primarily horizontal so the grid only requires 3 elevation bands.
4. Fill all required bins green, then export CSV (`/mag_mounted_samples.csv`) and run:
   `python tools/mag_calibration.py mag_baseline_samples.csv mag_mounted_samples.csv`
5. Upload the generated `mag_mount.json` to LittleFS.

### What each calibration corrects

| Distortion | Baseline | Mounted |
|-----------|---------|---------|
| Hard-iron bias (sensor, PCB) | ✓ | Refined |
| Soft-iron coupling (PCB, enclosure) | ✓ | Refined |
| DPV motor/battery magnetic signature | — | ✓ |

### Forcing re-calibration

Delete the corresponding JSON file(s) from LittleFS and reboot, or run the menu cal workflow to generate new sample CSVs.

## Gyroscope Calibration

**Method:** Stationary bias sampling (runs at boot if no file found)

**Procedure:**
1. Place device on a stable, level surface
2. Keep completely still — no movement or vibration
3. System samples for 10 seconds at ~100 Hz
4. Average of all samples = bias offset; scale remains 1.0

**Typical bias values:**
- Good: ±10 raw counts (±0.0002 rad/s)
- Acceptable: ±50 raw counts (±0.0008 rad/s)
- Bad (device moved): >200 raw counts

## Accelerometer Calibration

**Method:** 6-point orientation sequence (runs at boot if no file found)

**Procedure:**
1. System prompts for 6 orientations: X+, X-, Y+, Y-, Z+, Z- facing down
2. Hold device steady in each orientation for 2.5 seconds while it samples
3. System computes: `bias = (max + min) / 2`, `scale = 9.81 / (max - bias)` per axis

**Typical values:**
- Bias: ±50 raw counts (±0.004 m/s²)
- Scale: 0.98 – 1.02 per axis

## 4-Point Heading Calibration

**Purpose:** After magnetometer calibration, small systematic heading errors typically remain (2–8°). The 4-point heading cal measures the actual error at each cardinal heading and applies circular linear interpolation at runtime to reduce residual error to <2°.

**When to do it:** After completing baseline + mounted mag cal. The device must already have a working magnetometer calibration before heading cal is meaningful.

**Trigger:** Menu → **CAL > Hdg cal**

### Procedure

1. Open menu (BTN1) → **CAL > Hdg cal**. The menu closes and the heading cal screen appears.
2. The screen prompts for Step 1/4: **NORTH (0°)**.
   - The live AHRS heading (pre-correction, raw) is shown in large text so you can watch it stabilise.
3. Align the DPV to true North. Wait for the heading readout to stop changing.
4. Press **BTN2** to capture that heading.
5. The screen advances to Step 2/4: **EAST (90°)**. Align to East, wait for stable reading, press BTN2.
6. Repeat for Step 3/4 **SOUTH (180°)** and Step 4/4 **WEST (270°)**.
7. After the 4th capture, the calibration is automatically sent to the nav device and saved to `/hdg_cal.json`. A summary screen appears:

```
HDG CAL DONE
─────────────────
N: +2.3°
E: -1.8°
S: +2.1°
W: -1.5°
─────────────────
GREAT (2.3° max)
Consistent dir
```

8. Press **BTN2** to dismiss the summary and return to normal navigation.

### Summary screen interpretation

**Max error rating:**

| Max error | Rating |
|-----------|--------|
| < 3° | GREAT |
| < 6° | GOOD |
| < 10° | FAIR |
| ≥ 10° | POOR |

**Direction consistency:**
- **Consistent dir** (all corrections same sign, e.g. all positive) — indicates a systematic offset, which is the most correctable kind of error.
- **Mixed direction** (corrections vary in sign) — indicates more complex distortion; double-check mag calibration quality.

A POOR or mixed-direction result suggests the magnetometer calibration should be redone before relying on heading cal.

### How it works at runtime

The nav device stores the 4 `(indicated, correction)` pairs. At each heading computation:
1. The raw AHRS heading is computed
2. The correction table is queried using the raw heading as a lookup key (circular linear interpolation between the four points)
3. The corrected heading is used for dead-reckoning and sent in NavPacket

The raw heading is also sent separately as `heading_raw_deg` in NavPacket so the display device can capture it correctly during recalibration (the display shows `heading_raw_deg` during the cal prompts).

### Recalibrating

Simply run **CAL > Hdg cal** again. The new calibration overwrites `/hdg_cal.json` on completion.

## Storage API

```cpp
#include "util/storage.h"

// Magnetometer
storage::saveMagCalibration("/mag_base.json", magCal);
storage::loadMagCalibration("/mag_base.json", magCal);

// Gyro / Accel
storage::saveCalib3("/gyro_cal.json", gyroCal);
storage::loadCalib3("/gyro_cal.json", gyroCal);

// Heading cal
#include "util/hdg_cal.h"
hdg_cal::load(gHdgCal);           // returns bool
hdg_cal::save(gHdgCal);           // returns bool
hdg_cal::apply(headingDeg, gHdgCal);  // returns corrected heading
```

Files are stored on LittleFS (ESP32 internal flash). Total calibration data is <5 KB.

## Troubleshooting

| Problem | Cause | Fix |
|---------|-------|-----|
| Heading off by consistent amount | Systematic hard-iron bias | Run mounted mag cal or 4-point heading cal |
| Heading varies by orientation | Soft-iron distortion | Run full mounted cal + offline ellipsoid fit |
| Heading cal shows POOR | Mag cal has large residual errors | Redo mag cal before heading cal |
| Heading cal shows Mixed direction | Complex distortion pattern | Verify DPV has no moving magnetic sources; redo mounted cal |
| Gyro bias very large (>500) | Device moved during cal | Repeat on stable surface, no vibration |
| Accel scale way off (>1.1) | Wrong orientation or sensor issue | Verify axis labeling, check sensor |
| "LittleFS mount failed" | Flash partition not configured | Check `board_build.filesystem = littlefs` in platformio.ini |

## Speed Calibration (Flow Sensor)

Speed cal measures the flow sensor's k-factor (pulses per litre) by timing a swim over a known distance. Results are averaged across up to 6 runs and saved to `/speed_cal.json` on LittleFS.

### How it works

The flow sensor converts pulse frequency to speed using:
```
speed_ms = (freq_hz / k_factor) / 60 / 1000 / cross_section_m2
```
Speed cal solves for `k_factor` given the true distance and measured total pulses:
```
k_factor = total_pulses / (dist_m × 60 × 1000 × FLOW_CROSS_SECTION_M2)
```
Elapsed time cancels out — only total pulse count and true distance matter.

### Calibration file

```
/speed_cal.json     — Rolling history of up to 6 k-factor measurements
```

Format:
```json
{ "n": 3, "k": [1.12, 1.09, 1.15] }
```
The active k-factor is the average of all stored values. On boot the nav device loads this file and applies the average. If the file is absent, `FLOW_K_FACTOR` from `config.h` is used.

### Running a speed cal

See **CAL > Speed cal** in the [User Guide](user-guide.md) for the full step-by-step workflow. In brief:

1. Menu → **CAL > Speed cal**
2. Select distance (150–500 ft, default 300 ft)
3. Start DPV — timing begins automatically at flow ≥ 0.3 m/s
4. Swim the distance; the run stops when flow drops or heading deviates > 90° after 30 s
5. Accept (add to average), reset+accept (start fresh), or reject

### Accept vs Reset+Accept

| Option | When to use |
|--------|-------------|
| **ACCEPT** | Normal run — adds to rolling average of up to 6 measurements |
| **RESET+ACCEPT** | DPV serviced, impeller changed, or first cal on a new installation — clears history so old measurements from different hardware don't pollute the average |
| **REJECT** | Run was suspect (aborted early, DPV speed changed mid-run, etc.) |

### Setting FLOW_CROSS_SECTION_M2

`FLOW_CROSS_SECTION_M2` in `config.h` must be set to the physical intake cross-section of the DPV inlet before speed cal can give sensible k-factor values. Measure the inner diameter of the flow sensor mounting tube:
```
area = π × (diameter/2)²
```
A 50 mm diameter tube → 0.00196 m² ≈ 0.002 m². This constant is not calibrated automatically.
