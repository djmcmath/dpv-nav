# DPV-Nav Backlog

Everything outstanding on the firmware side, consolidated 2026-09-18 from the scattered
TODO notes, the deferrals buried in `docs/*-plan.md` "Risks & open questions" sections, and
the markers left in source.

**Ordered by what bites a diver, not by what's easiest.** Tier 1 items lose work silently —
no error, no warning, just wrong or missing data discovered later. Everything else waits.

Dive Map has its own list at [`dive-map/docs/architecture/backlog.md`](../../dive-map/docs/architecture/backlog.md).
Items are assigned to whichever repo the work *starts* in; a few need a half on each side
and say so.

---

## Tier 1 — Foot-guns

Silent loss. The diver gets no error, just bad data later.

- [ ] **A new mag cal silently invalidates the heading cal**

  The Fourier fit maps *indicated → correction*. Accepting a new baseline or mounted cal
  changes what "indicated" means for the same physical bearing, so `hdg_fourier.json`
  becomes wrong — and nothing on the device or the server catches it. After any
  baseline/mounted change the 12-point heading cal must be **re-collected from scratch,
  never gap-filled**. Today that rule lives only in a doc and in the builder's head.

  *Wanted:* record which mag cal a heading fit was collected against, and have the unit
  refuse to apply a `hdg_fourier.json` whose parent cal is no longer installed. Needs a
  server half too — accepting a mag cal should mark the heading cal stale — but the unit
  is the enforcement point, because the unit is where a wrong heading gets displayed and
  acted on.

- [ ] **Mounted dwell capture: server-side acceptance run still owed**

  Built and bench-run 2026-10-11: a mounted cell fills only from a still dwell inside its
  row's attitude window (`src/util/mounted_dwell.h`), with an attitude indicator, motion
  bar and hold bar on screen. On the unit the stillness limits were workable as shipped
  and the screen did not flicker. The horizon's roll was backwards in the first build and
  is fixed; the horizon is now drawn as 2x-exaggerated error from the row's target so it
  reads from the far end of the scooter.

  *Owed:* the acceptance run in Part A of dive-map's
  [`mounted-cal-dwell-plan.md`](../../dive-map/docs/architecture/mounted-cal-dwell-plan.md):
  a server fit of one dwell collection, with within-dwell scatter near 0.4 µT. That needs
  dive-map's fit corrections deployed.

- [ ] **A cal can't tell whether it was collected with thermal compensation on**

  Compensation is applied inside the raw mag read, so cal samples are normalised to
  `ref_temp_c` when `/mag_temp.json` is loaded and not at all when it is absent. Nothing
  records which. A cal collected one way and used the other is off by the full thermal
  drift (0.65 µT/°C), silently, and the item below is how the file goes missing.

  *Done 2026-10-11:* `mag_temp_c` and `tcomp` on every cal CSV row (baseline, gap-fill,
  mounted, heading samples). *Still wanted:* a `thermal` block in the cal
  JSON, and a load-time mismatch warning. Part D of
  [`mounted-cal-dwell-plan.md`](../../dive-map/docs/architecture/mounted-cal-dwell-plan.md).

- [ ] **`mag_temp.json` is local-only and `uploadfs` erases it**

  Thermal-compensation coefficients are per-unit and cost ~4 hours of heat testing to
  produce, but they live on LittleFS only and are uploaded by hand through tern.local. A
  single `pio run -e nav -t uploadfs` or a board swap destroys them.

  *Where:* `src/net/cal_sync.cpp` — the synced-kinds table (`baseline`/`mounted`/`hdg` →
  active + `_sync.json` paths, ~line 28) and the backup table (`accel_cal_backup` /
  `gyro_cal_backup` / `speed_cal_backup`, ~line 46). Dive Map needs a matching kind/key.
  `reloadCalibrationFiles()` already reloads mag_temp, so an installed file hot-applies.

- [ ] **Forgetting to stop logging silently defers the whole dive**

  `log_sync` deliberately skips the log file currently open for writing — a partial upload
  and the finished file are different bytes, so the server can't dedupe them and the
  assembler sees two overlapping runs of one dive. That makes "stop logging, *then* turn on
  WiFi" load-bearing, and getting it backwards means the dive just doesn't upload until
  some later pass.

  *Wanted fix is UX, not a smarter uploader:* prompt on the unit — "you've been out of the
  water a while, stop logging?" All three signals are already on the nav board (`depth::`,
  GPS fix quality in `NavPacket`, DR position). Not designed yet.

- [ ] **The cal OFFLINE screen promises nothing is lost, then nothing retries** *(half done)*

  `showCloudCalOffline`, shown when there's no WiFi at cal completion, used to say "Raw
  samples are saved — nothing is lost" and stop there. Since the unified cal screens
  (0.7.26) it says the samples are on the unit and "Next: upload at tern.local", where the
  per-file "Upload to cloud" action lives. **Still open:** there's no auto-retry when WiFi
  comes back.

  *Where:* the retry path already exists — `runCalUploadAndNotify()` in `nav_main.cpp`
  behind `POST /api/cal/retry-upload`. Retry orphaned cal CSVs on network join the way
  `log_sync` already does for dive logs.

- [ ] **Thermal compensation has never been tested on the cold side**

  Every heat test warmed from 19–22 °C and the fit covers 21–61 °C die temperature. A dive
  colder than the calibration is extrapolation — and narrow-range coefficient extrapolation
  has over-predicted every time it's been checked here. The working assumption is that
  heating and cooling behave the same, which held within the warm range and is unverified
  outside it.

  *Where:* cold-weather test planned for winter 2026–27; validate with a HIGH log (raw
  columns, no cal-frame dependency). The same test also answers whether the drift follows
  die temperature under even cooling, or came partly from parts near the sensor heating
  locally under a gun. See `docs/mag-temperature-compensation.md`, open items 1–2.

---

## Tier 2 — Trust the numbers

Nothing is lost, but something is unverified, unvalidated, or invisible when it goes stale.

- [ ] **OTA bootloader rollback has never been tested**

  The full display + nav OTA path is built and bench-verified to 0.7.4, and a network drop
  mid-install recovers and retries correctly on both boards. The rollback-revert case — a
  bad image that has to be backed out by the bootloader — is still untested on both boards,
  and it is the one that matters when an OTA goes wrong in the field.

  *Suggested:* `version.h` 0.7.2 → USB-flash both → bump to 0.7.3, publish, Update from
  tern.local; then fault-inject. Interlocked with `log_sync`: `otaBlockedReason()` refuses
  an install during a log upload.

- [ ] **The NaN poisoning trigger was never found**

  A single non-finite value used to become permanent: Mahony's guards are all `<`/`>`
  comparisons, false for NaN, and `quatNormalize`'s `1/sqrtf(nan)` keeps the quaternion NaN
  forever. Guards shipped 2026-08-24 and a `[POISON]` one-shot detector dumps raw+cal values
  on the first non-finite value — but the original trigger was never identified, so nothing
  was deliberately repaired.

  *Two persistence paths survive reboots:* cal JSON writes `inf`/`nan` as literal text and
  reads them straight back (**grep cal files for `inf`, not just `nan`**), and NVS
  `pos_x`/`pos_y`. Next recurrence during cal, the `[POISON]` block names the source.

- [ ] **The nav↔display link has no framing or CRC**

  `lib/dpvlink/dpvlink.cpp:813` — packed binary is a raw struct memcpy with the comment
  "No framing/CRC yet — add COBS later." It is raw 3.3 V UART between two boards in a sealed
  housing; a single corrupted byte is silently a different reading. The bad RX solder joint
  that once broke display→nav commands is the reminder that this link does fail physically.

  COBS framing plus a CRC turns a silent wrong value into a detectable dropped packet, which
  the display can then show rather than trust.

- [ ] **A stale display-board PCB copy still doesn't show the backlight jumper**

  The display unit has a hand-run jumper from the EyeSPI `Lite` pad (pad 2) to A5/GPIO4,
  added and bench-tested 2026-09-18, and firmware drives it in `display::setBacklight()` /
  `sleepForPowerOff()`. **Mostly corrected — audited 2026-11-09:**
  - `display_board.kicad_pcb` / `.kicad_sch` — **fixed.** Lite pad on net 20
    `Net-(A1-IO4{slash}A5)`, routed on F.Cu and B.Cu, schematic wire present, committed.
  - `display_board_Rev_B/display_board_Rev_0_gopro_housing.kicad_pcb` — **fixed.** Same net,
    5 routed segments.
  - `display_board_gopro_housing.kicad_pcb` / `.kicad_sch` — **still wrong.** Lite pad on
    net 37 `unconnected-(U1-Lite-Pad2)`, with `A1-IO4{slash}A5-Pad10` dangling too.

  *It is one circuit, not three.* Net-list comparison: `display_board` and the Rev B file
  have **identical** net lists, and the *only* electrical difference between `display_board`
  and the top-level gopro file is this jumper — nothing else diverges anywhere. The housing
  changes the outline and the retention, not the connections: Rev B drops the two M3
  mounting holes (hence 9 Edge.Cuts items vs 8), and mounting holes carry no nets, which is
  why the net lists match. The top-level gopro file has the *same* outline and the same 9
  footprints as `display_board`, so it is not a housing variant at all — it is a stale copy
  that missed the fix.

  *Do now, cheap:* sync the jumper into `display_board_gopro_housing.*` so every copy is
  truthful, regardless of how the housing question lands. A stale file that reads
  "backlight uncontrollable" is the trap here, and it costs nothing to close.

  *Open, and NOT a file-naming decision:* which housing the display unit ships in. The board
  is expected to be the same either way — gopro housing or custom-milled Delrin — with the
  same connections. The gate is building **at least one fully operable milled-Delrin display
  unit** before committing to that path. Until that unit exists, keep both housing layouts
  alive; don't consolidate the PCB files on the assumption that one housing won.

  *Note:* trust `TFT_BL` in `board_pins.h` over the KiCad netlist for the assembled unit. The
  same session corrected `TFT_RST` 22 → 23, which was **not** a wiring change — IO23 ↔ EyeSPI
  pad 8 was always routed (net 4) and only the constant was wrong, so the panel reset line
  had been floating.

- [ ] **`water_temp_c` decoupled from the water for a whole dive, cause unknown**

  On 2026-11-08 (`tools/logs/20261108-001.csv`) the MS5837's temperature sat flat at
  14.2–14.4 °C for 90 minutes, including a descent to 35 m straight through a thermocline,
  with two clean exponential excursions (τ 9–23 s, both directions) to 10.47 °C and
  10.99 °C. **The excursions were the correct readings.** The same unit on the same
  unchanged firmware read 15.5–16.0 °C at the surface and 9.7 °C mean below 20 m on
  2026-10-24 and 2026-10-30, crossing the thermocline promptly both times.

  *Ruled out 2026-11-09:* the compensation math (checked against the MS5837-30BA datasheet
  — dT, OFF/SENS, the second-order branch, C5/C6 indexing), the log path (no filtering,
  int16 at 0.01 °C), and a firmware change (`git log` shows `ms5837.cpp` untouched between
  the good dives and this one). The I2C bus was healthy too — depth tracked all dive, so
  the D1 reads were landing. Depth is unaffected: at 69.1–69.6 min the temperature swung
  1.3 °C while depth held 34.72–34.79 m, and the compensation wants the die temperature,
  which is what it measures.

  *Also ruled out:* tracking the housing instead of the water. The reading was flat within
  0.2 °C while *both* candidate environments moved several degrees (water 14.5 → 10.5,
  housing via `mag_temp_c` 16.9 → 13.5), so it was following neither. The probe is on the
  stern of the nav tube and has not moved in many dives, which makes a trapped bubble
  possible but not obviously likely.

  *Done 2026-11-09, independent of the cause:* failed ADC reads are counted and dropped
  rather than recompensated as fresh samples, the counters are on `tern.local`'s
  Diagnostics panel, and conversions are paced at `ms5837::SAMPLE_PERIOD_MS` (500 ms, from
  100% ADC duty at OSR 8192) so self-heating no longer biases the reading warm.

  *Priority: low.* Nothing on the unit or on Dive Map uses this column. Left here so the
  next occurrence is recognized rather than re-investigated — and `tern.local` will now
  say whether the sensor was answering at all.

---

## Tier 3 — Build it out

Features and UX with a design already thought through.

- [ ] **Polar gap-fill cells can't be aimed at all**

  Heading gain is `tan(pitch)`: a 3° hand wobble moves reported heading 1.7° at pitch 30°,
  11° at 75°, and 56° at 88° — one to two whole 30° sectors from a tremor. Bands 0 and 4
  (|pitch| > 60°) are unsteerable in heading *by construction*, and those are exactly the
  cells gap-fill sends people to. The grid is a plate carrée projection, so a polar cell is
  **13.4%** of a level cell's solid angle (0.0701 vs 0.5236 sr) while being drawn the same
  width — equal screen size hiding wildly unequal difficulty.

  Everything else in gap-fill steering is fixed and bench-validated; this is the one
  remaining cause, and faster feedback will never substitute for it — that was tested.

  *Fix:* fewer heading sectors in bands 0/4 (4 × 90° also matches their solid angle), or one
  "nose up, any direction" target per polar band. **Cross-system wire/bin-ordering change** —
  read `dive-map/docs/architecture/calibration-grid-conventions.md` first; touches
  `coverage.py`, `mag_cal_orient`, `device.rs`, `CoverageHeatmap` and the packet arrays
  together.

- [ ] **Thermal cal is 4 unprompted hours — decide the builder story**

  Producing `mag_temp.json` today means heat-gunning the board at each cardinal, logging
  MID/HIGH, waiting 30–60 min per heading to cool, then running `tools/mag_temp_fit.py` on a
  laptop. Fine for one person; a builder will not do it. **Open decision:** a pre-ship step
  performed on every board, or part of the builder's initial cal flow with device prompts
  (like `CAL > Speed cal`'s phased UI), a cloud fit, and a shorter protocol.

  *What shortens it:* the drift is body-fixed and identical at all four headings
  (±0.03 µT/°C), so **one heading may suffice** — the four are a consistency check, not a
  requirement. HIGH logs carry raw columns, so no cal-frame dependency, and the cooling leg
  alone gave the fit. Stakes: drift reaches ~2.2°/°C of heading.

- [ ] **Nothing tells the diver the displayed position has gone stale**

  Depth changing, flow ≈ 0, for N minutes means DR isn't advancing and the position on the
  screen is drifting away from truth. The 2026-09-12 diver had no indication of this through
  35.8 minutes and 205 m of it. Bonus: the same condition turns every deco hang into a free
  current measurement.

  *Needs:* explicit accumulated-error/confidence state in firmware — the existing
  GPS-fix-trust decay gate is a different thing.

- [ ] **Targeted heading resampling still means manual transcription**

  Phase A shipped end to end — thin-sector detection, the endpoint, and the editable-rows
  manual-entry form. Phase B is the firmware round trip: server-synced targets pushed to the
  unit reusing `CAPTURE_HDG_POINT`, so the diver is steered to the specific bearings that are
  thin instead of transcribing them by hand. Deliberately sequenced after real-world use of
  Phase A.

  The final on-unit cal already lands at 1–2° at all check points, so this is friction, not
  correctness — the trigger to build it is that friction resurfacing.

- [ ] **Landmarks never reach the unit**

  The diver's local `waypoints::` list and the server's curated landmarks are entirely
  separate worlds. A new `cloud_client.h` call — `fetchNavAidsForSite(...)` mirroring the
  existing `fetchCalibrationStatus()` / `downloadUpload()` device-auth pattern — would
  populate a synced landmark cache distinct from the local list.

  *Blocked on* a `dive_sites` table existing in Dive Map: the device needs to ask "landmarks
  for the site I'm diving," and there's nothing to ask about yet. Follow-on: sort the
  downloaded list (or the existing `WaypointListPacket`) nearest-first before sending
  nav→display, reusing the full-screen waypoint picker in `display_main.cpp`. For line
  landmarks "nearest" means nearest-point-on-line — the firmware waypoint model is point-only
  today.

- [ ] **Speed cal can't be run underwater**

  Since 2026-10-29 the whole Cal submenu is hidden in dive mode, Speed cal included, to
  keep the at-depth menu short. The on-unit speed cal is still the rough first pass (unit
  on a stick, walked back and forth in shallow water a few times; close but not right); the
  real cal is long straight legs between known GPS points, done post-dive. If an on-unit
  underwater run is wanted again: an "advanced" / "allow cal underwater" switch set from
  the web page, which un-hides Speed cal (only) in dive mode. Undecided which.

- [ ] **Magnetic declination is a hard-coded constant**

  `DEFAULT_DECLINATION_DEG` in `config.h` (positive = East) is baked in at build time.
  14.7°E suits Lake Washington (its comment still says southern Oregon). A unit taken
  anywhere else shows MAG headings and MAG target bearings off by the difference. Wanted: look
  it up from lat/lon. Options: a public geomagnetic API (NOAA's calculator needs a key),
  computed on dive-map and synced like the cal, or the World Magnetic Model (WMM) evaluated on
  the device from the last good GPS fix, which works offline. The value would live in NVS and
  update at the surface. The WMM option needs no network, which suits a unit that is often
  offline.

- [ ] **Mounted cal: do the tilted rows earn their time?**

  Every mounted cal so far has filled the whole +30 and −30 rows, not just the 6 spread
  cells per tilted row the firmware needed until 0.7.26, and the unified cal screens
  raised auto-finish to all 36 cells (`MAG_CAL_SECTORS_MID` = 12, `kNeedTotal` = 36) to
  keep encouraging that, with FINISH available at any time. Nobody has checked whether the
  extra cells change the result.

  What the server does with them: the mounted correction is fitted from the **level row
  only** (≥ `MIN_MOUNTED_LEVEL_SAMPLES` = 30 still, level samples, no azimuth gap over
  `MOUNTED_LEVEL_MAX_GAP_DEG` = 90°, in dive-map's `calibration-processor/callib/fit.py`).
  The tilted rows only free the vertical hard iron b_z in
  `compute_mounted_correction_body`, which needs `MOUNTED_BODY_MIN_TILTED_SAMPLES` = 36
  still, tilted samples, about two dwells. The first real refit after that shipped
  (2026-10-23) found b_z ≈ 0 on this unit.

  *Analysis:* refit past mounted CSVs with the tilted rows thinned (all 12, the 6-spread
  rule, two dwells per row, none) and compare b_z, its standard error, and the heading
  residuals at tilt. If thinning changes nothing, drop `MAG_CAL_SECTORS_MID` in
  `config.h` back to 6 or lower (the dwell tests are written in terms of it) and tell
  divers they can stop early. The analysis runs in dive-map;
  the rule it decides lives here.

- [ ] **Calibration walkthrough in the user guide**

  The unified cal screens make calibration one process (1 Baseline, 1a Fill gaps, 2
  Mounted, 3 Heading) with the same frame and buttons throughout. `user-guide.md` should
  walk a new owner through it screen by screen: what each step is for, what to do, what
  "done" looks like, and what each result means.

  *Images:* `tools/cal_screen_mock/` renders every screen (`node
  tools/cal_screen_mock/render.js` → `png/`). `screens.js` was brought in line with
  `display.cpp` when the redesign was built (0.7.26), so they're ready to use; re-check
  against the shipped build first. Unblocked once 0.7.26 is out.

---

## Tier 4 — Long tail

Real, small, not urgent. Good candidates when something above is blocked on hardware or a dive.

- [ ] **Nothing frees space for a log once it is open**

  `cleanupOldLogs()` runs at boot only, and only until 32 KiB is free. A log that outgrows the
  free space drops rows silently: `logging.cpp` never checks a write's result. A two-hour
  HIGH or ungated MID log is ~540 KiB of the 768 KiB partition, so old logs have to be
  cleared by hand first. Wanted: when a log opens, free what its level could need, deleting
  already-uploaded logs first (`log_sync::isUploaded`), and warn on the display if it can't.

- [ ] **The status LED says nothing useful in dive mode**

  *Low priority.* The LED is tested and works (see Settled). In dive mode it only indicates
  that it's in dive mode — correct, and arguably enough, since it isn't the primary display
  or indicator and nothing is lost by it staying quiet. But it's an always-visible channel
  that's currently idle for the whole dive, and there is probably *something* worth putting
  on it. Open question is what: candidates are log-write health (the Tier 4 item above fails
  silently today), remaining log space, battery state, or a cal/sensor fault that the diver
  would otherwise only find on the display. Pick one that matters without needing to be read
  as a legend at depth — see the "minimize presses / preserve awareness" principle.

- [ ] **The logging integration example hardcodes roll and pitch to zero**

  `docs/logging-integration-example.cpp:122-123` carries two literal TODOs to extract them
  from the AHRS quaternion. Small, but it's the file someone copies from — the real
  extraction already exists in the orientation code.

---

## Settled — do not re-open

Decisions already made, recorded so they stop costing time. If one of these comes back up,
the answer is here, not in fresh analysis.

- **Nav-board status LED works as designed** — *bench + in-water tested, closed 2026-11-09.*
  Built 2026-10-14 (`util/status_led.h`, legend in `docs/user-guide.md`), now fully tested.
  GPS indication works well, and dive/surface mode reads correctly (indirectly). No rework
  owed; the only remainder is the open Tier 4 question of what else it could report in dive
  mode.

- **Mast-mounted GPS indicator board — built, not needed** — *2026-11-09.* A second NeoPixel
  indicator board was built and works correctly, but reading the NeoPixel on the nav board
  is easier than looking up into the mast, so it earns nothing in practice. Good build, not
  worth integrating or maintaining. Don't re-propose remote/mast indicators unless the nav
  board itself becomes hard to see.

- **Near-vertical roll noise in gap-fill** — *closed 2026-09-18.* "Noise near vertical is just
  a part of the game." Roll credit near vertical is largely noise and stays that way;
  `reconstructRoll`'s 1e-6 g degenerate guard never fires and returns 0.0 = sector 0 =
  UPRIGHT, so at true vertical the device credits "upright" rather than refusing. A gate
  (require cos(pitch) ≥ 0.1) would stop the grid claiming roll diversity it doesn't have, at
  the cost of rejecting samples the diver worked to collect. The mitigation is technique —
  work the ~65° band edge, not the pole — documented in `docs/dev-calibration-guide.md`.

- **TLS CA pinning removed, not made refreshable** — *2026-08-27.* `CLOUD_ROOT_CA_PEM` and
  `rootCaConfigured()` are gone; every `WiFiClientSecure` in `cloud_client.cpp` calls
  `setInsecure()`. The connection is still encrypted; the server's chain is not validated.
  Chosen over building CA-rotation machinery for non-sensitive dive data — a Let's Encrypt
  root rotation would otherwise brick every deployed unit until someone finds it and plugs in
  USB, which is the exact failure mode cloud sync existed to avoid.

- **Finning is not a supported speed-cal mode.** No real-time motor signal exists on the unit,
  so the 6 m/min gate is the deliberate substitute. Note that speed cal `k` is a *divisor* —
  smaller k means faster — the cross-section cancels, and bad history entries must be reset,
  never averaged out.

- **float32 lat/lon in the logs** — *deferred deliberately, not an oversight.* 0.43 m of
  quantization (3.81e-6 deg at 47.7 N) against a 20–50 m typical position error at a wreck.
  `pos_x_m`/`pos_y_m` are finer, so the loss is only in the lat/lon conversion. **The one
  exception worth raising:** a *calibration* dive, where small distances get back-solved from
  GPS. A per-log opt-in — double, or fixed-point offsets from a log-header origin — is the
  shape wanted there.

- **Load-state magnetic offset** — *measured, too small to act on.* A repeatable 0.38 µT in
  |B| and 0.33° in heading between power states, sign reversing on all four edges, with the
  board stationary (pitch spanning 0.10°, roll 0.08°). Genuine and current-dependent, but a
  single calibration can only absorb it in one state and it isn't worth compensating.
  Separately: "shutting off WiFi makes a small difference" was **not** evidence for anything
  before 2026-09-16, because `TOGGLE_WIFI` never moved the radio — it only flipped
  `gWifiEnabled` and wrote NVS. That's since fixed, so a menu WiFi toggle is now a valid load
  test.

- **Baseline has a live grid** — *shipped 0.7.25, hardware-verified 2026-11-07.* An
  in-session sphere fit locks a few seconds into the tumble and the grid takes over;
  see `docs/baseline-live-grid-plan.md`. On the first real session the transition
  was clean, and the collection was "meaningfully useful" rather than blind. Baseline,
  then gap-fill, gave a really good baseline; mounted and the 12-point heading cal
  followed normally, finishing within 1–2° at every test point. This retires two old
  items: the rough-scan constants (`MAG_CAL_ROUGH_SCAN_MIN_SAMPLES`, `_EXPECTED_RANGE`)
  and the X/Y/Z bar labels. The bars are now only the few seconds before lock, so
  neither is worth tuning.

- **The two-pass baseline cal was retired, not shipped.** Corrected 2026-11-05: this entry
  used to say the opposite. Baseline is a ROUGH_SCAN-only tumble (BTN2 sends
  `FINISH_BASELINE_COLLECTION`), and the live grid lives in gap-fill. That's the flow
  verified on hardware, including the 2026-09-18 reference run. Roll coverage *is* tracked,
  in gap-fill and server-side. A live baseline grid is reopened on new evidence; see the
  live-grid entry above. That doesn't reopen the 2026-07-25 design: the Mahony-fed COLLECT grid and
  the min/max handoff stay retired.
