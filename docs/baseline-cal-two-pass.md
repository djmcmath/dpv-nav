# Baseline Calibration: Two-Pass Redesign

> **Superseded (2026-07-25).** A hardware test of this design (real N/E/S/W grid, fed
> by the ROUGH_SCAN bias bootstrap) still failed — no correlation between physical
> orientation and the highlighted box, consistent with Mahony filter lag/hysteresis
> under fast rotation, not a mapping bug. A follow-up check also ruled out per-axis
> coverage bars as a completion signal (structurally can't see joint 3-D gaps) and the
> live incremental fit readout (settles at a stable-but-wrong value even for good
> datasets). The live on-device grid concept is retired for Baseline; see
> [divemap's baseline-cal-coverage-feedback-plan.md](../../divemap/docs/architecture/baseline-cal-coverage-feedback-plan.md)
> for the replacement — collect honestly, grade for real server-side, show gaps on the
> web. Left in place below for the diagnostic history (why each on-device attempt
> failed) — genuinely useful context, not a live plan.
>
> **Revisited (2026-11-05).** A replay of five real baseline uploads found the pass-2
> handoff was probably fighting a bad bias as well as filter lag: the min/max bias was
> 15–25 µT off until the last third of a full tumble. A sphere fit on the same samples is
> within 1–3 µT after 50–75 samples. A live baseline grid built on that, binned the way
> gap-fill bins, is planned in [baseline-live-grid-plan.md](./baseline-live-grid-plan.md).
> See Status below.

Companion to [dev-calibration-guide.md](./dev-calibration-guide.md) (the on-device procedure) and
[cloud-calibration-plan.md](./cloud-calibration-plan.md) (the WiFi upload/fit round-trip).
This note is about what happens *during* Baseline collection, on the device, before any
CSV is ever uploaded — specifically, how the live bin-coverage grid decides what to show
the diver while they're rotating the unit.

## Goal

Give the diver a coverage indicator during Baseline cal that they can actually trust:
a box lighting up should mean they did the thing it claims, consistently, in a way that
lets them tell "I have more to do" from "I'm done" without guessing. Divemap's
`docs/architecture/device-calibration-plan.md` calls this the open UX question; this is
the resolution on the dpv-nav side.

## The original problem

Baseline's bin grid (`imu.cpp`'s `getBinIndex`) classified each raw sample by AHRS
`pitch_deg`/`heading_deg` into a 12-heading × 5-elevation grid. Two things made that
untrustworthy specifically during Baseline:

- **Chicken-and-egg**: heading accuracy depends on the magnetometer being calibrated,
  which is the exact thing Baseline is collecting data *for*. The AHRS's own heading
  output is close to a guess for the whole run.
- **Geometric degeneracy near vertical**: tilt-compensated heading is ill-conditioned as
  pitch approaches ±90° — a known, already-designed-around case
  (`MAG_CAL_SECTORS_EXTREME` in `config.h` already gives the ±60° rows a lower bar for
  exactly this reason), but it compounds the first problem rather than replacing it.

## What was tried and didn't work

**v1 — raw-vector direction, Fibonacci lattice.** Bin by the raw (recentered)
magnetometer vector's nearest match in a 60-point golden-angle equal-area lattice,
instead of AHRS heading. Sidesteps both problems above — no AHRS dependency at all. On
real hardware: the highlighted cell jumped unpredictably (e.g. slow rightward rotation
sometimes moved the highlight 8 cells right, or 4 left) with a visible diagonal-streak
coverage pattern. Cause: displaying lattice index `i` at row-major grid position
`(i/12, i%12)` assumes neighboring indices are neighboring points in 3D space. Golden-angle
constructions deliberately do the opposite — consecutive indices are scattered far apart
in azimuth, which is what gives the lattice its equal-area property. Spatially-adjacent
samples landed at unrelated screen positions.

**v2 — raw-vector direction, fixed body axis.** Same raw-vector idea, but classified via
elevation/azimuth around a fixed body axis (the mag sensor's own +Z), reusing the
existing row/col band logic so screen-adjacent cells would be space-adjacent again. On
real hardware: pointing the unit up filled *lower*-tier boxes; achieving full coverage
required holding the unit upside-down and rolling it in a specific, non-obvious way;
slow rotation still sometimes moved the highlight in an unrelated direction. Two
compounding causes:

1. The reference axis (raw mag sensor +Z) has no relationship to gravity or the
   device's physical orientation — it's arbitrary, set by wherever Earth's local field
   happens to point relative to the chip. There was never a reason "pointing up" would
   correlate with it.
2. More fundamentally: **any (elevation, azimuth) parameterization of a sphere has a
   coordinate singularity at its poles** — azimuth becomes hypersensitive to small
   motions there, the same reason flat map projections distort near the poles. This is
   independent of which axis is chosen as the pole; relocating the reference axis only
   relocates where the singularity bites, never removes it.

The RMS this produced (2.2%, just over calibration-processor's 2.0% good/warn boundary)
is a real, not just cosmetic, consequence: the contorted motion needed to satisfy a
confusing display produced worse actual sphere coverage than a natural tumble would
have.

## Root cause, restated

The grid was never geometrically wrong. It was being asked to report a trustworthy
heading from an uncalibrated magnetometer. That's a data problem, not a display problem
— no amount of clever binning fixes it, because the thing being displayed (heading) is
genuinely not known yet. Fix the data, and the original grid — which was never actually
broken in shape, only in what it was fed — works again.

## The two-pass design (retired 2026-07-25)

**Pass 1 — ROUGH_SCAN.** No grid. The diver tumbles the unit with no spatial promises at
all — just three things that can't lie because they don't claim any orientation
correspondence:
- Per-axis (X/Y/Z) raw-range bars (0–100%, against `MAG_CAL_ROUGH_SCAN_EXPECTED_RANGE`
  — same constant and purpose as the legacy single-stage cal's `NB_EXPECTED_RANGE`).
- A sample counter.
- The live incremental 2-D fit readout (`fit_hdg_err_deg`, `fit_delta`) — unchanged from
  before, and the one signal that's stayed reliable across every attempt so far, since
  it doesn't depend on any orientation estimate.

BTN2 declares "good enough" (`ADVANCE_BASELINE_PASS`), gated at
`MAG_CAL_ROUGH_SCAN_MIN_SAMPLES` (40) so it can't fire on essentially zero data — below
that it's ignored and the screen says how many more samples are needed.

**Handoff.** `magBinCalAdvanceToCollect()` computes a rough hard-iron bias from
ROUGH_SCAN's running min/max (`(max+min)/2` per axis, identity soft-iron — deliberately
the cheap version, not a full on-device ellipsoid fit) and loads it via the existing
`imu::setMagCalibration()`. This is a UX aid, not a real calibration: it never touches
LittleFS, and `loadCalibration()` reloads whatever's actually on flash once the cal
session ends (success or otherwise), so it can't leak into normal navigation afterward.

**Pass 2 — COLLECT.** Unchanged from the original, pre-spike grid: same
`getBinIndex(pitch_deg, heading_deg, isMounted)`, same row-weighted completion, same
N/E/S/W and elevation labels, same rendering code. The only thing that changed is what's
feeding it — the *same* Mahony/AHRS pipeline used everywhere else in this codebase (same
axis conventions, including the documented mag Y-axis negation — see
[ahrs-orientation.md](./ahrs-orientation.md)) now has a roughly-debiased magnetometer to
work with instead of raw uncalibrated data, so its heading output is meaningfully more
useful for grid classification than it was pre-ROUGH_SCAN. No new heading-computation
code was written for this — reusing the existing, proven pipeline was a deliberate
choice over hand-rolling a fresh tilt-compensation formula, given this codebase's own
history with subtle mag-axis sign bugs.

Mounted cal is untouched — it starts directly in COLLECT, same as always. This redesign
is baseline-only.

## What's new vs. what's reused

| Piece | Status |
|---|---|
| `CalPhase::ROUGH_SCAN` | New enum value; `COLLECT` unchanged (dpvlink.h) |
| `CalProgressPacket.cov_x/y/z`, `.sample_count` | New fields, JSON keys `cx`/`cy`/`cz`/`sc` |
| `DisplayCmd::ADVANCE_BASELINE_PASS` | New command (BTN2 during ROUGH_SCAN) |
| `imu::magBinCalAdvanceToCollect()` | New — computes rough bias, calls `setMagCalibration()` |
| `imu::getBinIndex()` (COLLECT) | Reverted to original pre-spike AHRS-based logic, unchanged |
| `imu::magBinCalTick()` ROUGH_SCAN path | New — min/max tracking, sample accumulation, no bin logic |
| `display::showBaselineRoughScan()` | New screen — axis bars, sample count, fit readout, prompt |
| `display::showCalGrid()` | Reverted to original labeling, unchanged otherwise |
| Baseline `magBinCalIsComplete()` | Returns false during ROUGH_SCAN; COLLECT logic unchanged |

## Risks & open questions

- ~~**Not yet verified on hardware.**~~ **Tested on hardware 2026-07-25 and failed** (see
  the note at the top). The 2026-09-18 reference run (1.2% baseline, under 1° on-unit /
  1.6° on the website) used the flow that replaced it: a ROUGH_SCAN-only baseline, then
  gap-fill. An earlier edit credited that run to this design.
- **Rough bias is hard-iron only.** No soft-iron correction from ROUGH_SCAN — deliberate,
  to keep the bootstrap cheap and because hard-iron dominates uncorrected heading error.
  If COLLECT's heading still isn't good enough after this, a fuller on-device
  ellipsoid/PCA fit is the next lever (rejected for this pass in favor of testing the
  cheap version first).
- **`MAG_CAL_ROUGH_SCAN_MIN_SAMPLES = 40` and the axis-bar `MAG_CAL_ROUGH_SCAN_EXPECTED_RANGE
  = 6800` are untuned guesses** (the latter borrowed from the legacy single-stage cal's
  constant). Expect to revise after real-hardware feel, same as the row-weighted
  completion thresholds were originally tuned.
- **Axis bars aren't tied to physically-marked housing features.** "X/Y/Z" are the raw
  logical-frame axis names, not something a diver can act on directly ("tip the
  connector-end up more") without knowing the board's axis map. Worth revisiting if the
  bars turn out to be genuinely useful signal rather than just a rough sanity check.
  the diver mostly ignores in favor of watching the live fit number.
- ~~**Roll coverage is still not tracked anywhere**, in either pass.~~ **Closed — roll is
  now a tracked third coverage axis.** The gap was real (the original AHRS-based
  `getBinIndex` used pitch/heading only, and per-axis min/max bars don't surface roll),
  and it stopped being acceptable once a tilted-ring mounted fit was traced to it. Roll
  sectors, the 4-triangle roll widget, `binRollSatisfied()` as the single
  cell-is-finished predicate, and the server-side coverage sync all shipped; see
  `docs/dev-calibration-guide.md` and dive-map's
  `docs/architecture/calibration-grid-conventions.md`. What remains open is **steering**,
  not tracking: heading gain is `tan(pitch)`, so cells at |pitch| > 60° are not aimable,
  and near-vertical roll credit is largely noise — accepted as-is 2026-09-18, with the
  mitigation being technique (work the ~65° band edge, not the pole).

## Status

**Retired for baseline.** Pass 1 is all that's left of this design. What ships:

- **Baseline stays in ROUGH_SCAN for the whole session.** `magBinCalBegin(BASELINE)`
  starts there and never hands off. `magBinCalAdvanceToCollect()` and
  `ADVANCE_BASELINE_PASS` were removed. BTN2 now sends `FINISH_BASELINE_COLLECTION`
  (`magBinCalFinishBaseline()`), and the CSV uploads for the real fit and coverage grade.
  The screen (`showBaselineRoughScan()`) is the X/Y/Z bars, the sample count and the 2-D
  fit readout. In practice the diver ignores it and just tumbles.
- **The grid lives in gap-fill.** Once the baseline is graded, a gap-fill session takes
  the server's target map. It bins each sample from that sample's own mag + accel
  (`mag_orient::binIndexFor`), with no AHRS filter in the loop. That's the grid divers
  actually steer by.
- **Mounted still starts directly in COLLECT.**

The 2026-11-05 replay (`dive-map/calibration-processor/tools/baseline_bootstrap_replay.py`)
revisits a live baseline grid: an in-session sphere fit plus gap-fill's binning, so
neither cause of the 2026-07-25 failure is present. See
[baseline-live-grid-plan.md](./baseline-live-grid-plan.md), shipped in 0.7.25 and
verified on hardware 2026-11-07. ROUGH_SCAN survives as the few seconds before that
grid's bootstrap locks.
