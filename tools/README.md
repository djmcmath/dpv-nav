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

## correct_track.py

Post-dive dead-reckoning correction tool. Takes a DPV-nav data log CSV (with `pos_src` column) and corrects the DR track to close on known GPS anchors.

### Installation

```bash
pip install numpy scipy
```

(numpy/scipy are only required for the `--plot` flag and some advanced modes; the core correction runs without them)

### Usage

```bash
python correct_track.py input.csv --mode joint --output corrected.csv
```

### Correction Modes

| Mode | When to use |
|------|------------|
| `joint` (default) | Single dive with GPS at start and end. Solves for a constant speed scale (k) and heading offset (θ) across all DR segments using closed-form weighted least squares. |
| `proportional` | Loop dives or cases where joint fit degenerates. Applies distance-weighted closure independently per segment; always closes exactly. |
| `reciprocal` | Calibration runs only (outbound leg → GPS midpoint → reciprocal return). Solves simultaneously for k, θ, and water current vector. Use to derive `motor_cal.json` heading offset and speed k-factor from a controlled test. |

### Reciprocal Calibration Workflow

1. Set up a known course between two GPS-visible points.
2. Run the DPV out on one heading, get a GPS fix midpoint, then run back on the reciprocal heading.
3. Download the log CSV.
4. Run: `python correct_track.py run.csv --mode reciprocal`
5. The output includes the solved heading offset (θ) — use this as `heading_offset_deg` in `motor_cal.json`.

### Output columns

The tool adds `adj_pos_x_m`, `adj_pos_y_m`, `adj_lat`, `adj_lon` columns. GPS rows pass through unchanged. To re-ingest into dive-map visualization, copy the `adj_lat`/`adj_lon` columns over the originals.

### Optional Flags

| Flag | Description |
|------|------------|
| `--mode <mode>` | `joint` (default), `proportional`, or `reciprocal` |
| `--output <file>` | Output CSV path (default: `<input>_corrected.csv`) |
| `--max-theta <deg>` | Reject joint solution if heading offset exceeds this value |
| `--max-k-error <frac>` | Reject joint solution if speed scale error exceeds this fraction |
| `--plot` | Save a plot of original vs corrected track (requires matplotlib) |

## Future Tools

- **log_analyzer.py**: Parse and visualize LittleFS data logs
- **heading_plotter.py**: Real-time heading visualization from serial output
- **imu_diagnostic.py**: Sensor health check and noise analysis
