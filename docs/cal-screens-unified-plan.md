# Unified Calibration Screens (Plan)

Make the four calibration steps look and work like one process. Baseline, Fill gaps,
Mounted and Heading each grew their own title style, progress location, button hints and
accept/reject layout: four title styles, about eight phrasings of "press a button", and
accept/reject drawn anywhere from 8 px text to 154×112 px boxes. This plan puts all four,
plus the shared upload and result screens, into one frame with one set of button rules.

Designed with the user 2026-11-09. Mockups: <https://claude.ai/artifact/UNkMxYdHoWi5kpYHdDxcpX>,
source in [`tools/cal_screen_mock/`](../tools/cal_screen_mock/). The mock draws every screen
at 320×240 with the firmware's own GFX font, so the coordinates below are real pixel
positions, not sketches.

Out of scope: Speed cal and Current hold (speed cal has its own backlog item for an
in-water layout), and the legacy `showCal()` for cal_mode 0/1.

## Decisions (2026-11-09)

- **Four steps, presented as three:** 1 Baseline, 1a Fill gaps (optional), 2 Mounted,
  3 Heading. The 1a token shows on every screen.
- **One frame on every cal screen:**

  | Band | y | Content |
  |---|---|---|
  | Header | 0–23 | Step strip (current step on a cyan box), step name in cyan, counter right-aligned in white, cyan rule at y 23 |
  | Work area | 24–183 | The step's own widgets, the only band that differs between steps |
  | Status | 186–209 | One instruction at size 2, with a dim rule at y 184 |
  | Keys | 212–239 | Two softkeys, each 156×26 with radius 5: left at x 2 (BTN1), right at x 162 (BTN2) |

- **The header counter** counts toward whatever ends the step: samples to 2160 on
  Baseline, target cells on Fill gaps, cells to 36 on Mounted, marks to 12 on Heading. On
  closing screens it names the phase instead: UPLOAD, RESULT, SENT, OFFLINE or FAILED.
- **Status colors:** red means blocked, yellow means adjust, green means good. On closing
  screens the status line says what comes next ("Next: …").
- **Softkeys** (BTN1 is physically left and BTN2 right; confirmed). A key's label says
  what that button does right now. Key styles:
  - *blank*: dim outline, no label. The button does nothing here.
  - *dim*: the action isn't available yet.
  - *normal*: gray outline, white label.
  - *go*: green fill. Used only for MARK when the level gate is ready.
  - *hold*: gray label that fills red while held.
- **Buttons:** pressing BTN2 keeps going (MARK, FINISH, ACCEPT, OK). Holding BTN1 for
  `LONG_PRESS_MS` (2 s) throws the work away (QUIT, REJECT), and the left key fills while
  it's held. A short BTN1 press flashes "Hold BTN1 to quit" (or "…to reject") in the status
  line for 1.5 s. Every cal screen claims both buttons.
- **Minimum text size:** instructions, numbers and key labels are size 2 or larger. Axis
  ticks and the counts inside cells stay at size 1.
- **Results:** the number is drawn in the band color, with GOOD, OK or POOR next to it. The
  server's recommendation text is no longer shown on the unit.
- **Baseline** ends on FINISH or when the 2160-sample buffer fills. A full grid never ends
  it.
- **Mounted** gets FINISH at any time once one cell is filled, and auto-finishes at all 36
  cells. Every empty cell is red. Whether the tilted rows are worth filling is a backlog
  analysis ("Mounted cal: do the tilted rows earn their time?"); until that's done, fill
  them.
- **Grid guidance stays broad:** "Fill red & yellow cells". A per-cell or per-roll hint
  flickers, because rolling the unit moves heading and elevation too.
- **Heading:** the target is the main number. The unit's own reading is smaller, gray and
  labeled INDICATED HDG, under "Align to external compass", so divers don't try to make the
  two numbers match.
- **Sizes:** grid rows are 24 px (Baseline, Fill gaps) and 20 px (Mounted). Both horizons
  are 80 px.
- **Accepting a baseline on the unit downloads its gap-fill map straight away.** If that
  fails, the stale map is deleted. This removes the tern.local trip between steps 1 and 1a
  and closes the stale-map hole described under Bugs.
- **After ACCEPT, an Installed screen says what comes next.** The result screen only asks
  for the decision; what comes next isn't known until the map download finishes.
- **Cal menu:** Baseline (1), Fill gaps (1a), Mounted (2), Heading (3), then Speed cal,
  which stays unnumbered.

## Bugs this fixes

- **Mounted collection doesn't claim the buttons.** `handleModalButtons()` claims them for
  ROUGH_SCAN, BASELINE_GRID and GAP_FILL only. During mounted cal, BTN1 opens the menu but
  the cal screen keeps drawing priority, so the menu is invisible. BTN2 then selects
  whatever item is highlighted (root item 0 is Select WP, whose own modal then takes the
  buttons). This breaks menu invariant 4 in CLAUDE.md. Found by reading the code, not
  reproduced on hardware.
- **No way out.** A cal ends only by finishing it or by a power cycle. Heading has no exit
  at all short of 12 marks.
- **Tiny accept/reject on baseline and mounted.** `showCloudCalResult()` sets text size 1
  for the recommendation, and that size carries into the ACCEPT/REJECT list.
- **A gap-fill refusal looks like an upload failure.** "No baseline" and "no target map"
  arrive as `CalCloudStage::FAILED` (`sendGapFillRefusal()`), and the display shows
  "CLOUD CAL / Upload failed … retry from tern.local". Nothing was uploaded, and retrying
  can't help.
- **`showCloudCalOffline()` promises nothing is lost and points nowhere** (backlog item).
  This plan does the half that adds the tern.local pointer; the automatic retry stays open.
- **The cloud screens call `fillScreen()` on every 4 Hz render.**
- **A new baseline leaves gap-fill on the old baseline's map.** The gap-fill target map
  (`/cal_targets.json`) is refreshed only by tern.local's Check for updates
  (`cal_sync.cpp`, `refreshTargets()`), and nothing ties it to the installed baseline. After
  a new baseline is accepted on the unit, Fill gaps either refuses for lack of a map or,
  worse, runs against the *previous* baseline's targets.
- **ACCEPT tells the server "accepted" even when the install failed.** If copying the
  pending file over the active one fails, nav still calls
  `respondToCalibration(true)`, so the server makes it current while the unit keeps the
  old cal.
- **Accepting can flash NO LINK.** On ACCEPT the display goes straight back to the nav
  screen while nav blocks in `cloud::respondToCalibration()`, which can take up to
  `CLOUD_HTTP_TIMEOUT_MS` (15 s). The display's NO LINK timeout is 5 s.

## Screens

Positions are from the mock, as (x, y). Sizes are GFX text sizes: size n is 6n×8n per
character.

### 1 Baseline

**Bars (ROUGH_SCAN)**
- Counter: `sample_count/2160`.
- X, Y and Z bars at y 34, 62 and 90, each 292×22 starting at x 24, keeping today's colors.
- "Finding north..." in white at size 2, y 126. Below it, "The grid appears" and "once it
  locks." in gray at y 150 and 166.
- Status: "Tumble the unit randomly", yellow.
- Keys: QUIT | FINISH. FINISH is dim below `MAG_CAL_ROUGH_SCAN_MIN_SAMPLES`.

**Grid (BASELINE_GRID)**
- Counter: the same samples count.
- 5×12 grid with GX 26, GY 36, CW 24, RH 24. Each cell has a 1 px black gutter, the current
  cell gets a 2 px white border, and the position dot stays as today.
- Ticks (size 1): N, E, S, W at y 27; +60…−60 at x 2.
- Readout row: "38/60 cells" in white at (4, 162), "284° +41°" in yellow at (150, 162), and
  the roll widget, 26 px, at (287, 157).
- Status: "Fill red & yellow cells", switching to "Auto-finish at 2160" (yellow) once
  `sample_count` reaches 90% of the cap.
- Keys: QUIT | FINISH.

**On completion**, for `CAL_COMPLETE_HOLD_MS`, the last collecting screen stays up with
the status "Done - saving" in green ("Buffer full - saving" when `max_samples_reached`) and
both keys blank.

### 1a Fill gaps

- The same grid. Untargeted cells stay dark (`0x1082`).
- Counter: `bins_green/targets`.
- Readout: "N cells left".
- Status: "Fill red & yellow cells".
- Keys: QUIT | FINISH. FINISH is dim below `MAG_CAL_GAPFILL_MIN_SAMPLES`.

### 2 Mounted

- Counter: `bins_green/36`.
- 3×12 grid with RH 20. Every empty cell is red. The current cell is outlined white inside
  the window and yellow outside it.
- Horizon, 80 px, at (4, 100).
- MOTION label at (92, 102). The motion bar is 146×18 at (170, 101), with the cause
  ("still", "turning", "pushed", "shaking") drawn inside it.
- HOLD bar at (170, 125).
- Readout "130° P+01 R-02" at (92, 152).
- Status: `mountedCalInstruction()`, wording unchanged.
- Keys: QUIT | FINISH. FINISH is dim until one cell is filled.

### 3 Heading

- Counter: marks done out of 12.
- "Align to external compass" in gray at (4, 28).
- "TARGET" in gray at (4, 55). The target is size 4, white, at (220, 48), right-aligned.
- "INDICATED HDG" in gray at (4, 86). `heading_raw_deg` is size 3, gray, at (244, 82).
- Horizon, 80 px, at (4, 104), keeping today's 6× gain.
- HOLD bar at (170, 106), "P+0.8 R-0.4" in gray at (92, 133), "Avg err +2°" in yellow at
  (92, 159).
- Status, from the level gate:

  | Gate state | Status | MARK key |
  |---|---|---|
  | NOT_LEVEL | "Not level", red | dim |
  | MOVING, SETTLING | "Hold still", yellow | dim |
  | READY | "Ready - press MARK", green | go |
  | INACTIVE (an old nav build) | "Press MARK when aligned", white | go |
  | `HDG_CAL_LEVEL_GATE = false` (test build) | "GATE OFF - test build", red | go |

**MARKED** stays up for the same `HDG_CAL_ACK_HOLD_MS`, now inside the frame. The counter
has already advanced. The work area shows "MARKED" at size 5 in green, then
"TGT 120°  IND 123°", then "err +3°  avg +2°" in yellow. Status: "Next: 150°". Both keys
are dim.

### Endings (all steps)

The header keeps the session's step (see "Display-side state" below); the counter names
the phase.

| Screen | Work area | Status | Keys |
|---|---|---|---|
| Uploading | "Uploading" size 3; seconds size 3 yellow; "Takes up to 30 s. / The unit is busy." gray | "Please wait" | blank \| blank |
| Result | Value size 6 in band color at (8, 36); band word size 4, right-aligned at y 44; "RMS error" or "likely worst error"; Baseline adds "N orientation gaps" when `coverage_gaps > 0` | "Press ACCEPT to install"; POOR: "Next: run *step* again", red | REJECT (hold) \| ACCEPT |
| Saving (after ACCEPT) | The result, unchanged | "Saving...", green | blank \| blank, until nav reports back |
| Installed | "Installed" green; Baseline adds the map outcome: "Gap-fill map: N cells / ready on the unit.", or "Gap-fill map not / updated: *reason*" in yellow | Next line, see below | blank \| OK |
| Fill gaps sent | "Uploaded" green; "Patch RMS 8%"; "N cells still thin" | "Next: merge on website" | blank \| OK |
| No WiFi | "No WiFi" yellow; "Samples are saved / on the unit." | "Next: upload at tern.local" | blank \| OK |
| Upload failed | "Upload failed" red; the error, wrapped at size 2 | "Next: retry at tern.local" | blank \| OK |
| Fit rejected (`rejected`) | "Fit rejected" red; the server's reason, wrapped | "Next: collect again" | blank \| OK |
| Can't start (gap-fill refusal) | "Can't start" red; the nav board's reason, wrapped | "Nothing was collected" | blank \| OK |
| Quit (after the hold) | "Quit" size 4; "Nothing was uploaded." | "Back to the nav screen" | blank \| blank, for 1.5 s |
| Rejected (after the hold) | "Rejected" size 4; "Your old cal stays on." | "Back to the nav screen" | blank \| blank, for at least 1.5 s and until nav reports back |

The Uploading screen's static content is drawn once; after that only the seconds redraw,
when they change.

The Installed screen's Next line:

| Step accepted | Next line |
|---|---|
| 1, map downloaded, N > 0 cells | "Next: 1a FILL GAPS" |
| 1, map downloaded, 0 cells | "Next: 2 MOUNTED" |
| 1, map not downloaded | "Next: sync at tern.local" |
| 2 | "Next: 3 HEADING" |
| 3 | "All 3 steps done", green |

The 1a decision uses the downloaded target count (thin plus empty cells), not the
result's `coverage_gaps`, which counts empty cells only.

Values: Baseline and Mounted print `rms_pct` as `%.0f%%`. Heading prints the leave-one-out
`max_err_deg` (which arrives in the same field) as `%.1f°`, or `--°` when it's negative.

## Display-side state (`display_main.cpp`)

- **`gCalStep`** (`CalStep` enum, in `display.h`: BASELINE, FILL_GAPS, MOUNTED, HEADING).
  - It comes from the first CalProgressPacket of a session: ROUGH_SCAN or BASELINE_GRID
    means BASELINE, GAP_FILL means FILL_GAPS, `cal_type` MOUNTED means MOUNTED. Starting
    the heading cal sets HEADING.
  - It's held through the upload and the ending screens.
  - A CalCloudResultPacket with no session behind it (a retry-upload from tern.local)
    sets the step from `cal_type`, with `installable == false` meaning FILL_GAPS.
- **`gGapFillStarting`**: set when `display_main` sends START_GAPFILL_CAL (the menu sends
  it from `menu.cpp` through `display_main`'s send callback, so that is the only place
  that sees it). Cleared by the first GAP_FILL CalProgressPacket, by any
  CalCloudResultPacket, or after 5 s. A FAILED result while it's set is a refusal, so the
  display shows "Can't start", not "Upload failed".
- **`gCalFinishPending`**, renamed from `gBaselineFinishPending` because Mounted uses it now
  too. While it's set, the collecting screen shows the status "Finishing..." in green with
  both keys dim. This replaces `showBaselineFinishing()`, so a refused finish falls back to
  the same screen with no flash.
- **`CloudCalUiPhase::SAVING`**: entered on ACCEPT or REJECT, ended by the nav board's
  RESPONDED packet (below) or `CLOUD_CAL_WAIT_TIMEOUT_MS`.
  - It joins the `linkAlive` overrides, the same way WAITING does.
  - ACCEPT then shows Installed. REJECT shows Rejected, for at least 1.5 s.
  - An ACCEPT that comes back `installed = false` shows "Not installed" with the reason.
  - On timeout it drops back to the nav screen, as ACCEPT does today.
- **`gCalQuitPending`**: after QUIT_CAL, ignore CAL_PROGRESS and CAL_ORIENT packets until a
  NavPacket shows `system_state != CALIBRATION`, or for 3 s at most. Without this, a
  progress packet already on the wire would bring the cal screen back for a moment.
- **Hold and hint:** `display::setCalKeysOverlay(holdPct, hint)` is called before each
  render. While the hold is in progress, the render interval drops to
  `HDG_CAL_RENDER_INTERVAL_MS`, so the fill moves in 20 steps rather than 8.

## Changes by file

### Shared

- `src/config.h`
  - Move `BIN_CAL_MAX_SAMPLES` (`imu.cpp:610`, 2160) here as `MAG_CAL_BIN_MAX_SAMPLES`,
    since the display needs it for the counter.
  - Change `MAG_CAL_SECTORS_MID` from 6 to 12, with a comment pointing at the backlog
    analysis. Fix the comment near line 209 that describes the 6-cell rule.
  - Move `LONG_PRESS_MS` (now `static` in `display_main.cpp`) here so `cal_keys.h` can use
    it.
- `lib/dpvlink/dpvlink.h`
  - Add `DisplayCmd::QUIT_CAL = 47`.
  - Add `PacketType::CAL_RESPONDED` and `CalRespondedPacket`. Nav sends it once an ACCEPT
    or REJECT is fully handled, on every path, including an ACCEPT or REJECT it ignored
    because the id didn't match. (Decided 2026-11-09: its own packet type rather than a
    new stage on CalCloudResultPacket, so no field is repurposed, and an old display
    ignores it instead of reading it as a fit result.)
    - `cal_type`: the cal that was accepted or rejected.
    - `accepted`: what the display asked for.
    - `installed`: true only when an ACCEPT's file copy worked.
    - `target_count`: the downloaded target count for a baseline accept, −1 if the map
      wasn't downloaded or doesn't apply.
    - `error`: why it wasn't installed, or why the map wasn't downloaded.
  - Round-trip it in `tools/dpvlink_test/`.
- `src/util/cal_keys.h` (new, pure C++): the hold gesture.
  - Inputs: BTN1/BTN2 pressed, BTN1 press start, BTN1 fired, and now.
  - Outputs: hold percent, discard-fires-now, and short-press.
  - Progress counts only while BTN1 alone is down. If BTN2 joins, progress resets, so the
    two-button reinit wins.
  - It fires once at `LONG_PRESS_MS`. A release before that is a short press.
  - Host test in `tools/cal_keys_test/`, compiled against the real header like the other
    host tests.

### Nav

- `src/nav_main.cpp`
  - Factor the tail of the bin-cal completion block (`magBinCalEnd(); loadCalibration();
    gInCal = false; sysState = READY;`) into `endBinCal()`, used by completion and by QUIT.
  - QUIT_CAL handler:
    - Bin cal (mode 5, 6 or 7): `endBinCal()` with no CSV dump and no upload. Any
      previous samples CSV on flash is left as it was.
    - Heading: `gHdgCalActive = false`, `gHdgSampleCount = 0`, `gLevelGate.reset()`.
      Samples live only in RAM until FINALIZE, so nothing is on flash to clean up.
    - Anything else: ignore.
  - FINISH_BASELINE_COLLECTION (~line 2399): widen the `gCalMode == 5 || 7` gate to
    include 6. Keep the enum name, and add a comment that it now finishes any bin cal.
  - ACCEPT_CLOUD_CAL (~line 2626): after the copy and `loadCalibration()`, keep the
    result of `cloud::respondToCalibration()`.
    - If the copy failed, don't tell the server it was accepted and don't touch the map.
      Today ACCEPT responds `true` even when the copy fails, which leaves the server
      and the unit disagreeing. Send RESPONDED with `installed = false` and the reason.
    - Baseline only (the active path is `storage::MAG_BASE_FILE`):
      - If the respond succeeded, download the gap-fill map.
      - If it failed, *don't* download: the server still has the previous baseline as
        current and would hand back its map. Delete `/cal_targets.json` instead.
      - If the download fails, delete it too.
    - Then send RESPONDED.
  - REJECT_CLOUD_CAL: send RESPONDED after the respond call.
  - The "no matching pending result" branch of both sends RESPONDED too, with the
    error, so the display never waits out the Saving timeout.
- `src/net/cal_sync.h` / `cal_sync.cpp`: make the map download public, e.g.
  `bool refreshGapFillTargets(int& targetCount, String& err)`, and add `clearTargets()`.
  - `refreshTargets()` becomes a thin wrapper that turns the result into tern.local's
    message text, so both paths share one download.
  - The server needs no change. Accepting sets `is_current` on the new row inside the
    respond request (`db/queries.rs`), and `GET /calibrations/targets` reads the
    `is_current` baseline's stored `fit_result.coverage` (`handlers/device.rs`).
- `src/sensors/imu.cpp`
  - `magBinCalFinishBaseline()`: today it returns false for mounted. Give mounted a branch
    that accepts once at least one cell is filled and sets the user-done flag.
  - `magBinCalIsComplete()`: the mounted branch becomes
    `g_baselineUserDone || g_mountedDwell.complete()`. Rename `g_baselineUserDone` to
    `g_userDone`; it's no longer baseline-only.
  - Use `MAG_CAL_BIN_MAX_SAMPLES`.
- `src/util/mounted_dwell.h`: no change beyond the constant. `kNeedTotal` becomes 36.
- `tools/mounted_dwell_test/dwell.cpp`: the checks at ~235–269 hard-code 24 cells and
  the six-spread rule. Rewrite them in terms of `MAG_CAL_SECTORS_MID`, and test
  `rowMaxGapSectors()` directly, so dropping the constant back after the analysis doesn't
  mean rewriting the tests.

### Display

- `src/drivers/display.h` / `display.cpp`
  - Frame helpers, static in `display.cpp`: `drawCalHeader(step, counter)`,
    `drawCalStatus(parts)` (which also handles the hint override) and
    `drawCalKeys(left, right, holdPct)`. Each keeps its own last-drawn cache, all reset by
    `clear()`, so nothing redraws unless it changed. The fill redraws only the pixels that
    changed.
  - `setCalKeysOverlay(uint8_t holdPct, bool hint)`.
  - `drawHorizon()` takes its size as a parameter. `MC_AI_S` goes from 96 to 80; update
    the two `static_assert`s and the `GFXcanvas16` allocation.
  - Move into the frame: `showBaselineRoughScan`, `showCalGrid`, `showMountedCal`,
    `showHdgFourierCalPrompt`, `showHdgFourierCalAck`, `showCloudCalWaiting`,
    `showCloudCalOffline`, `showCloudCalFailed` (which gains a kind: TRANSPORT, REJECTED,
    REFUSED), `showCloudCalResult` (absorbs `showHdgCalResult`) and
    `showGapFillUploaded`.
  - Add `showCalQuit(step, rejected)`.
  - Delete `showBaselineFinishing`, `showHdgCalResult` and `drawHdgResultButton`.
  - Signatures gain a `CalStep`. `showCloudCalResult` loses `recommendation` and `choice`.
- `src/display_main.cpp`
  - `handleModalButtons()`:
    - One block for every collecting screen: bin cal in any phase, *including mounted*,
      and heading while COLLECTING. BTN2 sends FINISH or MARK, a `cal_keys` hold sends
      QUIT_CAL, and a short BTN1 press shows the hint.
    - Result: BTN2 sends ACCEPT and a hold sends REJECT. Remove `gCloudCalChoice` and the
      BTN1 cycle.
    - OFFLINE, FAILED and gap-fill sent: BTN2 means OK.
  - Add the state above. `renderCalProgress()`'s dispatch is unchanged.

### Menu

- `src/menu/menu.cpp`: labels "Baseline (1)", "Fill gaps (1a)", "Mounted (2)",
  "Heading (3)". "Speed cal" is unchanged.
- `src/menu/menu.h`: raise `MENU_LABEL_LEN` from 12 to 14, because "Fill gaps (1a)" is 14
  characters. The size-3 menu row holds 16, the same room "WiFi CONNECTING" already uses.

### Dive Map

- `calibration-processor/callib/fit.py`, the comment near line 65: it says the device
  requires "11 of 12 sectors" in the tilted rows. That was already wrong (the device needs
  6); make it say 12 and name the dpv-nav constant. Comment only.

### Docs, at ship time

- **CLAUDE.md:**
  - Calibration System: mounted completion at 36, FINISH and QUIT.
  - The heading result description, which still mentions the two half-screen buttons.
  - The "Key display functions" list.
  - Menu Safety Invariant 4, which now holds on every cal screen.
  - "Next free values": `DisplayCmd` 48.
- **`user-guide.md`:** in the Baseline, Gap Fill, Mounted and Hdg sections, cover the
  frame, holding to quit or reject, FINISH on Mounted, the counters, INDICATED HDG, and the
  gap-fill map arriving with the accept (drop the "run Check for updates first" step from
  the gap-fill instructions). The illustrated walkthrough is a separate backlog item.
- **Menu paths:** about 50 places in `user-guide.md`, `calibration-guide.md` and CLAUDE.md
  say "CAL > Baseline", "CAL > Hdg cal" and so on. Switch them to the new labels, and update
  CLAUDE.md's Menu Structure tree.
- **`calibration-guide.md`:** anywhere it describes these screens or buttons.
- **`backlog.md`:** mark the OFFLINE item half done, with auto-retry still open. The
  walkthrough item is unblocked.
- **`tools/cal_screen_mock/screens.js`:** match anything the build changed.

## Implementation order

1. **Constants and pure logic.** Add `MAG_CAL_BIN_MAX_SAMPLES` and `LONG_PRESS_MS` to
   `config.h`, set `MAG_CAL_SECTORS_MID = 12` and rework the dwell test, and add
   `cal_keys.h` with its host test. Run every host test.
2. **Nav:** `endBinCal()`, QUIT_CAL and mounted FINISH. Nothing on screen sends QUIT yet,
   and mounted FINISH waits for step 4.
3. **Display frame and endings.** Add the frame helpers, move the shared ending screens
   into the frame, and switch the result screen to direct ACCEPT and hold-to-REJECT. These
   screens are shared by all four steps, so this is the biggest consistency win, and it can
   go out alone as a test build.
4. **Collecting screens, one at a time,** each with its QUIT/FINISH/MARK wiring:
   1. Mounted first, which also fixes the button leak.
   2. Baseline bars and grid.
   3. Fill gaps.
   4. Heading, with MARKED.
5. **Accept and map sync:** RESPONDED, the SAVING phase, the Installed screen, and the
   nav-side map download and deletion. Rejected waits on RESPONDED too.
6. **Ship:** menu labels and `MENU_LABEL_LEN`, docs, mock sync, a version bump to
   `0.7.26-dev.N`, the hardware test, and a test-build publish.

Work happens on `feature/cal-screens-unified`, merged to main at step 6. Builds reach the
unit as OTA dev builds (`0.7.26-dev.N`); each publish is confirmed first. Bench checks
come after step 3 and after each collecting screen, not all at the end.

## Hardware test

Flash both boards from the same tree.

- [ ] Each step, start to finish: collecting → FINISH (or MARK ×12) → upload → result →
      ACCEPT. The header shows the right step and counter the whole way through.
- [ ] Hold-to-QUIT from every collecting screen:
  - Nav returns to READY, nothing uploads, and the samples CSV on flash is unchanged.
  - The installed cal is back. Quitting a baseline must not leave heading running on the
    identity cal.
  - The cal screen doesn't reappear.
- [ ] Release BTN1 at about 1.5 s: nothing fires and the fill resets. A short press shows
      the hint.
- [ ] BTN1 and BTN2 held together during a cal still reinitializes the display and never
      fires QUIT.
- [ ] Hold-to-REJECT on a result: the old cal stays installed. ACCEPT is a single press.
- [ ] Mounted FINISH:
  - With 2–3 cells, the server refuses and the "Fit rejected" reason is readable.
  - With a full level row, it fits.
  - A full 36 auto-finishes.
- [ ] BTN1 and BTN2 during mounted collection no longer reach the menu.
- [ ] Baseline counter: it carries straight over at lock, the warning appears from about
      1944, and the session auto-finishes at 2160.
- [ ] Fill gaps with no target map shows "Can't start", not "Upload failed".
- [ ] Accept a baseline on the unit: Saving, then Installed with the cell count. Fill gaps
      then starts straight away, and its targets match the website's coverage view for the
      new baseline. There's no NO LINK flash while it saves.
- [ ] Accept a baseline with the server unreachable after the upload (for example, drop
      the AP during Saving): Installed says the map wasn't updated, `/cal_targets.json` is
      gone, and Fill gaps refuses with the sync instruction.
- [ ] Accept mounted and heading: Installed shows the right Next line. Reject: the
      Rejected screen holds until nav reports back.
- [ ] No WiFi at completion: the offline screen points at tern.local.
- [ ] No visible flicker on any screen, especially the hold fill and the 10 Hz heading
      screen.
- [ ] Heading: INDICATED HDG and the horizon don't collide (that row ends at y 103 and the
      horizon starts at y 104).

## Risks

- **Flicker.** Every screen today is careful to redraw only what changed. The frame
  helpers have to keep that, which is why each has its own cache.
- **Mixed versions.** Not a real concern: OTA updates both boards in one pass, so only a
  single-board USB bench flash can split them. With a new display and an old nav,
  QUIT_CAL and mounted FINISH are ignored, and the display's quit and finish timeouts drop
  it back on the cal screen. An old display ignores CAL_RESPONDED.
- **Mounted takes longer at 36 cells.** FINISH is the way out. The backlog analysis may
  bring the number back down.
- **REJECT now takes a 2 s hold** instead of two presses. This was decided; it's slower on
  purpose.
- **Long server reasons are cut off.** The wire's error text is 64 characters, so long
  messages are truncated. At size 2, wrapping gives three lines of 26.
- **Accepting takes longer for a baseline,** because it adds a second blocking HTTPS call
  to nav's accept. The SAVING phase covers it; the worst case is two
  `CLOUD_HTTP_TIMEOUT_MS`, inside the 40 s wait.
- **A failed respond leaves the server and the unit disagreeing.** This was already true.
  If `respondToCalibration()` fails, the unit has the new baseline installed but the
  server still has the old one as current. A later Check for updates on tern.local would
  then offer the old one back. This plan only stops gap-fill from acting on that mismatch;
  fixing the disagreement itself is out of scope.

## Follow-up (not in this plan)

The trip between 1a and 2 still goes through two other places. A gap-fill patch is merged
on the Dive Map website, then the merged baseline is pulled to the unit with tern.local's
Check for updates. Cutting that to the device alone (merge server-side on upload, then
offer the merged fit on the unit's own result screen) is the next step toward doing all
of calibration from the unit.

## Status

Planned 2026-11-09. Both open decisions were settled 2026-11-09: the map downloads with
the accept, and the menu reads "Baseline (1)" etc. Reviewed against main (b97ea43) the
same day; RESPONDED became its own packet type. Implementation started 2026-11-09 on
`feature/cal-screens-unified`.
