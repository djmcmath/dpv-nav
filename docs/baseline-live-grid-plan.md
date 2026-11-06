# Baseline Live Grid (Plan)

During the baseline tumble, show the same 60-cell grid and roll widget gap-fill uses. It
should appear a few seconds into the tumble, so the diver can steer toward gaps while
still in the baseline. Upload, server grading and gap-fill don't change.

Why now, after [baseline-cal-two-pass.md](./baseline-cal-two-pass.md) retired the last
attempt: that attempt had two problems, and both are now solved. Gap-fill already bins
each sample from that sample's own mag + accel, with no AHRS filter, and it works on
hardware. A replay of real baseline uploads shows an in-session sphere fit is good enough
to bin with after 50–75 samples. The remaining gap was the bias to bin with before any
calibration exists.

## Evidence (2026-11-05)

`dive-map/calibration-processor/tools/baseline_bootstrap_replay.py`, five prod baseline
uploads, binned through the server's own `assign_bins`, against each upload's accepted
fit:

| Run (cal id, unit) | Lock at | Bias err at lock | Same cell | Adjacent | Further |
|---|---|---|---|---|---|
| `9471b121`, c807 (09-18 ref) | 50 | 1.0 µT | 89% | 11% | 0.1% |
| `b423759c`, c807 (10-10) | 75 | 1.5 µT | 90% | 9% | 0.1% |
| `6858d148`, 3365 (10-15) | 75 | 1.3 µT | 89% | 11% | 0.1% |
| `e58990ba`, 3365 (10-20) | 75 | 3.4 µT | 85% | 15% | 0.1% |
| `76bf0382`, 3365 (10-15, graded 5.2% bad) | 75 | 5.8 µT | 81% | 18% | 1.1% |

- **Lock time.** 50–75 samples is about 5–8 s at the 10 Hz mag rate.
- **The misses are next-door cells.** With identity soft iron, agreement tops out around
  91% even with every sample in the fit. What's left is samples near a 30° boundary.
- **The bad run is a lower bound.** Its own truth fit is the poor one.
- **End-of-run gap lists.** The cells the bootstrap grid would still flag overlap the
  server's by about 90%. A full baseline currently leaves 115–159 of 240 (cell × roll)
  cells thin or empty, which is why gap-fill is never optional.
- **Min/max is not usable.** The min/max bias ROUGH_SCAN tracks today is 15–25 µT off
  until the last third of a tumble, which means 20–40° of median heading error.
- **Gyro heading was not tested.** The cal CSVs carry no timestamps to integrate, and it
  isn't needed.

## Design

```
ROUGH_SCAN (pre-lock)  --lock-->  BASELINE_GRID  --BTN2 / buffer full-->  dump + upload (unchanged)
  bars screen, every sample stored     gap-fill's grid + roll widget
  sphere refit every 25 samples        sphere refit every 25 samples; re-bin all if bias moves
```

1. **Pre-lock is today's ROUGH_SCAN.** Every non-duplicate sample is stored. The bars
   screen gets one line: "Finding north — keep tumbling".
2. **Refit every `MAG_CAL_BOOT_REFIT_EVERY` (25) stored samples.** Fit a 4-parameter
   linear least-squares sphere over all stored samples: `|m|² = 2m·b + c`, with
   `r² = c + |b|²`. Recompute from the buffer each time (≤2160 samples, mean-centered,
   in `double`) rather than keeping running sums. That's simpler, and float32 sums of
   `|m|²·x` lose precision at these magnitudes.
3. **Lock** when all three hold:
   - N ≥ `MAG_CAL_BOOT_MIN_SAMPLES` (50).
   - `|r / r_expected − 1| < MAG_CAL_BOOT_RADIUS_TOL` (0.10). Here `r_expected` =
     `MAG_CAL_EXPECTED_FIELD_UT` × `g_mag_lsb_per_uT`.
   - The bias moved less than `MAG_CAL_BOOT_STABLE_UT` (2 µT) since the previous refit.
4. **On lock:**
   - Set `g_binCalBinningCal = {bias, identity}`.
   - Switch the sub-phase to `BASELINE_GRID`.
   - **Re-bin every stored sample**, so the pre-lock tumble isn't wasted.
5. **After lock:**
   - Each tick bins the sample through the same per-sample path gap-fill uses
     (`mag_orient::reconstructOrientation` / `binIndex` / `rollSector`).
   - Accept it unless it's a duplicate or its (cell, roll sector) is at the ceiling.
   - Keep refitting every 25 samples. If the bias has moved more than
     `MAG_CAL_BOOT_REBIN_UT` (3 µT) from the one the counts were made with, re-bin
     everything. That self-corrects a bad early lock. In a normal tumble the estimate
     wanders about 1.5 µT, so this should rarely fire.
6. **Finish is unchanged.** BTN2 sends `FINISH_BASELINE_COLLECTION`, a full buffer
   auto-finishes, and the same `mag_baseline_samples.csv` uploads to the same endpoint.
7. **No lock means today's behavior.** If the gate never passes (wrong site field, a
   diver who only spins), the session stays on the bars screen.

**When a cell is finished.** Use `binRollSatisfied()`, the gap-fill predicate, with this
session's counts only: upright ≥ `MAG_CAL_GAPFILL_TARGET_CAP` (15) and each other roll
sector ≥ `_OTHER_ROLL` (5). With no server roll targets loaded,
`rollSectorServerSatisfied()` already returns false. That gives one predicate, so the
grid colour, the "N/60 cells" count and the roll widget can't disagree. No
auto-complete: the polar rows are unaimable (Tier 3, "Polar gap-fill cells"), so the
diver decides when to stop.

**Acceptance ceiling.** This is the one change to *what gets collected*. Today baseline
accepts everything and the server down-weights crowded cells (`bin_weights`). A
per-(cell, roll) ceiling of 2× the finished threshold (30 upright / 10 other) keeps easy
cells from filling the 2160-sample buffer before the hard cells are reached, which gets
likelier once the diver is chasing gaps. **Tune it with the replay before hardware**
(step 2 below). If the capped set grades worse, fall back to accept-all.

**Why the radius gate, and its limit.** A single-axis spin puts the samples on a flat
ring. A sphere fit on a ring is degenerate: any center on the ring's axis fits. The
min-norm answer is the ring itself, with radius = the horizontal field (0.39× at 67°
dip), so the radius gate rejects it. A real wobble of 10° or more usually fits
correctly. The gate *bounds* a false lock but doesn't prevent one: in synthetic tests, a
large ring close to a great circle can pass a 10% radius check while still ~20 µT off.
The self-heal re-bin is the backstop. Neither the fit's own standard error nor
gravity-direction spread separates the cases: the first reports ~1.4 µT on a 46 µT-wrong
ring, and real tumbles score as low as spins on the second.

**Expected field.** `MAG_CAL_EXPECTED_FIELD_UT = 50.1` sits next to
`DEFAULT_DECLINATION_DEG` in `config.h`. It's site-specific in the same way and has the
same story. 50.1 µT = the 3420–3444 counts every 2026-09..10 accepted baseline fit came
out to, on both units, at 68.42 LSB/µT. At another site, the WMM total intensity is
close enough for a 10% gate, or the grid simply never appears (safe failure). Learning it
per unit from the last accepted fit is a follow-up.

## Changes by file

### Nav (`dpv-nav`)

- **`src/util/sphere_fit.{h,cpp}` (new, no Arduino dependencies, so it host-compiles):**
  - `fitSphere(const int16_t* xyz, size_t strideBytes, int n, SphereFit&)`. The stride
    lets it read `&g_binSamples[0].x` with `sizeof(BinCalSample)` directly.
  - A small lock/re-bin state machine: `onRefit(fit, n, lsbPerUt) → {NONE, LOCKED,
    REBIN}`, with parameters from `config.h`. The host test drives this exact code.
- **`src/config.h`:** the `MAG_CAL_BOOT_*` constants, `MAG_CAL_EXPECTED_FIELD_UT`, and
  the ceiling multiplier.
- **`src/sensors/imu.cpp`:**
  - `BinCalSubPhase::BASELINE_GRID`.
  - The ROUGH_SCAN tick gains the refit cadence and the lock transition. The buffer-full
    auto-finish must exist in both baseline sub-phases.
  - **Factor gap-fill's calibrate → reconstruct → bin → roll-sector block into one
    helper** used by both GAP_FILL and BASELINE_GRID. Keep its "which cal" comment. Two
    copies of the orientation code is how highlight and grade drift apart.
  - `rebinAll()`: recompute every stored sample's bin with `g_binCalBinningCal`, then
    rebuild `g_binCounts`, `g_binRollCounts` and the per-bin last-sample dedupe state.
  - `magBinCalGetProgress` / `fillLiveOrient`:
    - Report `CalPhase::BASELINE_GRID`.
    - `bins_green` counts cells where `binRollSatisfied` holds.
    - Set `has_cell_satisfied` and fill `cell_satisfied[]`.
    - Fill the roll-widget fields for BASELINE_GRID as well as GAP_FILL.
  - `magBinCalFinishBaseline`: count `g_binSampleCount` for baseline too.
    `g_roughScanSampleCount` stops at lock.
  - `magBinCalBegin` / `magBinCalEnd`: reset the bootstrap state and the binning cal.
  - **Per-sample diagnostics in the CSV.** Two trailing columns, `dev_bin` and
    `dev_roll`: the cell the device assigned when it accepted the sample, −1 before lock,
    never rewritten by a re-bin. Both server loaders read the leading columns by position
    and ignore the rest (`coverage.load_csv_9axis` takes `[:, :9]`, `fit.load_csv` takes
    `[:, :3]`). That makes Saturday's session self-scoring against the server's binning
    with no Serial access. The cost is 2 bytes per sample (+4.3 KB of heap during
    collection). The buffer is freed before upload, so this doesn't reopen the 0.7.18
    WiFi-heap issue, but check `heap_free` at session start.
  - Log lock, refit and re-bin events (N, bias, r, how long the refit took). Use
    whatever path is readable on a sealed unit (check `util/boot_log`), not Serial only.
- **`lib/dpvlink/dpvlink.h`:** `CalPhase::BASELINE_GRID = 3`, with a comment pointing
  here.
- **`lib/dpvlink/dpvlink.cpp`:** Split the encoder gate that currently wraps `tg` /
  `rc` / `rt` / `rs` / `dn` under `GAP_FILL && has_targets`. `tg` stays gap-fill-only.
  `rc` / `rt` / `rs` / `dn` go out for gap-fill-with-targets **or** BASELINE_GRID. The
  decoder doesn't gate on phase; confirm that.

### Display

- **`src/display_main.cpp`:**
  - `renderCalProgress`: BASELINE_GRID → `showCalGrid(pkt, "BASELINE CAL")`.
  - The BTN2 finish condition (~line 1399) gains BASELINE_GRID.
  - Check the DONE-hold and finishing-screen paths, which go through `renderCalProgress`.
- **`src/drivers/display.cpp` `showCalGrid`:** a baseline-grid mode alongside
  `isGapFill`.
  - Every cell is a target. Green = `cell_satisfied` (count fallback for an old nav
    build), yellow = some samples, red = none.
  - Status line `N/60 cells`.
  - Line 2 and the roll widget show for both modes. Today they're gated on `isGapFill`.
  - Update the stale heading-label comment, which still talks about the COLLECT
    bootstrap.
  - Check the screen redraws cleanly on the mid-session switch from the rough-scan
    screen. `showCalGrid`'s first static draw does `fillScreen`.
- **`showBaselineRoughScan`:** add the lock hint and drop the 2-D "Fit:" line. Dive
  Map's `baseline-cal-coverage-feedback-plan.md` found that number settles 20–30° wrong
  on good datasets, and it would read like a grade for the grid.

### Dive Map

- **No server change.**
- **`tools/baseline_bootstrap_replay.py`:**
  - Add `--ceiling` (for step 2).
  - When `dev_bin` / `dev_roll` are present, score them directly against `assign_bins`
    under the accepted fit (for step 8).

### Docs, at ship time

- `CLAUDE.md`: the Baseline bullet (~209), which says "No live grid", and the gap-fill
  "Unlike baseline cal" bullet (~243).
- `calibration-guide.md` and `user-guide.md`: the baseline procedure.
- `mag_cal_orient.h`: the "WHY THIS IS NOT THE SEVENTH FAILED ATTEMPT" block. Its
  precondition, "runs ONLY with a good baseline cal", now has a second form: a locked
  in-session bootstrap.
- `imu.cpp`'s bin-cal header block, the `CalPhase` comments, and the
  `baseline-cal-two-pass.md` Status.
- `tools/README.md` host-test table, the release notes, and the backlog item: close it,
  and close or retire the two rough-scan items it makes moot.

## Implementation order

1. **Fixtures, the fit module and the host test.** Check the five replay CSVs and their
   fit JSONs (~580 KB) into `tools/baseline_boot_test/fixtures/`. The IDs are in the
   table above; the replay tool's docstring has the query.
   `tools/baseline_boot_test/run.sh` compiles `sphere_fit.cpp` + `mag_cal_orient.cpp`
   and asserts:
   - Each good run locks at 25–100 samples, with bias within 4 µT of its fit.
   - Live same-cell ≥ 85% and non-adjacent ≤ 1% against bins from the fixture fit.
   - Lock N matches the Python replay exactly at the same parameters, and bias matches
     to 0.1 µT.
   - A zero-wobble single-axis spin (about the vertical and a horizontal body axis)
     never locks.
2. **Tune the ceiling in the replay.** Simulate the ceiling on the five runs. Report how
   many samples it rejects and the `fit_baseline` RMS% of the capped set against the
   uncapped one. Pick the multiplier, or accept-all.
3. **`imu.cpp`:** the sub-phase, the binning-helper refactor, `rebinAll`, the progress
   fields and the CSV columns.
4. **`dpvlink`:** the phase value and gates. Extend `dpvlink_test` with a BASELINE_GRID
   packet that round-trips and fits the 512-byte buffer with a full grid. The gap-fill
   packet is ~319 bytes today, and this one drops `tg`.
5. **Display:** dispatch, BTN2, the `showCalGrid` mode and the rough-scan hint.
6. **Gates:**
   - `pio run -e nav` and `pio run -e display`.
   - `orient_equivalence.py`, `gapfill_chain_check.py`, `frame_fixture_check.py` and
     every `tools/*_test/run.sh` still pass. The gap-fill refactor must not change
     gap-fill.
7. **Docs, the version bump (0.7.25) and release notes.**
8. **Hardware (Saturday)**, below.

## Hardware test (Saturday)

Both boards change, so flash both, then **confirm `FW_VERSION` in the boot log before
anything else.**

1. **Tracking.** Start a baseline. Time the lock (target ≤ 10 s of tumbling). Then:
   - Level, nose at magnetic N/E/S/W by phone compass (set it to magnetic, or subtract
     14.7°): the highlight is in that column, ±1 cell at a boundary.
   - Pitch up 45°: the highlight moves up a row.
   - Roll 90° at level: the roll widget sector follows.
2. **A full session, steering by the grid.** Note whether the buffer fills, and whether
   any self-heal re-bin fired (the event log).
3. **Grade.** Upload, then read the server result:
   - RMS% should be no worse than recent baselines (1.0–1.4%).
   - Count the thin/empty cells. Success is materially below 115–159 of 240.
4. **Agreement.** Run the replay on the uploaded CSV. It scores `dev_bin` / `dev_roll`
   directly. Target ≥ 85% same cell and ≤ 1% non-adjacent.
5. **Degenerate start.** A second baseline that opens with 20 s of level yaw spin only,
   then tumbles. Expect no lock during the spin, or a re-bin once tumbling starts. The
   grid must not stay wrong after the tumble.
6. **Gap-fill afterwards.** Count the targets left, against the usual load after a blind
   baseline.

## Risks & open decisions

- **The ceiling changes the data.** That's why step 2 runs before any firmware.
  Accept-all is the fallback.
- **False lock on near-great-circle data.** The radius gate bounds it only loosely. The
  self-heal re-bin is the backstop, and test 5 exercises it.
- **Site-specific expected field.** Outside ±10% of 50.1 µT the grid never appears and
  the screen stays on the bars. That's safe, but the feature is invisible to a builder
  elsewhere. Follow-up: learn `r_expected` per unit from the last accepted baseline.
- **CPU.** A refit is ≤2160 samples of `double` arithmetic every ~2.5 s. A re-bin is
  ≤2160 `reconstructOrientation` calls at lock and on rare re-bins. Estimated at tens of
  ms; log the duration on hardware. The AHRS isn't in this path, but the link packets
  are.
- **Inherited:** polar cells are unaimable in heading (Tier 3); near-vertical roll credit
  is noise (Settled). The baseline grid takes on both as-is.
- **The heading labels are magnetic.** It's the same frame as gap-fill and the server
  grid, so they're consistent. Just remember it when checking against a phone.

## Status

Planned 2026-11-05. Build on 2026-11-06, hardware test on 2026-11-07.
