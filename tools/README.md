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

## Track assembly and correction — moved to Divemap

`correct_track.py`, `assemble_track.py` and their tests now live in the Divemap
repo, at **`dive-map/track-processor/`**.

They moved because they stopped being bench scripts and became the engine of a
Divemap service: the `track-processor` container runs exactly this code to
assemble uploaded logs into dives and correct them. A second copy here would
guarantee the two drift — and a divergence between what the website computes and
what you compute at the bench is a very hard bug to see.

Same tools, same flags. To run them against logs in this directory:

```bash
cd ../dive-map/track-processor

# Group a pile of uploaded logs into dives (handles power cycles, junk files,
# and logs that start after the unit has already left its last GPS fix)
python3 assemble_track.py ../../dpv-nav/tools/*.csv -o /tmp/dives

# Correct one. The default --mode auto works out which correction the anchor
# geometry can actually support, and tells you what it cannot know.
python3 correct_track.py /tmp/dives/dive_20260705_090952.csv

# Tests
python3 tests/test_correct.py && python3 tests/test_assemble.py
```

### Getting a dive that can actually be calibrated

An anchor in the **middle** of a dive is worth far more than fixes at both ends
of a round trip. On an out-and-back your net displacement is near zero no matter
how far you swam, so the endpoints cannot tell speed error apart from a current —
both explain the drift equally well, and the solver will happily report a
physically absurd speed factor.

Snapping a waypoint at the wreck (a `W` row) splits that useless loop into two
informative legs, which is enough to solve for speed factor, heading offset
**and** the current, exactly. `--mode auto` will say so when it happens.

## Future Tools

- **log_analyzer.py**: Parse and visualize LittleFS data logs
- **heading_plotter.py**: Real-time heading visualization from serial output
- **imu_diagnostic.py**: Sensor health check and noise analysis
