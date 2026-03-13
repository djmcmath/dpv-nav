# Soft-Iron Magnetometer Calibration Workflow

The built-in `calibrateMagnetometer()` only computes hard-iron offset (bias), setting the soft-iron matrix to identity. For ±5° heading accuracy on a DPV with motors, batteries, and other magnetic sources, you need soft-iron calibration using ellipsoid fitting.

## When You Need This

- Magnetometer is mounted on a DPV with variable magnetic signature
- Simple hard-iron calibration still produces >5° heading errors
- Heading accuracy varies depending on which direction you're facing

## Workflow

### 1. Data Collection (ESP32)

Mount the magnetometer on the DPV in its installed position — all magnetic sources (motors, batteries, electronics) must be present and in their normal configuration during collection.

#### Method A: Menu (recommended)

1. Power on both devices
2. Open menu on display device (BTN1) → **CAL > Full cal**
3. The OLED switches to a calibration progress screen showing time remaining and a coverage bar
4. Immediately start rotating the DPV:
   - Drive in large circles (left and right)
   - Tilt nose up and down while circling
   - Roll left and right while circling
   - Goal: trace a sphere in 3D space with the magnetometer
5. Continue for 120 seconds — the progress screen shows elapsed % and a countdown
6. After 120 seconds the screen returns to normal navigation; data is saved to `/mag_cal_samples.csv` on LittleFS

#### Method B: Serial commands

The collection module ([util/mag_cal_collect.cpp](../src/util/mag_cal_collect.cpp)) also accepts serial commands (useful for bench testing without the display device):

```
start_cal   — Start mag calibration data collection (30 sec default)
dump_cal    — Dump collected data to serial as CSV
clear_cal   — Clear calibration data from LittleFS
```

**Important:** The more complete your rotation coverage, the better the calibration. Target ≥1000 samples and coverage across all orientations.

### 2. Data Export (ESP32 → PC)

1. In serial terminal, type `dump_cal` and press Enter
2. System prints CSV data between `--- START DATA ---` and `--- END DATA ---` markers
3. Copy all lines between markers (including header: `X,Y,Z`)
4. Save as `mag_samples.csv` on your PC

**Verify data quality:**
- Should have 1000+ samples (120 sec collection at the main loop rate)
- X, Y, Z values should vary widely (full rotation coverage)
- If samples are clustered, re-run with better rotation

### 3. Ellipsoid Fitting (Python on PC)

```bash
cd d:\Documents\dpv-nav\tools
python mag_calibration.py mag_samples.csv
```

**Requirements:** `pip install numpy scipy`

**Output:**
```
Hard-Iron Offset (bias):
  X:  1600.50
  Y: -2782.50
  Z:  6671.00

Soft-Iron Correction Matrix:
  [ 0.950000,  0.012000, -0.008000]
  [ 0.012000,  1.020000,  0.005000]
  [-0.008000,  0.005000,  1.030000]

Radii variation: 2.15%
```

**Quality check:**
- Radii variation <10% (ideally <5%)
- If >10%, rotation coverage was poor — recollect
- Soft-iron matrix should be close to identity (diagonal ~1.0, off-diagonal ~0.0)
- Large deviations (>0.2 on diagonals) suggest strong magnetic distortion

Script saves `calib_mag_cal.json` with both bias and soft-iron matrix.

### 4. Upload Calibration (PC → ESP32)

**Option A: LittleFS upload (recommended)**
1. Place `calib_mag_cal.json` in `data/` folder
2. Run `pio run -e nav -t uploadfs`
3. Reboot — `storage::loadMagCalibration()` loads it automatically

**Option B: Hardcode in nav_main.cpp (quick bench test)**
```cpp
MagCalib magCal{
  {1600.5f, -2782.5f, 6671.0f},  // bias
  {
    { 0.950000f,  0.012000f, -0.008000f},
    { 0.012000f,  1.020000f,  0.005000f},
    {-0.008000f,  0.005000f,  1.030000f}
  }
};
```

### 5. Verification

1. Place device in known orientation (pointing magnetic north)
2. Read heading from serial output
3. Rotate 90° clockwise (east) — verify heading increases ~90°
4. Rotate 90° counter-clockwise (west) — verify heading decreases ~90°
5. Repeat at different pitch/roll angles

**Expected accuracy:** ±5° heading error across all orientations.

## Theory: Why Ellipsoid Fitting?

The magnetometer measures Earth's magnetic field vector (~50-60 µT). Rotating through all orientations, raw readings should form a **sphere** centered at origin.

Magnetic distortions cause:
- **Hard-iron:** Sphere shifts away from origin (constant offset)
- **Soft-iron:** Sphere deforms into ellipsoid (directional scaling/rotation)

Simple min/max calibration only centers the ellipsoid (hard-iron). Ellipsoid fitting also reshapes it back into a sphere (soft-iron), giving accurate heading in all orientations.

## Multi-Environment Calibration

For DPVs used in different configurations (different gear mounted):
1. Collect data in each configuration
2. Run Python script for each: `mag_calibration.py mag_samples_light.csv`
3. Save each as separate JSON: `calib_mag_cal_light.json`, etc.
4. Load appropriate calibration based on current configuration

## References

- Freescale AN4246: "Calibrating an eCompass in the Presence of Hard and Soft-Iron Interference"
- ST AN4509: "Ellipsoid fitting for magnetometer calibration"
