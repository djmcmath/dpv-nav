# DPV-Nav Tools

This directory contains offline processing tools for calibration and data analysis.

## mag_calibration.py

Python script for computing magnetometer calibration from raw sensor samples. Supports
the two-stage baseline + mounted workflow.

### Installation

```bash
pip install numpy scipy
```

### Usage

**Baseline mode** (off-scooter, full sphere):
```bash
python mag_calibration.py --mode baseline mag_baseline_samples.csv
python mag_calibration.py --mode baseline mag_baseline_samples.csv --flat mag_flat.csv
```
Output: `mag_base.json`

**Mounted mode** (on-scooter correction, requires baseline result):
```bash
python mag_calibration.py --mode mounted --base mag_base.json mag_mounted_samples.csv
```
Output: `mag_mount.json`

### Quality Indicators

**Baseline:**
- RMS error < 2%: GOOD
- RMS error 2–5%: ACCEPTABLE
- RMS error > 5%: POOR — recollect with better spherical coverage

**Heading accuracy** (reported when `--flat` data is provided):
- Total error < 2°: EXCELLENT
- Total error 2–5°: GOOD
- Total error > 5°: POOR — recollect flat rotation data

### Complete Workflow

See [../docs/calibration-guide.md](../docs/calibration-guide.md) for the full step-by-step
procedure including what to expect at each stage and how to verify the result.

## Future Tools

- **log_analyzer.py**: Parse and visualize LittleFS data logs
- **heading_plotter.py**: Real-time heading visualization from serial output
- **imu_diagnostic.py**: Sensor health check and noise analysis
