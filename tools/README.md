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

## Calibration gap-fill host tests

None of these need hardware or PlatformIO -- they host-compile the
dependency-free parts of the firmware and drive them against the server's own
Python. Run all three after touching `src/util/mag_cal_orient.cpp`,
`lib/dpvlink/dpvlink.cpp`, or `divemap/calibration-processor/callib/coverage.py`.
The first and third need an interpreter with the calibration-processor's
requirements installed (they import `callib.coverage`, which uses numpy).

| Script | Proves |
|---|---|
| `orient_equivalence.py` | the firmware orientation port and `coverage.py` agree sample for sample, including every degenerate branch and the poles. Also prints the axis-convention sentinel (passing `magNED` costs up to 180 deg of heading). |
| `dpvlink_test/run.sh` | `CalProgressPacket`'s 2-bits-per-cell target map round-trips index-for-index, rejects corrupt input wholesale, and fits the 512-byte link buffer. |
| `gapfill_chain_check.py` | a cell the *server* flags is the cell the *device* targets -- driven end to end through bin assignment, wire packing, and decode. |

What they do **not** prove: that the firmware compiles, or that the axis
conventions match the physical hardware. **They structurally cannot** -- they
are synthetic, and they build their samples from the same convention the code
under test assumes, so any error both sides share is invisible to them. That is
not hypothetical: on 2026-08-26 `orient_equivalence.py` passed on all 12,247
samples while the mag vector was being consumed in a Y-mirrored frame alongside
right-handed accel, an error worth up to 180 deg of heading at any attitude off
level. Use `frame_fixture_check.py` for that half.

## Frame fixture — the only test with external ground truth

`frame_fixture_check.py` replays a capture taken from **real hardware at known
physical attitudes** and checks that the orientation math returns the attitude
that was actually held. Its ground truth is an operator and a compass, not the
code, which is the entire point.

```bash
python3 frame_fixture_check.py                 # uses fixtures/frame_fixture.csv
python3 frame_fixture_check.py other.csv other_cal.json
```

To capture (or re-capture) a fixture, run the **`axis_test`** serial command on
the nav device. It walks you through 13 attitudes, verifies each gravity-snapped
one against `classifyAccelOrientation()` rather than trusting you held it, and
prints two blocks at the end — save them as `fixtures/frame_fixture.csv` and
`fixtures/frame_fixture_cal.json`. Re-capture only when the sensors physically
move on the board or the axis maps change; a re-capture invalidates the old
fixture as ground truth for the new geometry.

Its headline number is **dip consistency**: the angle between the magnetic field
and gravity-down is a property of your location, so it must read the same at
every attitude regardless of heading, declination, or hemisphere. If mag and
accel are in frames of opposite handedness it instead swings — by 138 deg in the
2026-08-26 case — with no ground-truth heading needed to see it.

Run it after any change to `src/util/mag_cal_orient.cpp`, `callib/coverage.py`,
the `AxisMap`s in `nav_main.cpp`, or the `magNED` negation. The synthetic tests
above cannot catch what it catches, and it cannot catch what they catch.
