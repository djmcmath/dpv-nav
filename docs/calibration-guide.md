# Sensor Calibration Guide

> **Shipped:** Baseline, Mounted, and Heading (12-pt) calibration all fit in the cloud
> now. Collect on the unit with WiFi connected, and the nav device uploads the raw
> samples, gets a fitted result + quality verdict back within seconds, and shows an
> accept/reject screen right there on the display — no laptop step required. See
> [cloud-calibration-plan.md](./cloud-calibration-plan.md) and divemap's
> [heading-cal-cloud-plan.md](../../dive-map/docs/architecture/heading-cal-cloud-plan.md)
> for the implementation history. The manual laptop workflow (export CSV, run the
> Python fitting tool locally, upload the result under the exact expected filename)
> described in places below is now the **offline fallback** — it still works, and is
> what happens if the unit has no WiFi at the moment calibration finishes, since the
> raw CSV is always saved to LittleFS either way.

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

The preferred calibration workflow uses two stages, each producing a JSON file that is the result of ellipsoid fitting — done automatically in the cloud when the unit has WiFi, or offline with the same fitting tool as a fallback:

| Stage | Menu item | File | When to do |
|-------|-----------|------|-----------|
| Baseline | CAL > Baseline | `/mag_base.json` | First time, or after hardware change. Device off the DPV. Covers full sphere. |
| Mounted | CAL > Mounted | `/mag_mount.json` | After baseline, with device installed on DPV. Corrects for DPV magnetic signature. |

Mounted (and Baseline's `Fill gaps` patch pass) collect raw samples using a
**bin-aware** approach: the screen shows a heading × elevation grid, and bins fill
green as adequate samples are collected, completing automatically once all required
bins are green. Baseline's first-time pass instead uses axis-range bars with no grid
and completes when the diver presses BTN2 — see the note under **Running Baseline
Calibration** below for why. Either way, the sample CSV is saved to LittleFS once
collection ends.

### Running Baseline Calibration

> **Live bin-coverage grid retired for Baseline (2026-07-25).** Six attempts at a
> trustworthy live grid during a first-ever baseline pass all fought the same
> chicken-and-egg problem (heading needs a calibration that doesn't exist yet) — see
> [baseline-cal-two-pass.md](./baseline-cal-two-pass.md) for the failed attempts. The
> replacement (shipped) collects with simple axis-range bars and live fit stats
> instead — no grid, no false confidence — then grades coverage for real on the
> server and shows a coverage heatmap on the Dive Map website. See divemap's
> [baseline-cal-coverage-feedback-plan.md](../../dive-map/docs/architecture/baseline-cal-coverage-feedback-plan.md).

1. **Remove device from DPV** and take it somewhere with low magnetic interference.
2. Open menu (BTN1) → **CAL > Baseline**.
3. The display shows axis-range bars and live fit stats (no grid — see note above). Rotate the device through all orientations — roll, pitch, yaw, and diagonals — until the bars look full and the fit stats look stable.
4. When you judge coverage good enough, press **BTN2**. The display shows a brief "finishing" screen while the nav device writes `/mag_baseline_samples.csv` to LittleFS.
5. **If the unit has WiFi connected:** the nav device uploads the CSV, the server fits it and grades coverage, and the result is staged (not yet active). The display shows an accept/reject screen with the quality band and RMS. Accept to install it immediately (hot-reloads on the spot); reject to discard it and keep whatever was active before. The full coverage heatmap and any thin/empty regions are visible afterward on the Dive Map website under Calibration History.
6. **If the unit has no WiFi:** the display shows an offline notice and no fit happens automatically. The CSV is still saved to LittleFS, so you can fall back to the manual path — download `/mag_baseline_samples.csv` (web interface or LittleFS tools) and run `python tools/mag_calibration.py mag_baseline_samples.csv`, then upload the generated `mag_base.json` to LittleFS yourself.

**Filling coverage gaps:** see [Filling Coverage Gaps](#filling-coverage-gaps) below — thin or empty regions the website flags can be patched with **CAL > Fill gaps** without redoing the whole tumble.

### Running Mounted Calibration

Mounted keeps the original live bin-coverage grid — its narrower ±30° operating range sidesteps the pole problem that motivated retiring Baseline's grid, so nothing about the collection UI changed here.

1. **Mount device on DPV** in its normal installed position (all motors, batteries, and other magnetic sources present and at operational distance).
2. Open menu (BTN1) → **CAL > Mounted**.
3. Rotate the DPV through heading and tilt orientations. DPV operation is primarily horizontal so the grid only requires 3 elevation bands.
4. When all required bins are green, the nav device writes `/mag_mounted_samples.csv` to LittleFS.
5. **If the unit has WiFi connected:** same cloud round trip as Baseline — CSV uploads automatically, the server fits it, and the display shows an accept/reject screen with quality band and RMS. Accept to install and hot-reload immediately.
6. **If the unit has no WiFi:** offline notice is shown; CSV is still saved. Fall back to the manual path: export `/mag_mounted_samples.csv` and run
   `python tools/mag_calibration.py --mode mounted --base mag_base.json mag_mounted_samples.csv`,
   then upload the generated `mag_mount.json` to LittleFS **under exactly that name** (`/mag_mount.json`). Do not pass `--output` with a different name unless you rename it on upload — the firmware only reads `/mag_mount.json` and silently ignores anything else (this only applies to the manual fallback path; the cloud path always writes the correct filename). See the heading troubleshooting notes for why this bites.

### What each calibration corrects

| Distortion | Baseline | Mounted |
|-----------|---------|---------|
| Hard-iron bias (sensor, PCB) | ✓ | Refined |
| Soft-iron coupling (PCB, enclosure) | ✓ | Refined |
| DPV motor/battery magnetic signature | — | ✓ |

### Forcing re-calibration

Delete the corresponding JSON file(s) from LittleFS and reboot, or run the menu cal workflow to generate new sample CSVs.

## Filling Coverage Gaps

Redoing an entire baseline tumble or 12-point heading collection to patch one thin
region is wasteful. Both mag baseline and heading (hdg) calibration support adding a
short supplemental collection that the server merges into the existing fit, rather
than starting over. The two work differently — mag's gap-fill is on-device, hdg's is
a web form — because their gaps are detected differently (a 2-D orientation grid vs.
a 1-D ring of headings).

### Baseline: CAL > Fill gaps

A *second* baseline pass that patches only the orientation cells the Dive Map website
flagged as thin or empty on your installed baseline calibration — not a full redo.

**Preconditions** (both refused on-screen if missing, rather than run degraded):
- A baseline calibration must already be installed.
- A target map must have been synced first: on `tern.local`, under **Calibration
  Cloud Sync**, press **Check for updates** while connected to WiFi (cached on the
  device as `/cal_targets.json`). This is a **web page button on the unit's own
  `tern.local` page**, not a device menu item — there is no `CAL > Check for
  updates` on the unit itself; the on-device refusal message says as much.
  **Every** "Check for updates" click refreshes the target map, not just the
  first — it's the same call whether or not there's a new cal file to install,
  so re-run it any time you want the latest thin/empty regions (e.g. after
  merging a gap-fill patch on the website), not just once ever.
- If the target map fails to sync, the reason shows up right in the "Check for
  updates" result text on `tern.local` (a common one: the currently-accepted
  baseline predates coverage grading, or has none). It used to fail silently
  into a Serial log only, useless for a diver checking from a phone with no USB
  cable — fixed so the actual failure reason is always visible on the page.

**Procedure:**
1. Open menu (BTN1) → **CAL > Fill gaps**.
2. Unlike the regular Baseline pass, this one *does* show a live coverage grid — a
   good calibration already exists, so orientation can be computed algebraically
   per-sample without needing the Mahony filter. Rotate toward the flagged cells;
   the grid fills in as you go.
3. Collection auto-completes once every targeted cell is satisfied. BTN2 also
   finishes early, since a targeted cell can be physically unreachable depending on
   how the device is mounted.
4. The patch uploads to the cloud as a baseline-mode collection and is fit from
   *just* the patched cells — small on purpose. This fit is never directly
   installable (the on-device result screen is informational only, no accept/reject
   toggle) — it only becomes useful once merged with your existing baseline.
5. On the Dive Map website, open **Calibration History** for the device. Your most
   recent baseline row shows a **"New collection available"** badge. Expand it to
   see what the patch covers (a small coverage-grid thumbnail, which gaps it fills,
   sample count), then tap to combine. This produces a new pending calibration row
   (not yet accepted) that you review and **Accept** the same way as any fresh fit.

**Why you sometimes have to hold the unit upside-down or on its side:**

The 60-cell grid tracks *where* the sensor's tracked axis points — elevation and
heading — but not how the unit is *rolled* about that axis while pointing there.
That's on purpose: it's what makes tilt-compensated heading work at all. But it
means a cell can show fully green ("enough samples at this elevation/heading")
while every one of those samples came from roughly the same roll — a real gap in
what the fit actually saw, invisible on the grid alone.

To close it, each cell also grades **roll diversity** across four orientations —
**upright**, **on its right side**, **upside-down**, **on its left side** — the
same plain-language poses the accelerometer calibration section above uses.
Upright needs about 3× the samples of the other three (most natural handling is
upright-biased anyway, and closing this gap only needs enough angular diversity
to break the "one roll" degeneracy, not dense sampling at every angle). A cell
you're standing in can look done on the main grid and still want a quick
roll — inverting the unit, or laying it on a side — before it clears.

The screen shows a small square, split into four triangles (top/right/bottom/left
= upright/right-side/upside-down/left-side), next to the orientation readout. It
always reflects **whichever cell you're currently pointing at** — no separate
screen to flip to — colored the same red/yellow/green as the main grid, with a
white outline on whichever triangle matches your current roll. If a cell's
triangles are still red or yellow once the main grid says you're on it, roll the
unit through the outlined shape's neighbors until they clear.

*(Readings can lag or jump while the unit is actively moving — the accelerometer
briefly sees your hand's motion as "down," not just gravity. Pause near the
target orientation rather than reading the grid or the roll widget mid-motion.)*

### Heading (12-pt): the website manual-entry form

The 12 fixed points (every 30°) can leave one span under-sampled — e.g. only 4 of the
12 points falling in a 90° swing near North — which the whole-CSV fit can mask with a
deceptively good in-sample error while the correction is actually bad in the gap. The
server checks for this automatically on every heading fit (`sector_adequacy`): it
looks for gaps wider than 45° between the collected **indicated** headings and, if it
finds one, suggests bearings to steer that would land readings inside it.

The two frames matter here. The gap is found in the *indicated* domain, because that
is what the Fourier series is a function of and therefore where it is unconstrained —
a gap is wide precisely because the indicated heading swung fast across it. But a
diver can only steer an *actual* bearing, so each suggestion is projected back by
interpolating between the two collected points that bracket the gap, then rounded to
the nearest 5° (the finest anyone can hold against a small compass). Suggestions
landing within 10° of a bearing you already collected are dropped, since they would
add no constraint the fit doesn't already have.

Unlike Baseline's gap-fill, there is **no on-device collection flow for this** — no
firmware round trip, no menu item. It's a small manual-entry form on the website:

1. On the Dive Map website, open **Calibration History** for the device. A heading
   (hdg) calibration row with a detected gap shows a **"N thin sector(s)"** badge.
2. Expand it to see the flagged gap(s) and the bearings to steer (e.g. "Between your
   337° and 066° points the compass swung 90° — steer 015°, 050°").
3. **On the unit, set DISPLAY > Heading to RAW** before taking any readings. The fit
   needs the uncorrected magnetic heading; the nav screen normally shows the
   corrected one, and readings taken in TRUE or MAG mode will make the calibration
   worse rather than better.
4. An editable-rows form appears, seeded with those bearings. For each one: aim the
   DPV at that bearing using the same external reference (compass, known landmark)
   the original 12-point collection used, read the heading off the unit's display,
   and type the `(actual, indicated)` pair into the form by hand.
5. Submit. This posts the typed rows to the server, which combines them with your
   original 12-point upload and re-fits — producing a new pending calibration row,
   same as any other fit (not auto-accepted).
6. Review and **Accept** the new row on the website when you're satisfied. Set the
   unit's heading mode back off RAW (it also reverts to MAG on the next boot).

**Getting an accepted web result onto the physical unit:** accepting a calibration
*on the website* only updates the database — there is no background mechanism that
pushes it to the device. The device only picks up an accepted web-side result when
you explicitly ask it to: on `tern.local`, under **Calibration Cloud Sync**, press
**Check for updates**. This applies to both the Baseline gap-fill merge and the hdg
manual-entry gap-fill described above — a calibration you accepted on the web is not
"live" on the unit until you also do this step. (A calibration accepted *on the
device itself*, via the normal accept/reject screen right after collection, needs no
such step — it's already local.)

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
6. After Step 12, the nav device saves `/hdg_samples.csv` and immediately hands off to
   the same cloud upload/accept-reject flow Baseline and Mounted use (there is no
   longer a separate "export CSV, run the tool, upload" screen here):
   - **If the unit has WiFi connected:** the CSV uploads automatically, the server
     fits it (auto-selecting 1–4 harmonics) and checks for sector gaps (see
     [Filling Coverage Gaps](#filling-coverage-gaps) below), and the display shows
     an accept/reject screen with the quality band and max error in **degrees**
     (baseline/mounted show the equivalent number as a percent — same screen,
     different unit label). Accept to install `hdg_fourier.json` immediately and
     hot-reload; reject to discard and keep whatever heading correction was active
     before.
   - **If the unit has no WiFi:** the display shows an offline notice. `/hdg_samples.csv`
     is still saved to LittleFS either way, so the manual fallback below still works.
7. Press **BTN2** to exit the result screen and return to normal navigation.

**Part 2 — Offline fit and upload (fallback only, when the unit has no WiFi at cal time):**

1. Download `/hdg_samples.csv` from the nav device (via web interface or LittleFS tools).
2. Run: `python tools/fourier_fit.py hdg_samples.csv [--plot]`
   - Prints per-point residuals and selects the best number of harmonics (1–4).
   - Saves `hdg_fourier.json` in the current directory.
   - Optionally saves `hdg_fourier_fit.png` (requires `--plot`).
3. Upload `hdg_fourier.json` to the nav device LittleFS root.
4. Reboot — the correction loads automatically.

This offline tool has no sector-gap check of its own — it fits whatever CSV you give
it. If you took this fallback path and want gap detection too, you can still upload
the CSV later once you have WiFi (via the same on-device Hdg cal flow, or by getting
the raw CSV into a fresh cloud upload), or just watch for an obviously bad sector in
the residual printout and hand-add points there before fitting.

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

Run **CAL > Hdg cal** again — the fresh 12-point collection uploads and fits in the
cloud automatically (WiFi permitting) and replaces `hdg_fourier.json` on accept, same
as the first time.

### Troubleshooting: heading still off after a full calibration

Real failure modes actually hit in the field, hardest-to-spot first. If the heading is wrong *after* baseline + mounted + Fourier, it is almost always one of these, not the mag math:

1. **Checking in TRUE mode against a magnetic reference (or vice-versa).** *Symptom:* a near-constant offset of roughly the local declination (≈14.7° here) in **every** sector, with only a few degrees of per-sector variation on top. The nav device always computes TRUE heading; **DISPLAY > Heading** cycles TRUE → MAG → RAW on the display side. A handheld compass reads MAGNETIC. When checking against a compass, set the display to **MAG**. This is the #1 cause of "everything is ~15° off." (**RAW** is a third, bench-only mode — magnetic *and* uncorrected — used for gap-fill collection, not for checking a finished calibration; see below.)

2. **Mounted correction uploaded under the wrong filename.** The firmware only ever loads `/mag_mount.json` ([storage.h:28](../src/util/storage.h#L28)). This is specifically a risk of the **manual offline fallback path** (cloud uploads always write the correct filename automatically): if you run the tool with `--output mag_mounted.json` (or any other name) and upload *that*, it is silently ignored — the **previous** mounted cal stays in effect and your fresh numbers never take. *Symptom:* the soft-iron ellipticity you meant to remove is still present (large residual 2nd harmonic). `mag_calibration.py` now prints a loud warning when the output name won't be read; heed it. Confirm on boot: `[STORAGE] mag_mount.json loaded`, with a `b_eff` that matches the new values.

3. **Motor offset shifts the check but not the cal.** `/motor_cal.json`'s `heading_offset_deg` is added to the *displayed* heading **after** the Fourier correction ([nav_main.cpp:333](../src/nav_main.cpp#L333)), but the hdg-cal samples record `heading_raw_deg`, which is *before* it. So the motor offset is invisible to the fit and shows up as a constant bias in your check. Expect it, or zero it while collecting.

4. **Fourier over-fit / coverage gaps.** With 12 points, the fit caps at 2 harmonics on purpose — a tiny RMS at higher orders is over-fit, not accuracy. Because the deviation compresses the *indicated* scale, evenly-spaced targets can leave a large hole in the indicated domain (a ~50° gap near North is common); the fit is unconstrained there and worst in that sector. The cloud fit flags this automatically (`sector_adequacy`, gaps > 45°) and the Dive Map website shows a "thin sector(s)" badge with suggested headings to fill it — use the manual-entry form described in [Filling Coverage Gaps](#filling-coverage-gaps) above, it's the supported way to close a gap without redoing all 12 points. `fourier_fit.py`, the offline-fallback tool, warns about the same gap but has no form to act on it — see the hand-collecting note below if you're on that path.

**Reading indicated headings for gap-fill (both paths):** the new rows must be in the same frame as the existing ones — `heading_raw_deg`, i.e. **pre-Fourier, pre-motor magnetic**. Set **DISPLAY > Heading** to **RAW** and read the value straight off the nav screen; the digits turn yellow and the suffix reads `R` so a RAW reading can't be mistaken for a navigable heading. RAW shows `heading_raw_deg` from the NavPacket verbatim — the same value `CAPTURE_HDG_POINT` records — so `hdg_fourier.json` and `motor_cal.json` can stay installed. Set the mode back when you're done; RAW is not sticky and reverts to MAG on the next boot anyway.

> Before 2026-08-27 this required deleting `hdg_fourier.json`, zeroing `motor_cal.json`, reloading cal, and then restoring both by hand — with no backup path for `motor_cal.json`, so a copy had to be saved first. That is no longer necessary and should not be done.

On the **offline-fallback path**, append the `actual,indicated` rows to `hdg_samples.csv` and refit with `fourier_fit.py`. On WiFi, use the web form described in [Filling Coverage Gaps](#filling-coverage-gaps) above.

**What the suggested headings mean:** they are **bearings to steer**, rounded to the nearest 5° (the finest a diver can hold against a small compass). The gap itself is detected in the *indicated* domain — that is where the Fourier series is unconstrained — but each suggestion is projected back into the actual domain by interpolating between the two collected points bracketing the gap, so aiming at one lands the reading inside the hole. The website states both frames: "Between your 337° and 066° points the compass swung 90° — steer 015°, 050°."

## Storage API

```cpp
#include "util/storage.h"

// Magnetometer
storage::saveMagCalibration("/mag_base.json", magCal);
storage::loadMagCalibration("/mag_base.json", magCal);

// Gyro / Accel
storage::saveCalib3("/gyro_cal.json", gyroCal);
storage::loadCalib3("/gyro_cal.json", gyroCal);

// Heading cal (read-only on device — file is written by ACCEPT_CLOUD_CAL after a
// cloud fit, or uploaded manually after the offline fallback)
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

### Deriving k from a corrected dive

A dive corrected in Dive Map against GPS fixes and waypoint snaps reports a speed
factor (called `k` there too, unrelated to this one) defined as

```
true_speed = s x logged_speed
```

Reported speed is proportional to `1/k_factor`, so scaling speed by `s` means:

```
k_new = k_old / s
```

Sign check, worth doing every time: `s < 1` means the unit read fast, so `k` goes **up**.

Two things to get right:

- **`k_old` must be what the unit was actually running on that dive** — its NVS history
  via `speed_cal::averageK()`, not `baseline cal jsons/speed_cal.json`, which is only the
  seed file. If those differ, the composition is wrong.
- **Use one joint least-squares solve over the legs you trust.** Do not average per-leg
  `s` values (each has absorbed its own leg's heading error), and do not average several
  whole-dive solves that share data — both double-count, and both drag the answer back
  toward whichever legs you were trying to exclude.

Feeding the result back in: this is a derived measurement, not a timed run, so it does not
belong in the rolling average alongside them. Set `/speed_cal.json` to `{"n":1,"k":[k_new]}`
— the equivalent of **RESET+ACCEPT** — and note what it came from.

#### Worked example — 2026-08-29, Dawn

Five legs, anchored by two GPS fixes and three waypoint positions. Legs 1–3 (surface swim
out, then both deep DPV legs) were judged trustworthy; legs 4–5 were shallow with
intermittent scooting and swimming, so one speed factor cannot hold across them.

```
joint solve, legs 1-3   s = 0.9029    k_new = 0.1686 / 0.9029 = 0.1867
joint solve, legs 2-3   s = 0.8728    k_new = 0.1686 / 0.8728 = 0.1932
joint solve, all 5      s = 0.8657    k_new = 0.1686 / 0.8657 = 0.1947
```

**Do not chase the last digit.** Re-solving with the three waypoint positions perturbed by
±10 m (1σ) — realistic, since one is sidescan-derived — gives `s = 0.906 ± 0.034`, so
`k_new` lands anywhere in 0.179–0.193. Which legs you include moves the answer less than
the anchor uncertainty does. Take it to three decimals, record what it was derived from,
and expect to refine it.

The heading offset from the same solve (+11.6°) barely moves under that perturbation. That
one is worth acting on with confidence; this one is a better guess than what it replaces.

### Setting FLOW_CROSS_SECTION_M2

`FLOW_CROSS_SECTION_M2` in `config.h` must be set to the physical intake cross-section of the DPV inlet before speed cal can give sensible k-factor values. Measure the inner diameter of the flow sensor mounting tube:
```
area = π × (diameter/2)²
```
A 50 mm diameter tube → 0.00196 m² ≈ 0.002 m². This constant is not calibrated automatically.
