# Soft-Iron Magnetometer Calibration Workflow

The built-in `calibrateMagnetometer()` only computes hard-iron offset (bias), setting the soft-iron matrix to identity. For ±2-5° heading accuracy on a DPV with motors, batteries, and other magnetic sources, you need soft-iron calibration using the hybrid ellipsoid-fitting workflow described here.

## Key Insight: 3D RMS ≠ Heading Accuracy

These are different metrics and can diverge significantly:

- **3D RMS** measures how well calibrated points fit a sphere across all orientations. A 3D ellipsoid fit minimizes this globally, but pulls the sphere center toward densely-sampled regions. If most samples are from tilted orientations (the upper hemisphere when tumbling), the horizontal-plane bias is under-constrained and can be off by hundreds of counts, causing systematic heading errors even with excellent RMS.

- **Heading accuracy** only depends on bias_x and bias_y — the two axes that determine `atan2(y, x)`. Even a 1.5% sphere RMS can produce 15-25° heading errors if the XY center is offset.

**The fix:** Use separate data collection sources for different parts of the calibration:
- Flat rotation data → 2D algebraic circle fit → `bias_x`, `bias_y`
- 3D spherical data → Z range midpoint → `bias_z`
- 3D spherical data (centered on flat-derived bias) → PCA → soft-iron shape

## Workflow

### 1. Collect Spherical Data (Full 3D Coverage)

Mount the magnetometer on the DPV in its installed position — all magnetic sources (motors, batteries, electronics) must be present and in their normal configuration during collection.

#### Method A: Menu (recommended)

1. Power on both devices
2. Open menu on display device (BTN1) → **CAL > Full cal**
3. The OLED switches to a calibration progress screen showing time remaining and a coverage bar
4. Rotate the DPV through all orientations:
   - Drive in large circles (left and right)
   - Tilt nose up and down while circling
   - Roll left and right while circling
   - Goal: trace a sphere in 3D space with the magnetometer
5. Continue for 120 seconds — progress screen shows elapsed % and countdown
6. After 120 seconds the screen returns to navigation; data saved to `/mag_cal_samples.csv` on LittleFS

#### Method B: Serial commands

```
start_cal   — Start mag calibration data collection (30 sec default)
dump_cal    — Dump collected data to serial as CSV
clear_cal   — Clear calibration data from LittleFS
```

**Target:** ≥1000 unique samples spanning all orientations.

### 2. Collect Flat Rotation Data

This is a separate, dedicated step that is critical for horizontal-plane heading accuracy:

1. Place the DPV as flat/level as possible (or use a known-flat surface)
2. Slowly rotate the DPV in a full 360° circle around its vertical axis, staying as flat as possible
3. One slow, smooth 360° rotation takes ~30-60 seconds — multiple overlapping rotations are fine
4. Export this as a separate CSV (see export steps below)

The flat rotation data feeds the 2D circle fit that directly determines `bias_x` and `bias_y`, which are the only two values that affect horizontal heading accuracy.

### 3. Data Export (ESP32 → PC)

**Spherical data:**
1. In serial terminal, type `dump_cal` and press Enter
2. System prints CSV data between `--- START DATA ---` and `--- END DATA ---` markers
3. Copy all lines between markers (including header `X,Y,Z`)
4. Save as `mag_samples.csv`

**Flat rotation data (two options):**

- **From dedicated collection:** Collect separately using `start_cal`, `dump_cal` while holding flat. Save as `mag_flat.csv`.
- **From nav log:** If data logging is enabled (high-rate), export the log file and use it directly. The `mag_x_raw/y_raw/z_raw` columns are in µT (raw sensor, **no axis map applied**). The script converts automatically.

### 4. Run the Calibration Script

```bash
cd d:\Documents\dpv-nav\tools
python mag_calibration.py mag_samples.csv mag_flat.csv
```

Without flat data (falls back to 3D-only, poorer heading accuracy):
```bash
python mag_calibration.py mag_samples.csv
```

**Requirements:** `pip install numpy scipy`

**Example output:**
```
Bias from 2D circle fit (flat data) + 3D Z midpoint:
  X:  1606.52  (23.48 uT)  -- from flat rotation
  Y:  2697.98  (39.43 uT)  -- from flat rotation
  Z:  6227.00  (91.01 uT)  -- from 3D Z range

RMS error: 81.77  (2.37%)  Quality: ACCEPTABLE

HEADING ACCURACY ANALYSIS (flat device)
============================================================

  Flat rotation azimuth coverage: 12/12 sectors (100%)

  Calibrated flat circle (counts / uT):
  Center: (-0.3 / -0.004 uT,  -9.7 / -0.142 uT)  (ideal: 0, 0)
  Radius: X=1214.0 (17.74 uT)  Y=1244.8 (18.19 uT)
  Roundness: 0.975  (ideal: 1.000)

  Heading accuracy estimate:
  From circle offset:  +/-0.5 deg max
  From ellipticity:    +/-1.4 deg max
  Total (worst case):  +/-1.9 deg
  Rating: EXCELLENT (<2 deg expected error)
```

**Quality checks:**

The script reports two quality metrics — for this DPV use case, only heading accuracy matters:

| Metric | Meaning | Target |
|---|---|---|
| Heading accuracy (EXCELLENT) | Flat-device heading error estimate | <2° |
| Heading accuracy (GOOD) | Acceptable for most uses | 2-5° |
| 3D sphere RMS | Overall sphere quality | Secondary — will degrade with hybrid approach (2-3% is normal and acceptable) |

The hybrid approach intentionally sacrifices some 3D sphere RMS to optimize horizontal heading accuracy. A 2.37% 3D RMS with EXCELLENT heading rating is better than 1.08% 3D RMS with ±15° heading errors.

**Coverage map note:** The coverage chart shows magnetometer *reading vector* directions, not device orientation. A flat device with 65° local magnetic inclination maps to ~25° from the +Z pole — it will appear in the 0-30° bin, not at the equator of the chart. This is correct behavior.

### 5. Upload Calibration (PC → ESP32)

1. Copy `tools/mag_cal.json` → `data/mag_cal.json`
2. Run:
   ```bash
   pio run -e nav -t uploadfs
   ```
   **WARNING:** `uploadfs` reformats LittleFS — gyro/accel cal files are wiped. Either copy `gyro_cal.json` and `accel_cal.json` into `data/calib/` first, or re-run those calibrations after upload.
3. Reboot — `storage::loadMagCalibration()` loads `/mag_cal.json` automatically

### 6. Verification

Compare against a known-good magnetic compass while holding the DPV flat:

1. Point to 8 compass headings (N, NE, E, SE, S, SW, W, NW)
2. Record DPV heading reading at each
3. Check that errors alternate above and below zero (random ±noise), not all in one direction
4. All-same-sign errors indicate residual bias offset — recollect flat rotation data

**Expected accuracy with hybrid calibration:** ±2-3° typical, ±5° worst case across all headings when flat.

## Algorithm Details

### Why the Old 3D-Only Approach Failed

The 3D algebraic ellipsoid fit minimizes sphere residuals for all samples equally. When samples are unevenly distributed — more from tilted orientations than flat ones — the center drifts toward the dense region. Flat-plane bias errors of 100-500 counts (1.5-7 µT) are common, causing 10-25° heading errors even at excellent 3D RMS.

Adding flat samples to the spherical dataset partially helps but doesn't fully fix it — the 3D fit still averages over the whole distribution.

### The Hybrid Approach

1. **`bias_x`, `bias_y`** — Algebraic 2D circle fit on flat rotation X,Y samples:
   `2cx·x + 2cy·y + F = x² + y²` (linear least squares)
   This is immune to elevation-distribution effects because it only uses the axes that matter for heading.

2. **`bias_z`** — Midpoint of the 3D Z range: `(max_z + min_z) / 2`

3. **Soft-iron matrix** — PCA of 3D samples centered on the flat-derived bias. Finds principal axes of the ellipsoid, scales them to equal radius.

### Heading Error Formulas

- **From circle offset:** `arcsin(center_distance / circle_radius)` — systematic bias in one direction
- **From ellipticity:** `arctan((r_max - r_min) / r_avg)` — error that varies sinusoidally with heading

## Theory: Why Ellipsoid Fitting?

The magnetometer measures Earth's magnetic field vector (~50-60 µT). Rotating through all orientations, raw readings should form a **sphere** centered at origin.

Magnetic distortions cause:
- **Hard-iron:** Sphere shifts away from origin (constant offset)
- **Soft-iron:** Sphere deforms into ellipsoid (directional scaling/rotation)

Simple min/max calibration only centers the ellipsoid (hard-iron). Ellipsoid fitting also reshapes it back into a sphere (soft-iron), giving accurate heading in all orientations.

## Nav Log Format (for flat data from logging)

The data logger records `mag_x_raw`, `mag_y_raw`, `mag_z_raw` in **µT** with **no axis map applied** (raw LIS3MDL sensor values). The script auto-detects this format and converts to counts (×68.42 LSB/µT). Do not negate the Y axis — it is raw sensor output, not the remapped navigation frame.

## References

- Freescale AN4246: "Calibrating an eCompass in the Presence of Hard and Soft-Iron Interference"
- ST AN4509: "Ellipsoid fitting for magnetometer calibration"
