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

- [ ] **The cal OFFLINE screen promises nothing is lost, then nothing retries**

  `showCloudCalOffline` (~`display.cpp:1362`), shown when there's no WiFi at cal completion,
  says "Raw samples are saved — nothing is lost" and stops there. There is no auto-retry
  when WiFi comes back and no pointer to tern.local, where the per-file "Upload to cloud"
  action actually lives. A diver who believes the screen never uploads the cal.

  *Where:* the retry path already exists — `runCalUploadAndNotify()` in `nav_main.cpp`
  behind `POST /api/cal/retry-upload`. Two independent fixes: put the tern.local pointer on
  the screen, and retry orphaned cal CSVs on network join the way `log_sync` already does
  for dive logs.

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

- [ ] **Two rough-scan constants are still untuned guesses**

  `MAG_CAL_ROUGH_SCAN_MIN_SAMPLES = 40` and the axis-bar
  `MAG_CAL_ROUGH_SCAN_EXPECTED_RANGE = 6800` were never tuned — the latter was borrowed
  wholesale from the legacy single-stage cal's constant. The two-pass flow has since been
  validated repeatedly on hardware, so there is now real feel to tune against.

  *Where:* `docs/baseline-cal-two-pass.md`, Risks. The row-weighted completion thresholds
  were tuned the same way after real use.

- [ ] **The PCB files don't show the backlight jumper**

  The display unit has a hand-run jumper from the EyeSPI `Lite` pad (pad 2) to A5/GPIO4,
  added and bench-tested 2026-09-18, and firmware drives it in `display::setBacklight()` /
  `sleepForPowerOff()`. Both the current `hardware/schematic/display_board/display_board.kicad_pcb`
  and "Rev A" still show `unconnected-(U1-Lite-Pad2)` — so reading the PCB alone leads to the
  conclusion that the backlight can't be controlled from firmware, which was true until the
  jumper and is now wrong.

  *Note:* trust `TFT_BL` in `board_pins.h` over the KiCad netlist for the assembled unit. The
  same session corrected `TFT_RST` 22 → 23, which was **not** a wiring change — IO23 ↔ EyeSPI
  pad 8 was always routed (net 4) and only the constant was wrong, so the panel reset line
  had been floating.

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

---

## Tier 4 — Long tail

Real, small, not urgent. Good candidates when something above is blocked on hardware or a dive.

- [ ] **Logging runs at full rate while the diver is stationary**

  Worst-case LittleFS sizing assumes a continuous ~2 hr dive at 1 row/sec (~500 KB), but a
  meaningful chunk of that is kitting up on the beach or holding position looking at a wreck,
  where DR isn't advancing and a row adds little. Gating on the existing flow-threshold signal
  (`DR_MIN_FLOW_SPEED_MS`, or speed cal's `SPEED_CAL_START_THRESHOLD_HZ` /
  `SPEED_CAL_STOP_THRESHOLD_MS` pair) would shrink the realistic worst case and buy partition
  margin without touching the LittleFS allocation.

  *Needs a decision:* whether HIGH-level logging should still log at rest — that mode is
  sometimes used specifically to debug stationary sensor behaviour.

- [ ] **Cal axis bars are labelled X/Y/Z, which a diver can't act on**

  They're raw logical-frame axis names, not something you can do anything with ("tip the
  connector-end up more") without knowing the board's axis map. Worth revisiting only if the
  bars turn out to be genuinely useful signal rather than a rough sanity check the diver
  ignores in favour of the live fit number.

- [ ] **The logging integration example hardcodes roll and pitch to zero**

  `docs/logging-integration-example.cpp:122-123` carries two literal TODOs to extract them
  from the AHRS quaternion. Small, but it's the file someone copies from — the real
  extraction already exists in the orientation code.

---

## Settled — do not re-open

Decisions already made, recorded so they stop costing time. If one of these comes back up,
the answer is here, not in fresh analysis.

- **Near-vertical roll noise in gap-fill** — *closed 2026-09-18.* "Noise near vertical is just
  a part of the game." Roll credit near vertical is largely noise and stays that way;
  `reconstructRoll`'s 1e-6 g degenerate guard never fires and returns 0.0 = sector 0 =
  UPRIGHT, so at true vertical the device credits "upright" rather than refusing. A gate
  (require cos(pitch) ≥ 0.1) would stop the grid claiming roll diversity it doesn't have, at
  the cost of rejecting samples the diver worked to collect. The mitigation is technique —
  work the ~65° band edge, not the pole — documented in `docs/calibration-guide.md`.

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

- **The two-pass baseline cal is shipped and validated.** `docs/baseline-cal-two-pass.md` used
  to read as an unshipped design; its stale claims were retired 2026-09-18. Roll coverage *is*
  tracked now, and the flow *has* been verified on hardware repeatedly.
