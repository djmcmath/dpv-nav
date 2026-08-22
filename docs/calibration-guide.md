# Sensor Calibration Guide

> **Planned:** magnetometer calibration (Baseline/Mounted) is planned to move to an
> over-WiFi round-trip with Dive Map instead of the manual laptop-script step described
> below — see [cloud-calibration-plan.md](./cloud-calibration-plan.md). Nothing below has
> changed yet; the manual workflow remains the current, working procedure and will stay
> the fallback path once the cloud flow ships.

## Overview

DPV-Nav calibrates four sensors/subsystems: magnetometer, gyroscope, accelerometer, and heading. Calibration data is persisted to LittleFS as JSON files on the nav device. On boot, saved calibration loads in under a second. If gyro/accel files are missing, blocking calibration runs automatically. Mag is the exception: on a brand-new device (no mag_base.json/mag_mount.json/mag_cal.json at all) there's no sane automatic mag cal to run, so mag stays uncalibrated (identity — no correction) until the diver runs CAL > Baseline (+ Mounted) from the menu. Heading cal is always optional and silently skipped if absent.

## Calibration Files

```
/mag_base.json      — Baseline magnetometer (hard-iron + soft-iron 3×3), off-scooter cal
/mag_mount.json     — Mounted magnetometer correction (hard-iron + soft-iron 3×3), on-scooter
/mag_cal.json       — Legacy single-stage mag cal (loaded as fallback if base/mount absent)
/gyro_cal.json      — Gyroscope bias
/accel_cal.json     — Accelerometer bias + scale
/hdg_samples.csv    — Raw (target, indicated) pairs from heading cal collection run
/hdg_fourier.json   — Fourier heading correction (n harmonics + coefficient array, optional)
/motor_cal.json     — Motor-on heading offset (single float, optional)
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

**HdgFourier** (Fourier heading correction):
```json
{ "n": 4, "c": [5.85, -0.39, -5.10, 0.16, -16.54, -0.76, 1.88, -0.27, 2.92] }
```
`n` is the number of harmonics (1–4). `c` is the Fourier coefficient array: `[DC, cos1, sin1, cos2, sin2, ...]` (length `2n+1`). Generated offline by `tools/fourier_fit.py`.

## Boot Sequence

```
For mag: try two-stage chain (mag_base.json + mag_mount.json)
          → fall back to legacy mag_cal.json
          → fall back to identity (no correction); diver runs CAL > Baseline (+ Mounted) later
For gyro: load gyro_cal.json or run 10s stationary bias cal
For accel: load accel_cal.json or run 6-point orientation cal
For hdg: load hdg_fourier.json — silently skip if absent (optional)
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

> **Superseded (2026-07-25):** the live bin-coverage grid described in steps 3-5 below
> (including its two-pass revision from 2026-07-24, see
> [baseline-cal-two-pass.md](./baseline-cal-two-pass.md)) failed on hardware and is
> being retired for Baseline. The replacement collects with simple axis-range bars and
> no live grid, then grades the result for real on the server and shows coverage gaps
> on the Dive Map website instead of on the unit's screen — see
> [divemap's baseline-cal-coverage-feedback-plan.md](../../divemap/docs/architecture/baseline-cal-coverage-feedback-plan.md).
> Not yet implemented; this doc will get a fuller rewrite once it is.

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
   `python tools/mag_calibration.py --mode mounted --base mag_base.json mag_mounted_samples.csv`
5. Upload the generated `mag_mount.json` to LittleFS **under exactly that name** (`/mag_mount.json`). Do not pass `--output` with a different name unless you rename it on upload — the firmware only reads `/mag_mount.json` and silently ignores anything else. See the heading troubleshooting notes for why this bites.

### What each calibration corrects

| Distortion | Baseline | Mounted |
|-----------|---------|---------|
| Hard-iron bias (sensor, PCB) | ✓ | Refined |
| Soft-iron coupling (PCB, enclosure) | ✓ | Refined |
| DPV motor/battery magnetic signature | — | ✓ |

### Forcing re-calibration

Delete the corresponding JSON file(s) from LittleFS and reboot, or run the menu cal workflow to generate new sample CSVs.

**Planned cloud workflow:** see [cloud-calibration-plan.md](./cloud-calibration-plan.md) —
the offline steps above (export CSV, run `mag_calibration.py` locally, upload the
result) are planned to become "unit uploads the CSV over WiFi, gets a fitted result and
a quality verdict back, accept or reject on the spot." The on-device steps (positioning,
menu navigation, bin-coverage collection) are unchanged either way.

## Gyroscope Calibration

**Method:** Stationary bias sampling (runs at boot if no file found)

**Procedure:**
1. Place device on a stable, level surface
2. Keep completely still — no movement or vibration
3. System samples for 10 seconds at ~100 Hz
4. Average of all samples = bias offset; scale remains 1.0

**Typical values:**
- Good: ±10–50 raw counts (±0.001 rad/s)
- Bad (device moved): > 200 raw counts — repeat on a more stable surface

To force recalibration: delete `/gyro_cal.json` from LittleFS and reboot.

---

## Accelerometer Calibration

**Method:** 6-point orientation sequence (runs at boot if no file found)

**Not sure which physical side is "forward," "right," etc.?** Before calibrating,
run the `accel_orient` serial command (see [Serial Diagnostic Commands](#serial-diagnostic-commands)
below). It prints which of the 6 orientation labels currently matches, updating
a line at a time as you rotate the device in your hand, so you can learn the
mapping without guessing.

**Procedure:**
1. System prompts for 6 orientations in turn, each phrased as attitude-first
   plain language plus which edge/side ends up on top:
   - Nose pointing straight up — Front edge UP
   - Nose pointing straight down — Front edge DOWN
   - Resting on its left side — RIGHT side UP
   - Resting on its right side — LEFT side UP
   - Upside down, level — BOTTOM side UP
   - Straight and level, normal — TOP side UP
2. For each one, point the device that way and press Enter to sample.
3. Hold the device steady while it samples (2.5 s by default).
4. System computes: `bias = (max + min) / 2`, `scale = 9.81 / (max - bias)` per axis

**Typical values:**
- Bias: ±50 raw counts (±0.004 m/s²)
- Scale: 0.98 – 1.02 per axis

## Serial Diagnostic Commands

A few serial commands help confirm sensor orientation before or instead of
running a full calibration (connect via `pio device monitor`, type the
command, press Enter):

| Command | Purpose |
|---------|---------|
| `accel_orient` | Prints a new line each time the detected accelerometer calibration orientation changes as you rotate the device. Use this to learn the physical mapping before running accelerometer calibration. |
| `sensor_orientation` | One-shot: dumps raw native-frame accel/gyro/mag counts for a device held level, with axis-mapping guidance. |
| `axis_test` | Prompts you to point the device North/East/South/West and compares magnetometer readings, to sanity-check heading axis mapping. |
| `debug_axes` | One-shot snapshot tracing a sample through axis mapping, unit conversion, calibration, and the AHRS filter — useful when diagnosing heading errors. |

## Fourier Heading Calibration

**Purpose:** After magnetometer calibration, complex systematic heading errors typically remain (5–20°) due to the DPV's magnetic geometry (motor, batteries, structural asymmetries). The 4-harmonic Fourier correction captures these non-sinusoidal errors and reduces residual error to ~2° max.

**When to do it:** After completing baseline + mounted mag cal. Run in a magnetically clean environment (living room on a wooden floor — not in a garage with rebar in concrete). The device must already have a working magnetometer calibration.

**Trigger:** Menu → **CAL > Hdg cal**

### Procedure

**Part 1 — On-device data collection:**

1. Open menu (BTN1) → **CAL > Hdg cal**. The menu closes and the heading cal screen appears.
2. The screen shows **Step 1/12: Target 000°** and the live AHRS heading in large text.
3. Align the DPV to 0° (North). Watch the heading readout stabilise.
4. Press **BTN2** to capture that heading. The screen advances to **Step 2/12: Target 030°**.
5. Repeat for all 12 steps (every 30°: 0°, 30°, 60°, … 330°). Each BTN2 press captures the current indicated heading and records `(target, indicated)` on the nav device.
6. After Step 12, the nav device saves `/hdg_samples.csv` and the done screen appears:

```
HDG CAL
─────────────────
12 points saved

Export /hdg_samples.csv
Run fourier_fit.py
Upload hdg_fourier.json
─────────────────
Press BTN2 to exit
```

7. Press **BTN2** to return to normal navigation.

**Part 2 — Offline fit and upload:**

1. Download `/hdg_samples.csv` from the nav device (via web interface or LittleFS tools).
2. Run: `python tools/fourier_fit.py hdg_samples.csv [--plot]`
   - Prints per-point residuals and selects the best number of harmonics (1–4).
   - Saves `hdg_fourier.json` in the current directory.
   - Optionally saves `hdg_fourier_fit.png` (requires `--plot`).
3. Upload `hdg_fourier.json` to the nav device LittleFS root.
4. Reboot — the correction loads automatically.

### CSV format

`/hdg_samples.csv` has two columns:

```csv
actual,indicated
0.0,4.7
30.0,24.3
60.0,52.1
...
```

`actual` is the target heading the user aligned to; `indicated` is what the AHRS reported.

### How it works at runtime

The nav device loads `hdg_fourier.json` on boot and evaluates the Fourier series at each heading computation:

```
correction(θ) = c[0] + c[1]·cos(θ) + c[2]·sin(θ) + c[3]·cos(2θ) + c[4]·sin(2θ) + ...
headingDeg = headingRawDeg + correction(headingRawDeg)
```

`heading_raw_deg` is always sent in NavPacket alongside the corrected `heading_deg`, so the display can show pre-correction readings during recalibration.

### Recalibrating

Run **CAL > Hdg cal** again, then redo the offline fit and upload the new `hdg_fourier.json`.

### Troubleshooting: heading still off after a full calibration

Real failure modes actually hit in the field, hardest-to-spot first. If the heading is wrong *after* baseline + mounted + Fourier, it is almost always one of these, not the mag math:

1. **Checking in TRUE mode against a magnetic reference (or vice-versa).** *Symptom:* a near-constant offset of roughly the local declination (≈14.7° here) in **every** sector, with only a few degrees of per-sector variation on top. The nav device always computes TRUE heading; **DISPLAY > Heading** toggles TRUE/MAG on the display side ([display_main.cpp:237-244](../src/display_main.cpp#L237-L244)). A handheld compass reads MAGNETIC. When checking against a compass, set the display to **MAG**. This is the #1 cause of "everything is ~15° off."

2. **Mounted correction uploaded under the wrong filename.** The firmware only ever loads `/mag_mount.json` ([storage.h:28](../src/util/storage.h#L28)). If you run the tool with `--output mag_mounted.json` (or any other name) and upload *that*, it is silently ignored — the **previous** mounted cal stays in effect and your fresh numbers never take. *Symptom:* the soft-iron ellipticity you meant to remove is still present (large residual 2nd harmonic). `mag_calibration.py` now prints a loud warning when the output name won't be read; heed it. Confirm on boot: `[STORAGE] mag_mount.json loaded`, with a `b_eff` that matches the new values.

3. **Motor offset shifts the check but not the cal.** `/motor_cal.json`'s `heading_offset_deg` is added to the *displayed* heading **after** the Fourier correction ([nav_main.cpp:333](../src/nav_main.cpp#L333)), but the hdg-cal samples record `heading_raw_deg`, which is *before* it. So the motor offset is invisible to the fit and shows up as a constant bias in your check. Expect it, or zero it while collecting.

4. **Fourier over-fit / coverage gaps.** With 12 points, `fourier_fit.py` caps at 2 harmonics on purpose — a tiny RMS at higher orders is over-fit, not accuracy. Because the deviation compresses the *indicated* scale, evenly-spaced targets can leave a large hole in the indicated domain (a ~50° gap near North is common); the fit is unconstrained there and worst in that sector. The tool warns when the largest gap exceeds 45°. Fill it by adding targets near the gap (see below).

**Hand-collecting extra samples to fill a gap:** the new rows must be in the same frame as the existing ones — `heading_raw_deg`, i.e. **pre-Fourier, pre-motor magnetic**. To read that value off the nav screen: delete `hdg_fourier.json`, set `motor_cal.json` to `0`, reload cal, and put the display in **MAG** mode. Collect and append `actual,indicated` rows, refit, then restore both files. (Cleaner alternative: widen `kHdgCalTargets` in [display_main.cpp](../src/display_main.cpp#L68-L71) so the on-device flow gathers the extra points itself — no file juggling, guaranteed-consistent data.)

## Storage API

```cpp
#include "util/storage.h"

// Magnetometer
storage::saveMagCalibration("/mag_base.json", magCal);
storage::loadMagCalibration("/mag_base.json", magCal);

// Gyro / Accel
storage::saveCalib3("/gyro_cal.json", gyroCal);
storage::loadCalib3("/gyro_cal.json", gyroCal);

// Heading cal (read-only on device — file is uploaded by user after offline fit)
#include "util/hdg_cal.h"
hdg_cal::load(gHdgCal);                    // returns bool; reads /hdg_fourier.json
hdg_cal::apply(headingDeg, gHdgCal);       // returns corrected heading
```

Files are stored on LittleFS (ESP32 internal flash). Total calibration data is <5 KB.

## Troubleshooting

| Problem | Cause | Fix |
|---------|-------|-----|
| Heading off by consistent amount | Systematic hard-iron bias | Run mounted mag cal or Fourier heading cal |
| Heading varies by orientation | Soft-iron distortion | Run full mounted cal + offline ellipsoid fit |
| Fourier residuals > 5° after fit | Calibration in bad environment | Move away from concrete floors with rebar; redo collection |
| Fourier residuals > 5° in clean env | Complex DPV magnetic geometry | Use 4 harmonics; check DPV for loose magnetic components |
| Gyro bias very large (>500) | Device moved during cal | Repeat on stable surface, no vibration |
| Accel scale way off (>1.1) | Wrong orientation or sensor issue | Verify axis labeling, check sensor |
| "LittleFS mount failed" | Flash partition not configured | Check `board_build.filesystem = littlefs` in platformio.ini |

## Motor-On Heading Correction

**Purpose:** The Fourier heading calibration is collected with the motor off (bench procedure). When the DPV motor runs, its magnetic field adds a roughly constant heading bias — typically a fixed offset that doesn't depend on heading angle or motor speed. `/motor_cal.json` stores this single correction value.

**When to do it:** After completing Fourier heading cal. Run a reciprocal-leg test on a known bearing with the motor running; compare indicated headings against expected and note the offset. Alternatively, use `tools/correct_track.py` with a calibration run CSV (reciprocal legs between two known GPS points) to derive the offset automatically.

**File format** (`/motor_cal.json`):
```json
{ "heading_offset_deg": -3.0 }
```

- Positive value: compass reads low (offset is added to get true heading)
- Negative value: compass reads high (offset is subtracted from indicated heading)

**Loading:** The nav device loads `/motor_cal.json` at boot (and on "Reload Cal Files" from the web page). If the file is absent or invalid, no motor correction is applied.

**Creating the file manually:**
1. Align the running DPV to a known magnetic bearing.
2. Note the difference: `offset = actual − indicated`.
3. Create `motor_cal.json` with that value and upload it to LittleFS root.
4. Reboot or use "Reload Cal Files" on the web page.

## Speed Calibration (Flow Sensor)

Speed cal measures the flow sensor's k-factor (pulses per litre) by timing a swim over a
known distance. Up to 6 runs are averaged and saved to `/speed_cal.json` on LittleFS.

The flow sensor converts pulse frequency to speed:
```
speed_ms = (freq_hz / k_factor) / 60 / 1000 / cross_section_m2
```
Speed cal solves for `k_factor` given true distance and total pulses. Elapsed time cancels —
only pulse count and distance matter.

**Before speed cal:** Set `FLOW_CROSS_SECTION_M2` in `config.h` to the physical inner
cross-section of the DPV inlet. For a 50 mm diameter tube: π × (0.025)² ≈ 0.00196 m².

### Running a speed cal

1. Menu → **CAL > Speed cal**
2. Select distance (150–500 ft in 50 ft steps, default 300 ft)
3. Start DPV — timing begins automatically when flow exceeds threshold
4. Swim the selected distance at constant speed
5. Choose **ACCEPT**, **RESET+ACCEPT**, or **REJECT**

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
