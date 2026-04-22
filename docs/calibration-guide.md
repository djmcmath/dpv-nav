# Sensor Calibration Guide

## The short version

The compass chip can't read north accurately out of the box — nearby magnets, motors, and metal
bend the field. Calibration measures and corrects for this.

**Two-stage process:**

1. **Baseline cal** — Characterize the sensor on its own (off-scooter). Covers the full sphere.
2. **Mounted cal** — Correct for what the DPV's motors and batteries add. Done on-scooter.

Both stages follow the same pattern:
- Run a collection on the device (menu → CAL)
- Download the CSV to a PC
- Run a Python script to fit the calibration
- Upload the resulting JSON and reboot

If either stage is missing, the device still operates — it just uses whatever it has. Baseline only
is much better than nothing. Both stages gives you the best heading accuracy.

Gyro and accelerometer calibrate automatically on first boot. You normally only need to do the mag
calibration manually.

---

## Prerequisites

Install Python dependencies once:
```bash
pip install numpy scipy
```

You will need to download CSV files from the ESP32's LittleFS flash. Use the web interface
(WiFi must be enabled, get the device IP from the serial log) or a serial file-transfer tool.

---

## Stage 1: Baseline Calibration

Do this with the nav unit on the bench — not mounted on the DPV. The battery and electronics
that live on the nav PCB should be present; the DPV motors should not be.

### Step 1a: Collect baseline samples

1. Power on both devices (nav + display)
2. Open menu (short press BTN1)
3. Navigate to **CAL > Baseline** and press BTN2

The OLED switches to a calibration grid: 12 columns (heading sectors, 0° through 330°) and
5 rows (elevation bands: nose-up 60°, 30°, level, -30°, -60°). Each cell starts dark, turns
**yellow** at 5 samples, and **green** at 15 samples.

**Rotate the device through all orientations:**
- Spin slowly in a full circle while flat → fills the middle row (level band)
- Tilt nose up ~30° and spin again → fills row above center
- Tilt nose down ~30° and spin again → fills row below center
- Tilt nose up ~60° and spin → fills top row
- Tilt nose down ~60° and spin → fills bottom row
- Diagonal combinations fill any remaining cells

Take your time. Slow, deliberate rotation covers more unique orientations than fast spinning.
Expect 3–8 minutes for full green coverage. **There is no time limit.**

The collection ends automatically when all cells are green. The screen returns to navigation
and the samples are saved to `/mag_baseline_samples.csv` on LittleFS.

**Tip for the flat rotation:** Include one slow, smooth 360° spin while holding the device as
flat as possible. This same data will improve horizontal heading accuracy in the Python script.
Note roughly which portion of your collection was flat — you may want to export it separately.

### Step 1b: Export the CSV

Download `/mag_baseline_samples.csv` from LittleFS to your PC. Save it in the `tools/` directory
(or wherever you'll run the Python script from).

### Step 1c: Run the baseline Python script

```bash
cd tools
python mag_calibration.py --mode baseline mag_baseline_samples.csv
```

If you did a dedicated flat rotation and have it as a separate file:
```bash
python mag_calibration.py --mode baseline mag_baseline_samples.csv --flat mag_flat.csv
```

**What you should see:**

```
BASELINE CALIBRATION RESULTS
============================================================

Hard-Iron Offset (bias):
  X:  1606.52
  Y:  2697.98
  Z:  6227.00

Soft-Iron Correction Matrix:
  [1.023456, 0.001234, 0.000000]
  [0.001234, 0.978901, 0.000000]
  [0.000000, 0.000000, 1.000000]

Diagnostics:
  Samples: 847
  Ellipsoid radii:  [1214.3, 1244.7, 1198.6]
  Radii variation: 1.87%

Residuals:
  RMS error:  81.77  (2.37%)
  Max |error|: 156.23  (4.52%)
  Quality: ACCEPTABLE (2-5% RMS)
```

**Interpreting baseline output:**

| Metric | Good | Acceptable | Problem — recollect |
|--------|------|------------|---------------------|
| Samples | > 500 | 200–500 | < 100 |
| RMS error % | < 2% | 2–5% | > 5% |
| Radii variation | < 5% | 5–10% | > 10% (WARNING printed) |

If you provided flat data, a heading accuracy section also appears:

```
HEADING ACCURACY ANALYSIS (flat device)
============================================================

  Calibrated flat circle:
  Center: (-0.3 / -0.004 uT,  -9.7 / -0.142 uT)
  Radius: X=1214.0  Y=1244.8   Roundness=0.975

  Heading accuracy estimate:
  From circle offset:  +/-0.5 deg
  From ellipticity:    +/-1.4 deg
  Total (worst case):  +/-1.9 deg
  Rating: EXCELLENT
```

| Heading accuracy | Rating | What it means |
|-----------------|--------|---------------|
| < 2° total | EXCELLENT | As good as it gets |
| 2–5° total | GOOD | Acceptable for navigation |
| > 5° total | POOR | Recollect flat rotation data; make sure the device was actually flat |

**If RMS is POOR (> 5%):** The most common cause is incomplete spherical coverage —
not all grid cells were green, or the rotation was too fast for the bins to fill. Re-run
the collection and move more slowly through the extreme elevation bands (top and bottom rows).

The script writes `mag_base.json` in the current directory.

### Step 1d: Upload mag_base.json and reboot

> **Warning:** `uploadfs` reformats the entire LittleFS filesystem. All files — including
> gyro and accel calibration — are wiped. Copy anything you want to keep into the `data/`
> directory before running it; they will be restored alongside your new calibration.

1. Copy `tools/mag_base.json` to `data/mag_base.json`
2. If you have existing `gyro_cal.json` or `accel_cal.json`, copy those to `data/` as well
3. Upload:
   ```bash
   pio run -e nav -t uploadfs
   ```
4. Power-cycle the nav device

In the serial log on boot you should see:
```
[STORAGE] mag_base.json loaded
```

The device is now running with baseline calibration. Heading accuracy at this point is good
for bench testing. For use on the DPV, proceed to Stage 2.

---

## Stage 2: Mounted Calibration

This stage corrects for the magnetic distortion from the DPV itself — primarily the motors
and motor controller. Do this with the nav unit installed on the scooter in its normal
operating position, DPV fully powered.

Baseline calibration (Stage 1) must be loaded before running this step.

### Step 2a: Collect mounted samples

1. Power on DPV with nav unit installed
2. Open menu (BTN1)
3. Navigate to **CAL > Mounted** and press BTN2

The OLED shows a 12×3 grid (same 12 heading sectors, 3 elevation bands: nose-up 30°, level,
nose-down 30°). The mounted cal only covers ±30° pitch — reflecting the range of angles
possible while maneuvering a DPV underwater.

**What to do:**
- Drive the DPV in a slow, full circle at level trim → fills the center row
- Drive a circle with mild nose-up angle (10–25°) → fills top row
- Drive a circle with mild nose-down angle (10–25°) → fills bottom row

Keep your speed slow and constant. Abrupt changes confuse the heading estimate that drives
bin assignment. The motors must be running — they're the main magnetic source you're
calibrating against.

Collection ends when all cells are green. Saves `/mag_mounted_samples.csv` and returns to
navigation.

### Step 2b: Export the CSV

Download `/mag_mounted_samples.csv` from LittleFS to your PC. Save it in `tools/`.

### Step 2c: Run the mounted Python script

```bash
cd tools
python mag_calibration.py --mode mounted --base mag_base.json mag_mounted_samples.csv
```

**What you should see:**

```
MOUNTED CALIBRATION CORRECTION RESULTS
============================================================

Mounted hard-iron offset (b_mount):
  X:  +0.0234
  Y:  -0.0156
  Z:     0.0000  (locked)

Mounted soft-iron correction (M_mount, diagonal):
  [1.023400, 0.000000, 0.000000]
  [0.000000, 0.978500, 0.000000]
  [0.000000, 0.000000, 1.000000]

Diagnostics:
  Samples: 412  |  Scale fit method: algebraic
  Flat circle radius (base-corrected): 1198.45 counts  (17.51 µT)
  Ellipticity heading error (pre-correction): ±4.8°
  RMS error (after correction): 45.23  (1.87%)
```

**Interpreting mounted output:**

- **Ellipticity heading error (pre-correction):** How much heading error the DPV's fields
  were adding before this correction. 2–15° is normal and expected. If it's near zero, the
  scooter has minimal magnetic signature and the mounted cal is just refining the bias.
- **RMS error (after correction):** Should be < 3%. If it's > 5%, collect more samples with
  better heading coverage.
- **Scale correction > 15%:** The script will note this. It is plausible for a DPV with
  strong motor fields. Proceed and verify with the compass check below.
- **Scale correction < 2%:** Also noted. The DPV's soft-iron effect is small; the main
  benefit of mounted cal is the hard-iron (b_mount) correction.

The script writes `mag_mount.json`.

### Step 2d: Upload mag_mount.json and reboot

1. Copy `tools/mag_mount.json` to `data/mag_mount.json`
2. Also ensure `data/mag_base.json` is present (it should be from Stage 1)
3. Run:
   ```bash
   pio run -e nav -t uploadfs
   ```
4. Power-cycle the nav device

In the serial log on boot you should see both lines:
```
[STORAGE] mag_base.json loaded
[STORAGE] mag_mount.json loaded
```

The device now applies the full correction chain on every magnetometer reading:
`corrected = M_mount × (M_base × (raw − b_base) − b_mount)`

---

## Verification

**This is the test that matters.** Do it before trusting the system for a dive.

You need a reference compass — a standalone dive compass or a phone compass app. A phone is
good enough here since you're looking for gross errors, not ±1° accuracy.

1. Install the nav unit on the DPV in its normal diving position
2. Hold the DPV flat and level (as you would while diving)
3. Point to **8 headings**: N, NE, E, SE, S, SW, W, NW
4. At each heading, compare what the DPV display shows against your reference compass
5. Write down the error (positive = DPV reads too high)

**Interpreting results:**

| Error pattern | What it means | Action |
|---------------|---------------|--------|
| All errors within ±5°, alternating sign | Calibration is good | You're done |
| All errors biased in one direction (e.g. all positive) | Residual hard-iron offset | Redo flat rotation; rerun baseline with `--flat` |
| Large swings (e.g. +20° at N, −15° at E) | Soft-iron still distorted | Redo baseline with more complete spherical coverage |
| Heading reads backwards (increasing counterclockwise) | Axis map problem | Check CLAUDE.md magnetometer coordinate frame section |

**Pass criterion:** All 8 headings within ±5° of reference. If any heading is off by more
than 10°, do not dive with this unit — recalibrate first.

---

## Gyroscope Calibration

Runs automatically on first boot if `/gyro_cal.json` is absent.

**Procedure:** Place the device on a stable surface. Keep it completely still. The system
samples for 10 seconds and saves the bias offset.

**Typical values:**
- Good: ±10–50 raw counts (±0.001 rad/s)
- Bad (device moved): > 200 raw counts — repeat on a more stable surface

To force recalibration: delete `/gyro_cal.json` from LittleFS and reboot.

---

## Accelerometer Calibration

Runs automatically on first boot if `/accel_cal.json` is absent.

**Procedure:** Follow the serial prompts. Hold each of 6 faces pointing down (X+, X−, Y+, Y−,
Z+, Z−) for 2.5 seconds each. The device prompts before each orientation.

**Typical values:**
- Bias: ±50 raw counts (±0.004 m/s²)
- Scale per axis: 0.98–1.02

To force recalibration: delete `/accel_cal.json` from LittleFS and reboot.

---

## Calibration Files on LittleFS

```
/mag_base.json          — Baseline mag cal (hard-iron offset + soft-iron matrix)
/mag_mount.json         — Mounted correction (applied on top of base)
/gyro_cal.json          — Gyro bias
/accel_cal.json         — Accel bias + scale
/mag_baseline_samples.csv   — Raw baseline collection (temporary; safe to delete after upload)
/mag_mounted_samples.csv    — Raw mounted collection (temporary; safe to delete after upload)
```

The legacy `/mag_cal.json` (single-stage, hard-iron only) is still read as a fallback if
`mag_base.json` is absent. The bin-aware two-stage workflow supersedes it.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| Boot log shows no `[STORAGE] mag_base.json loaded` | File missing or uploadfs not run | Verify `data/mag_base.json` exists before uploadfs |
| Heading correct on bench but wrong on DPV | Mounted cal not loaded | Check boot log for `mag_mount.json` line |
| Heading drifts slowly while stationary | Gyro bias not calibrated | Check `/gyro_cal.json` loaded at boot |
| All 8 verification headings off by same amount | Residual hard-iron bias | Redo flat rotation; rerun baseline with `--flat` |
| Heading oscillates ±20° with rotation | Soft-iron not corrected | Better baseline coverage; aim for all 60 bins green |
| Python says POOR RMS (> 5%) | Insufficient 3D coverage | Slower rotation; cover all grid rows including top and bottom |
| uploadfs wiped my gyro/accel cal | Forgot to copy to `data/` | Copy JSONs to `data/`, repeat uploadfs |

---

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
| **ACCEPT** | Normal run — adds to rolling average (up to 6 measurements) |
| **RESET+ACCEPT** | DPV serviced, impeller changed, or first cal on new hardware — clears history |
| **REJECT** | Run was suspect (aborted early, speed varied, DPV turned mid-run) |

### Troubleshooting speed cal

| Problem | Fix |
|---------|-----|
| Run starts before DPV is moving | Raise `SPEED_CAL_START_THRESHOLD_HZ` in config.h |
| Run stops too early mid-swim | Lower `SPEED_CAL_STOP_THRESHOLD_MS` slightly |
| k-factor wildly different from existing | Verify `FLOW_CROSS_SECTION_M2` is set correctly |
| `/speed_cal.json` not found on first run | Normal — file is created on first ACCEPT |
