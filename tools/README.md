# DPV-Nav Tools

This directory contains offline processing tools for calibration and data analysis.

## mag_calibration.py

Python script for computing magnetometer soft-iron calibration from raw sensor data.

### Installation

```bash
pip install numpy scipy
```

### Usage

```bash
python mag_calibration.py mag_samples.csv
```

**Input:** CSV file with raw magnetometer samples (X,Y,Z columns)
**Output:** `calib_mag_cal.json` containing hard-iron offset + soft-iron matrix

### Example Output

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

Saved calibration to calib_mag_cal.json
```

### Quality Indicators

- **Radii variation < 5%**: Excellent calibration, good rotation coverage
- **Radii variation 5-10%**: Acceptable, but could improve with better rotation
- **Radii variation > 10%**: Poor calibration, incomplete rotation coverage - recollect data

### Complete Workflow

See [../docs/mag-calibration-workflow.md](../docs/mag-calibration-workflow.md) for the full ESP32 → PC → ESP32 calibration procedure.

## Future Tools

- **log_analyzer.py**: Parse and visualize SPIFFS data logs
- **heading_plotter.py**: Real-time heading visualization from serial output
- **imu_diagnostic.py**: Sensor health check and noise analysis
