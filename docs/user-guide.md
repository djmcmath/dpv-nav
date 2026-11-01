# DPV-Nav User Guide

This guide is in three parts:

1. **[Calibration](#calibration)**: done once for a new installation, and again whenever the physical setup changes, or if, for any reason, a calibration check fails.
2. **[Diving with DPV-Nav](#diving-with-dpv-nav)**: the routine from the night before, through the dive, to the surface.
3. **[Reference](#reference)**: menus, screens, settings and troubleshooting.

---

# Calibration

## Recommended Calibration Order (New Installation)

Do these in order. Each step relies on the one before it.

1. **Baseline mag cal** (**CAL > Baseline**): device off the DPV, full sphere coverage.
1a. **Review Baseline, Push Gap-Fill Map**: On map.terndiving.com, review your calbration coverage, then on tern.local select "Check for updates" to pull the gap-fill map.
2. **[Gap fill](#gap-fill-cal--fill-gaps)** (**CAL > Fill gaps**): your baseline calibration will be incomplete, and there's no good way to show that while you're collecting the data. Once a reasonably competent baseline exists, though, it's possible to highlight the gaps and fill them intelligently.
3. **Mounted mag cal** (**CAL > Mounted**): device installed on the DPV, horizontal rotations, provides an indication of how to adjust the optimal sphere based on the magnetic field of the scooter in orientations relatively similar to what you'll face on a dive.
4. **Fourier heading cal** (**CAL > Hdg cal**): 12 points on the DPV at 30° intervals, dead level, read against a calibrated compass.
5. **Speed cal** (**CAL > Speed cal**): 3–6 runs over a known distance to dial in the flow sensor k-factor.

Gyro and accel calibrations are started automatically on first boot if their files are absent.  Note that this calibration process requires the serial port to be connected.  If your unit powers on but never connects with the display, connect the USB and expect to walk through this process.

**Every step uses the same cloud round trip.** When collection finishes, the unit uploads the samples (if it has WiFi and a linked account), the server fits them, and the display shows a **CLOUD CAL** wait screen followed by an accept/reject result. Accept installs it immediately, with no reboot. Reject keeps whatever was active before. With no WiFi, the samples are still saved to the unit, and an offline Python fit is the fallback (see each step below and [calibration-guide.md](calibration-guide.md#magnetometer-calibration)).

## Baseline Calibration (CAL > Baseline)

Device **off** the DPV, rotated through every orientation. See [calibration-guide.md](calibration-guide.md#running-baseline-calibration) for the full procedure.

Baseline shows axis-range bars and live fit stats, not a grid. (A live grid during a *first* baseline pass turned out not to be trustworthy; see [calibration-guide.md](calibration-guide.md#running-baseline-calibration).) Rotate through all orientations until the bars fill and the stats look stable, then press **BTN2** yourself to declare it done. It doesn't finish on its own.

General note: I've found that a best process here looks like:
- Hold the unit in one hand, by the end, pointed straight down.  Raise it slowly through level to straight up.  Rotate ~15-20deg.  Lower the unit slowly.  Complete a full circle.
- Rotate the unit 90 deg, so now it's pointed straight down, but when you raise it to horizontal, it'll be lying on one side.  Repeat the circle this way.
- Rotate 90deg again, one more complete circle of up-and-downs
- One last rotation, one final complete circle of up and downs.
- A good target is something like 1400 samples.  Less than 1000 will yield a lot of gaps.  More than 1800 is probably redundant for a baseline -- you're gathering data with no idea whether its good.  Get the gap fill before collecting too many samples.

Offline fallback: run the Python fit and upload `mag_base.json`.

Then, in a browser:
- map.terndiving.com: Review the collected data.  You should have a mostly tolerable coverage of the sphere at this point, but there'll be some weaknesses.
[TODO: Screenshot]
- tern.local: Press the button in the Calibration section for "Check for Updates."  That pulls the gap-fill map so the unit can tell you where to point it to collect the full sphere.

## Gap Fill (CAL > Fill gaps)

A second, short baseline pass, still **off** the DPV, that collects only the orientations the first pass missed. After the baseline is fitted, the Dive Map server grades its coverage and flags the cells that came out thin or empty. Gap fill shows you those cells and walks you into them. See [calibration-guide.md](calibration-guide.md#baseline-cal--fill-gaps) for more on how targets are chosen and merged.

**Before you start:**
- A baseline calibration must already be installed on the unit.
- The unit needs the server's target map. On `tern.local`, under **Calibration Cloud Sync**, press **Check for updates**. Every press refreshes the map, so press it again after any change on the website. If the map can't be synced, the reason shows in the result text on that page.

Without either of these, **Fill gaps** refuses with an on-screen message rather than running.

### The gap-fill screen

```
BASELINE CAL

     N           E           S           W
 60° [  ][  ][  ][▓▓][  ][  ][  ][  ][  ][  ][  ][  ]
 30° [  ][██][  ][  ][  ][  ][  ][  ][  ][  ][  ][  ]
  0° [  ][  ][  ][  ][  ][  ][  ][  ][  ][  ][  ][  ]
-30° [  ][  ][  ][  ][  ][  ][  ][  ][▓▓][  ][  ][  ]
-60° [  ][  ][  ][  ][  ][  ][  ][  ][▓▓][▓▓][  ][  ]
                                                   ┌──┐
4/7 cells                                          │\/│
284° -37°                                          │/\│
                                                   └──┘
```

TODO: Add a screen shot of the gap-fill screen here.

**The grid** is every orientation the sensor can point: 12 heading columns (30° each) across, five elevation rows down (straight up at the top, level in the middle, straight down at the bottom). Only the cells the server flagged are your job:

| Cell color | Meaning |
|---|---|
| Dark gray | Not a target. The server is happy with this cell, so leave it alone. |
| Red | Target, no samples yet. |
| Yellow | Target, partly filled. The number in the cell is its sample count. |
| Green | Target finished, including its roll coverage (see below). |

- The **white border** marks the cell the unit is pointing into right now, and the **white dot** shows exactly where inside that cell. Steer the dot into a red or yellow cell and hold it there.
- **`4/7 cells`** counts finished targets out of the total targets for this session, not out of the whole grid.
- **`284° -37°`** is the heading and elevation you're pointing at, from the same calculation that places the highlight, so the two always agree.

**The orientation box** (lower right) answers a question the grid can't: how the unit is *rolled* while it points into that cell. A cell needs samples from more than one roll, or the fit sees only one side of the problem. The box is a square split into four triangles:

| Triangle | Roll |
|---|---|
| Top | Upright |
| Right | On its right side |
| Bottom | Upside-down |
| Left | On its left side |

Each triangle uses the same colors as the grid, for whichever cell you're pointing into right now:
- **Green**: that roll is covered for this cell (some may already be covered by your earlier baseline, so they start green).
- **Yellow**: some samples, not enough yet.
- **Red**: none yet.
- **Dark gray**: no live orientation to report.

The **white outline** marks the triangle matching your current roll. A cell only turns green on the grid once all four triangles are green. If the grid says you're in the right cell but the box still shows red or yellow, keep pointing the same way and roll the unit onto the side the box is asking for. Upright needs about 3× the samples of the other three.

**Tips:**
- **Pause, don't sweep.** While the unit is moving, the accelerometer sees your hand's motion as part of "down", so the grid and box can lag or jump. Get near the target, then hold still.
- **Top and bottom rows: aim for about 65°, not straight up or down.** Near vertical, a small wobble sweeps you across several heading columns and the roll reading is mostly noise. At 65° you're still in the outer row, with heading and roll roughly 12× steadier. See [calibration-guide.md](calibration-guide.md#baseline-cal--fill-gaps) for the numbers.
- **BTN2 finishes early.** Collection finishes on its own when every target is green, but some cells may be physically unreachable. Press BTN2 when you've got what you can.

### After collecting

1. If the unit has WiFi and the device is linked, the patch uploads automatically. The result screen is information only, with no accept/reject: a patch is never installed by itself.
2. On Dive Map, open the device's **Calibration History**. Your latest baseline row shows a **"New collection available"** badge. Expand it to see what the patch covers, then combine it. That produces a new pending baseline calibration, which you review and **Accept**.
3. On `tern.local`, press **Check for updates** to pull the merged baseline onto the unit. Until then the unit is still using the old one.
4. **Mounted and heading cal are fit on top of the baseline.** In the recommended order you haven't done them yet. If you gap-fill an installation that already has them, redo **Mounted** and then **Hdg cal** afterward.

## Mounted Calibration (CAL > Mounted)

Device installed on the DPV in its normal position, with motors, batteries and everything else magnetic in place. This calibrates out the scooter's own magnetic signature. See [calibration-guide.md](calibration-guide.md#running-mounted-calibration) for the full procedure.

Each cell of the grid is filled by **one steady hold**: the unit must sit still at the right attitude for about half a second. Swinging through a cell records nothing. That's deliberate: the fit takes "down" from the accelerometer, so a sample taken while the scooter is moving carries a false tilt and is useless.

### The mounted cal screen

```
MOUNTED CAL                                    9/24
     N           E           S           W
+30  [  ][██][  ][  ][  ][  ][  ][  ][  ][  ][  ][  ]
  0  [██][██][██][██][██][██][██][  ][  ][  ][  ][  ]
-30  [  ][  ][  ][  ][  ][  ][  ][  ][  ][  ][  ][  ]

┌────────┐  MOTION still
│  sky   │  [                              ]
│[--●--] │  HOLD
│ ground │  [██████████████                ]
└────────┘  210° P+02 R-01
            Holding...
```

TODO: Add a screen shot of the mounted cal screen here.

**The grid** is three rows of 12 heading cells: nose up 30°, level, and nose down 30°. Green cells are recorded; dark gray cells are still open. **`9/24`** (top right) counts recorded cells against the 24 needed: the whole level row, plus 6 in each tilted row.  **Note** I normally collect all 24 cells to green -- it's just as easy to collect all of them as it is to skip a few.

- The **highlight** marks the cell you're in. It's **white** when your attitude is inside that row's window (a cell can fill), and **yellow** when that row is only the nearest one (it can't fill from there yet).
- The **white dot** shows where you are inside the cell.

**The artificial horizon** (lower left) works like an aircraft's: it tilts with roll and slides up and down with pitch. It shows your error from the *row you're working on*, exaggerated 2× so it's readable from the far end of the scooter, not your raw attitude. The black mark in the middle is the unit's nose. The **brackets** mark how much error is allowed. Put the white horizon line between the brackets and level it, and the brackets turn from yellow to **green**.
- Level row: within 10° of level in both pitch and roll.
- Tilted rows: nose up or down 20–40°, within 15° of roll.

**The MOTION bar** shows how much the unit is moving. It needs to be **empty**. The word beside it says what's moving: `still`, `turning` (gyro), or `pushed` / `shaking` (accelerometer). If it won't empty with the scooter resting on its stand, that sensor's calibration is off, not your hands.

**The HOLD bar** fills (cyan) while the unit settles and then records. When it's full it turns green, the cell turns green, and the line reads `Got it - turn`.

The readout below the bars shows heading, pitch and roll. Under it, one instruction line tells you what to do next:

| Message | What to do |
|---|---|
| `Level it` / `Nose up to 30` / `Nose down to 30` | Pitch is outside the row's window. |
| `Level the roll` | Pitch is fine; roll isn't. |
| `Turn a bit more` | You're within 3° of the line between two cells. A hold won't start there. |
| `Hold still` | Attitude is right, but the unit is moving. Watch the MOTION bar. |
| `Holding...` | Recording. Don't move until the HOLD bar is full. |
| `Got it - turn` | Cell done. Move on to the next one. |
| `Row done - tilt` / `Row done` | Row complete. Move to a tilted row. |
| `DONE - saving` | All 24 cells are in. |

### Procedure

The rhythm for every cell is: **move the dot to the center of the next box, wait until it's stable, then wait until the HOLD bar fills.**

1. Open menu (BTN1) → **CAL > Mounted**.
2. **Level row first, on the stand: all 12 headings.** Turn about 30°, aiming the dot at the center of the next cell (not its edge). Let go, and wait for the MOTION bar to empty. Then wait for the HOLD bar to fill and the cell to turn green. Repeat.
3. **Then each tilted row: 6 headings, spread around the circle.** Every other cell is ideal; don't leave a gap wider than 3 cells (90°). Pitch the nose up (or down) until the brackets go green, then use the same rhythm.  Note that it's only slightly slower to actually fill every cell, and it doesn't hurt to have more data.
4. At 24/24 the unit saves the samples and, with WiFi, uploads them and shows the accept/reject result. If the server can't fit the samples, the screen says **Fit rejected** with its reason. The upload itself worked, so collect again rather than retrying.

Offline fallback: run the offline fit and upload `mag_mount.json`.

## Fourier Heading Calibration (CAL > Hdg cal)

After magnetometer calibration, systematic heading errors (5–20°) typically remain because of the DPV's magnetic geometry. Heading cal collects 12 data points for a Fourier fit, done in the cloud or offline as a fallback, which brings the residual error down to about 2°.

**When to use:** after completing baseline + mounted mag cal. Run it somewhere magnetically clean, not a garage with a concrete floor.

**First, check your compass.** The heading calibration aligns the IMU to an absolute reference. Most technical divers have some kind of compass mounted on their scooter, but it may or may not be accurate. My compass, for example, was ~12° off, and that led to a long series of confusing calibration results. Before aligning the IMU to your compass, make sure the compass is right. The best way is to point your scooter precisely at a distant object on a known bearing (use the alignment sights to get it as exact as you can), then correct whatever error your compass has. That gives you a single-bearing reference. Assuming your compass is round, you should be able to get a good calibration from there.

**Part 1 — On-device collection:**

1. Select **CAL > Hdg cal** from the menu. The display shows:
   ```
   HDG CAL
   Step 1/12: Target 000°
   Align to 000°
   Press BTN2 when stable
   ─────────────────────
          4.7°
   ```
   The large number is the current live raw heading. The HOLD bar shows how stable the unit is, and the artificial horizon shows level. Get it close to level, and wait for the HOLD bar to settle, before pressing BTN2.

2. Align the DPV to **0° (North)** as shown on your calibrated compass, which is your objective truth. Let the compass settle, and wait for the heading readout to stop changing.

3. Press **BTN2**. The step advances to 030°, then 060°, and so on through all 12 steps at 30° intervals.

4. After Step 12, the nav device saves `/hdg_samples.csv`. **If the unit has WiFi** and the account is linked, it uploads the file automatically, gets a fitted result back (~15–20 s), and shows an accept/reject screen right there:
   ```
   CLOUD CAL
   ─────────────────
   Quality: GOOD
   Max err: 1.8°
   ─────────────────
   > ACCEPT
     REJECT
   ```
   Press **BTN1** to switch between ACCEPT and REJECT, **BTN2** to confirm. Accept installs the new heading correction immediately (no reboot needed). Reject discards it and keeps whatever was active before.

   **If the unit has no WiFi**, you'll see an offline notice instead. The CSV is still saved, so the manual fallback below still works.

5. Press **BTN2** to exit and return to normal navigation.

**Part 2 — Offline fit and upload (fallback only, for when the unit had no WiFi):**

1. Download `/hdg_samples.csv` from the nav device LittleFS.
2. Run: `python tools/fourier_fit.py hdg_samples.csv`
3. Upload the resulting `hdg_fourier.json` to nav device LittleFS root.
4. Reboot. The correction loads automatically on the next boot.

**If one section of the circle came out thin** (for example, only a few of the 12 points landed within some 90° span), the cloud fit detects it automatically. The Dive Map website will show a "thin sector(s)" flag on that calibration, with suggested headings to fill it. See **[Filling a Thin Heading Sector](#filling-a-thin-heading-sector)** below. This only applies to the cloud path; the offline fallback tool doesn't check for it. A thin sector usually points to an unusually bad mounted or baseline calibration. If the calibration up to this step is good, the heading calibration should only be smoothing 3–4° at the worst heading, so the flag shouldn't ever trigger.

### Filling a Thin Heading Sector

If Dive Map flags a thin sector on your heading calibration, you don't need to redo all 12 points. A small web form fills just the gap:

1. Sign in to Dive Map and open your device's **Calibration History**.
2. The affected heading (hdg) calibration row shows a "thin sector(s)" badge. Expand it to see the gap(s) and the suggested headings to add.
3. Set the displayed heading to RAW (**DISPLAY > Heading**). RAW is the magnetic heading before the heading cal and the motor offset (`motor_cal.json`) are applied.
4. For each suggested heading: aim the DPV at that bearing, using the same calibrated-compass method as the original 12-point collection. Read the **indicated** heading off the unit's display, and type both the target and indicated values into the form.
5. Submit. The website combines your typed points with the original upload and refits, producing a new calibration that isn't active yet, for you to review.
6. Review the result and press **Accept** on the website.
7. **This does not update the device yet.** In a separate browser tab, open the web interface at `tern.local`, find **Calibration Cloud Sync**, and press **Check for updates**. That's the step that actually pulls the accepted result onto the device. If you skip it, the website shows an accepted calibration that the unit still isn't using.

## Speed Calibration (CAL > Speed cal)

Speed cal measures how accurately the flow sensor reports speed: you travel a known distance and the unit times it. The computed k-factor is saved and averaged with up to 5 previous runs.

**Workflow:**

1. Select **CAL > Speed cal** from the menu. The menu closes and a distance-selection screen appears:
   ```
   SPEED CAL
   Set distance:
       300 ft
   BTN1: change
   BTN2: confirm
   ```
2. Press **BTN1** to cycle through distances: 150 → 200 → 250 → 300 → 350 → 400 → 450 → 500 → back to 150 ft. Press **BTN2** to confirm.

3. The display switches to a waiting screen. **Start the DPV.** The run begins automatically once the flow sensor detects motion above ~0.3 m/s.

4. Travel the selected distance in a straight line at typical cruising speed. The display shows a large elapsed-time counter:
   ```
   SPEED CAL
   RUNNING
        47
   300ft target
   Stop=slow/turn
   ```

5. The run ends automatically when either:
   - **Flow drops** below ~0.08 m/s (DPV stopped), or
   - **Heading changes more than 90°** after at least 30 seconds of stable heading (cross the finish and swing the scooter to signal you're done)

   Runs shorter than 5 seconds are discarded and the device returns to WAITING.

6. The result screen appears:
   ```
   SPEED CAL
   300ft  87s
   Cur: 1.000
   New: 1.156
   ─────────────
   > RESET+ACCEPT
     ACCEPT
     REJECT
   ```
   - **RESET+ACCEPT**: discard measurement history, start fresh with this single run
   - **ACCEPT**: add this run to the rolling average (up to 6 measurements kept)
   - **REJECT**: discard this result, return to navigation unchanged

7. Press **BTN1** to cycle choices, **BTN2** to confirm. The active k-factor updates immediately.

**Tips:**
- A calm, straight stretch of at least 150 ft works best: a pool lane, a wall, a pier.
- You don't actually need to be mounted on a scooter for this. You can duct tape the unit to a stick and drag it through shallow water while wading along a measured distance.
- The longer the distance, the more accurate the result.
- After 3–6 runs the rolling average stabilizes; use ACCEPT each time.
- Use RESET+ACCEPT when the DPV has been serviced or the impeller changed.

---

# Diving with DPV-Nav

Navigation is only as good as the last known position it starts from, so most of this routine is about two things: making sure the heading is trustworthy, and getting a good GPS fix into the log right before you descend and right after you surface. The post-dive track correction anchors on those fixes, and on any waypoints you reported during the dive.  The farther apart truth-points are (GPS fixes or marked landmark arrivals), the more accurate the corrected track will be.  If you mark GPS at the descent line, do a dive with no marked waypoints, and report GPS at the same point at the end, the track correction will have no idea where you actually went on the dive, aside from dragging the final estimated position to the second GPS fix (rubber sheet style correction).  

## The Night Before

- **4-point heading check.** Set **DISPLAY > Heading** to **MAG** (your compass reads magnetic, so compare like with like). Point the DPV at N, E, S and W on your calibrated compass and compare each against the display. Each should agree within a couple of degrees. If any cardinal is off by more than ~5°, redo **CAL > Hdg cal**; if the errors are large (more than 10°) or inconsistent, redo **CAL > Mounted** first. Set the heading display back to **TRUE** (or whatever you navigate in) when you're done.
- **Battery fully charged.**
- **O-rings clean and lubed.** Clean every o-ring and seating surface, apply a thin film of grease, check the glands, close up, and do a vacuum test (see the assembly guide).
- **Set waypoints for the intended dive** on `tern.local` (see [Waypoints and HOME](#waypoints-and-home)):
  - the location of a known safe ascent point
  - the primary target
  - any known landmarks or intermediate waypoints along the way
- **Pull any calibration you accepted on the website.** If you accepted a calibration on Dive Map rather than on the unit, it isn't on the unit until you press **Check for updates** under **Calibration Cloud Sync** on `tern.local`.

## At the Beach

1. **Turn on the unit** and put in a **dry desiccant bag**.
2. Close the tube and run **one last pressure check.**
3. **Check the boot screen.** Every subsystem should report OK (see [Power On and Self-Test](#power-on-and-self-test)). The unit boots in whatever Op Mode it was last in. It should show **SURF** here.
4. **Wait for a good GPS fix, then turn on logging.** Once the nav-board LED is **blinking green**, the unit is using the GPS position, but it isn't in a log yet. Set **Log** to LOW or MID (see [Log levels](#log-levels)). A few seconds later the LED goes **solid green**: a good fix is in the log.
5. **Select WP** → the first landmark or target.  Verify that the bearing and range make sense.

## Before Descent

1. **Raise the GPS mast** and hold it as high as possible.
2. **Wait until the LED on the nav unit goes solid green.** If the antenna dipped during the surface swim, the LED will have dropped back to red or amber. Solid green means a fresh, good fix is in the log, and that your position is as up to date and correct as possible before you start the dive.  That's also the fix the post-dive track correction anchors on.
3. **Descend.** With a depth sensor, dive mode switches on by itself at about 4 ft (1.3 m). Without one, optionally, switch it yourself: **SETUP > Op Mode → DIVE**. Dive mode turns off GPS and WiFi; the LED drops to a faint green heartbeat every 4 s. Calibration is removed from the menu during DIVE mode.

## During the Dive

Do the dive just like you normally would, except better informed. The display shows bearing and range to the selected waypoint, your heading, and your speed.

- **Arrive WP whenever you arrive at any marked point.** This snaps the dead-reckoning estimated position to that waypoint's true lat/lon, which clears the error built up on the leg behind you.
- **Select WP** to set a new destination whenever you like. **Select WP → HOME** takes you back to your starting point.
- **Mark** anything you want to find again later: with the menu closed, a single **BTN2** press. The screen flashes **MARKED** (or a red **NOT LOGGING** if logging is off, because Marks are only recorded in the log).
- **Take occasional current measurements** with **Current**: hold station pointing upstream for up to 60 s (see [Measuring the Current](#measuring-the-current)). In a typical lake dive, that might mean one reading in the shallows and another at 40 m. On a trip to the Alki Fishing Reef, it might mean one near the beginning, one at the reef, and one at the end.

## On Surfacing

1. **Leave dive mode.** With a depth sensor, the unit returns to surface mode after staying shallower than ~2 ft (0.6 m) for 30 seconds. The LED stays dark during that dwell, then starts again from red as the GPS comes back. Without a depth sensor, switch it yourself: **SETUP > Op Mode → SURF**.
2. **Raise the mast and get a closing GPS fix.** Wait for solid green: the surfacing fix is now in the log.
3. **Turn Log OFF.** That closes the file, and a closed file is what the automatic upload sends the next time the unit joins a known network. A file that is still open is skipped, so forgetting this step silently delays the dive's upload.

---

# Reference

## Hardware

- **Nav device**: ESP32 with LSM6DS33/LIS3MDL IMU, Adafruit GPS, hall-effect flow sensor
- **Display device**: ESP32 with ST7789 TFT (320×240), two buttons
- The two devices are connected over a wired Serial1 link.

## Power On and Self-Test

1. Power on both devices. The nav device boots first. The display shows "Waiting..." until the link is established.

2. **Self-test sequence** (automatic):
    - IMU present: checks WHO_AM_I registers for LSM6DS33 + LIS3MDL
    - Calibration load: loads mag/gyro/accel calibration from LittleFS
    - If no calibration files are found, runs interactive calibration (90 s mag sweep, 10 s gyro at rest, 6-point accel) and reports cal FAILED (it needs baseline and mounted to get OK)
    - Fourier heading calibration loaded from `hdg_fourier.json` if present (silently skipped if absent)
    - GPS initialized (streams NMEA, doesn't wait for a fix); reports OK if it connected over serial
    - Flow sensor initialized (ISR-based pulse counting)
    - **NVS state restored**: the previous session's toggle states (GPS, WiFi, op mode, log level) and last estimated position are reloaded from ESP32 NVS flash
    - Display shows a boot status screen with pass/fail for each subsystem

3. **If recalibration is needed**: open the menu (BTN1) → **CAL > Mounted** to re-collect magnetometer samples for the installed position, or **CAL > Hdg cal** to collect new heading correction data.

## Nav-Board Status LED

The NeoPixel on the nav board, visible through the clear housing. Underway, it's only a heartbeat: a faint green blink every few seconds. At the surface, it shows GPS fix status.

| LED | Meaning |
|---|---|
| Solid green | A gate-accepted GPS fix is in the current log file since the GPS last started. Ready to descend. |
| Green blink | Nav is using the GPS position, but it isn't in a log yet: logging is OFF, or the first row hasn't been written. Turn on **Log** in the menu. |
| Amber, slow blink | A fix, but the confidence gate is still refusing it (typical for the first fixes after power-up or surfacing). Wait. |
| Red, fast blink | A fix too poor to ever be accepted. |
| Red, slow blink | Searching: no fix yet. |
| Red/yellow flicker | No data from the GPS module at all: unplugged, or dead. |
| Fast magenta | Nav failed its boot self-test and halted. Shown even if the display is dark, since the display link may be what failed. |
| Brief faint green every 4 s | Heartbeat: dive mode (or GPS turned off) and heading is live. No flash at all means nav is blocked or its heading has gone bad. |
| Dark | Powered off. Also briefly after surfacing: GPS stays off for the 30 s surface dwell, then the sequence starts again from red. |

After a dive, solid green again means the *surfacing* fix is logged. It resets every time the GPS restarts and every time a new log file opens.

## Buttons

| Button | Action | Effect |
|--------|--------|--------|
| BTN1 short press | Open / cycle menu | Opens the menu (if closed) or moves to the next item (if open) |
| BTN2 short press | Select menu item | Enters a submenu, runs an action, or goes back |
| BTN2 short press (menu closed, dive mode) | Mark | Marks current position in the log; flashes MARKED (or NOT LOGGING) |
| BTN1 + BTN2 held 2 s | Reset | Resets the display device |
| No button for 15 s | Auto-close | Menu closes, returns to the full nav display. Reopening within 2 minutes puts you back on the item you were on. |

**Note:** this is basically the Shearwater 2-button "change-select" model for operating a menu, which should be familiar to most divers.

## Menu

Press **BTN1** to open the on-screen menu. It appears in the lower half of the display while the status bar and bearing/range keep updating above.

### Menu Structure

Items marked *(surface)* are hidden in dive mode, so the list you cycle through underwater holds only what's useful there.

```
MENU
├── Select WP      — open waypoint selector: navigate TO a named waypoint
├── Arrive WP      — open waypoint selector: snap current position to a known waypoint
├── Mark           — mark current position in the log
├── Current        — measure the current: hold station pointing upstream (up to 60 s)
├── Log            — cycle log level: OFF / LOW / MID / HI (off is immediate; others start 5 s later)
├── DISPLAY
│   ├── Spd/ETA    — toggle speed vs ETA readout
│   ├── Units      — toggle meters vs feet (applies to distance, speed, and depth)
│   └── Heading    — cycle TRUE / MAG / RAW heading
├── SETUP
│   ├── GPS        — (surface) toggle GPS position + speed on/off
│   ├── WiFi       — (surface) toggle WiFi on/off
│   ├── Water      — (surface) toggle salt/fresh water density for depth calc (see Depth Sensor below)
│   ├── Op Mode    — toggle dive/surface mode (shows DIVE or SURF); the only SETUP item at depth
│   └── Link acct  — (surface) begin device-auth cloud account link
├── CAL            — (surface, whole submenu)
│   ├── Baseline   — magnetometer calibration, device off DPV (full sphere coverage), cloud-fit on completion
│   ├── Fill gaps  — patches thin/empty regions on an installed Baseline cal (requires synced target map)
│   ├── Mounted    — bin-aware magnetometer calibration, device on DPV (corrects DPV mag signature), cloud-fit on completion
│   ├── Hdg cal    — Fourier heading calibration: guided 12-point collection, then cloud-fit on completion
│   └── Speed cal  — interactive flow-meter k-factor calibration (swim a known distance)
├── OFF            — (surface) enter deep sleep (two presses to confirm; wake: hold both buttons ~1s)
└── Close          — leave the menu, change nothing
```

In dive mode the menu is: Select WP, Arrive WP, Mark, Current, Log, DISPLAY, SETUP (Op Mode only), Close. If dive mode switches on while you are inside CAL or on a hidden item, the menu moves you back to the root or on to the next visible item.

Toggle items show their current state (e.g. "Units: m", "Op Mode: SURF", "Log: OFF") and stay open after toggling. Non-toggle items (Baseline, Fill gaps, Mounted, Hdg cal, Mark, Current) run and close the menu. Select WP and Arrive WP open the waypoint selector.

All toggle states (GPS, WiFi, Water, Op Mode, Log level, and all Display settings) are saved to NVS immediately on change and restored on the next boot.

### Mark

**Mark is one press at depth.** In dive mode, with the menu closed, a single BTN2 press marks your position. Pressing both buttons together never marks. Every Mark (this shortcut or the menu item) flashes **MARKED** in the lower half of the screen for about 1.5 s. If logging is off it flashes a red **NOT LOGGING** instead, because a Mark is only recorded in the log file.

### Log Levels

**Logging starts on a delay and stops immediately.** The cycle is off → low → mid → high → off, so getting to any level means passing through the others. The menu label updates as soon as you press, but a **low**, **mid** or **high** selection waits for 5 seconds of no further presses before a log file is actually opened. That way, passing through a level on the way to the one you want doesn't leave a stub one-row file behind. Landing on **off** takes effect at once: the open file is closed immediately, which is also what makes it available to the automatic upload. If you want high, wait for the label to sit still on `Log:HI` before backing out. Logging is gated on movement, so (if it's working correctly) turning on the log and leaving the unit by the water doesn't write a giant useless log.

- **LOW**: logs only time, position, course, speed, and temperature. The minimum needed to track a dive on the website afterwards.
- **MID**: logs everything, in binary, so it ends up roughly the same size as a LOW log. Notably, it adds raw magnetics, to support or refute the hypothesis that there are magnetic anomalies in the lake.
- **HI**: only useful for debug and diagnostics, not for a normal dive. It's raw CSV, not binary, so it fills the limited space quickly.

### OFF

**OFF needs two presses.** The first BTN2 on OFF arms it and the item repaints as `OFF:SURE?`; a second BTN2 shuts the unit down. Pressing BTN1 (or letting the menu time out) disarms it. OFF sits second-to-last on purpose, with the harmless **Close** after it, so overshooting the end of the list lands on a no-op rather than on the one action you can't undo. In dive mode OFF is hidden: there's no reason to power off underwater.

### Waypoint Selector (Select WP and Arrive WP)

Selecting **Select WP** or **Arrive WP** opens a full-screen waypoint picker:

```
NAV TO:           (or "ARRIVED AT:")
─────────────────────────────────────
  Bomber
  1 / 4
─────────────────────────────────────
BTN1: next   BTN2: confirm
```

- **BTN1** cycles forward through the waypoint list (wraps around)
- **BTN2** confirms the selection:
  - **Select WP**: sets that waypoint as the navigation destination; the display switches to bearing/range
  - **Arrive WP**: snaps the dead-reckoning position to that waypoint's lat/lon. Use this when you arrive at a known location to reset accumulated error before the next leg.
- After BTN2 confirms, the selector closes and the display returns to navigation.

**HOME** is always the first waypoint (index 0). User-defined waypoints appear after it in the order they were added.

## Waypoints and HOME

**HOME** is always waypoint index 0. Its lat/lon is updated automatically whenever GPS signal quality reaches 3 bars or more. No manual action is needed: by the time you enter the water, HOME reflects the last good GPS fix. To navigate back to your starting point, open **Select WP**, cycle to **HOME**, and confirm.

**Named waypoints** are managed on the web interface at `tern.local` (or `192.168.4.1` when connected to the Tern AP). Scroll to the **Waypoints** section:
- The table shows all stored waypoints (name, lat, lon). HOME can't be deleted.
- To add or update: enter a name (max 19 characters), latitude, and longitude, then click **Save**. You can paste a `lat, lon` pair directly into the Lat field and both fields fill automatically.
- To delete: click the **Delete** button next to any non-HOME waypoint.

Up to 200 named waypoints can be stored. They are saved to `/config/waypoints.json` on the nav device's LittleFS and persist across reboots.

## Measuring the Current

Sixty seconds of holding still, pointing upstream, as still as possible relative to the ground.

**Why.** The flow sensor measures your speed *through the water*, not over the ground. That's the right thing for navigation, and it's why drifting reads zero. But it also means the unit is blind to a current, and a current it can't see gets silently absorbed into your position.

There's no way to measure a current at depth by difference: no GPS, and usually no second known position. But if you *hold station* against it, the water still runs past the impeller, and that reading is the current itself.

**Workflow:**

1. Select **Current**. The menu closes and a 5-second countdown appears.
2. Get **pointed upstream** and stop swimming. Set scooter speed so you're holding position over a fixed point on the bottom: a rock, a piece of wreck, anything that isn't moving.
3. The countdown ends and a 60-second hold begins. The screen shows a large countdown of the seconds remaining, with the **live flow reading** and heading below it. Watch the flow: if you're slipping, you'll see the number wander, and you still have time to fix your position.
4. At the end, the result shows in m/s and m/min, with the direction the current is flowing *toward*, and how many seconds went into the average. Press either button to dismiss.
5. **BTN1 finishes early.** Pressing it during the hold ends it now and keeps what you held: the firmware drops the unsteady seconds at the start and end and averages the rest. Use it when you can't hold station any longer; a 25-second hold is still a measurement. BTN1 during the 5-second countdown cancels outright, since nothing has been measured yet.

**Requirements and gotchas:**

- **Every measurement is saved, logging or not**, to `/currents.csv` on the unit. Download it from `tern.local`. If logging is on it is *also* written into the dive log as a `C` row. See the [data logging guide](data-logging-guide.md) for the file format and exactly how the ends are trimmed.
- Point *upstream*, not downstream. The unit adds 180° for you, so the logged direction is where the water is going.
- Headings on the hold screens follow your **Display > Heading** setting and carry the same T / M / R suffix as the nav screen. What gets *saved* is always true north, whatever the screen shows.
- Position doesn't advance during the hold, by design: you aren't making way over the ground, and the displayed speed reads 0.0 throughout.
- It's worth taking several measurements on any dive where current is a factor (see [During the Dive](#during-the-dive)).
- For now, the system does nothing with current measurements in real time. They're used in post-processing to correct position after the fact. Once we've collected enough samples and corrected enough real dives to trust current-corrected position in real time, current measurements will feed into estimated position during the dive.

## Op Mode (Dive/Surface)

The device boots in the **last saved mode** (surface mode on first boot). Dive mode turns off GPS processing and the WiFi radio; surface mode turns both back on. The selection is saved to NVS immediately. Switch manually with **SETUP > Op Mode**.

**Automatic depth trigger:** if a depth sensor is installed (see [Depth Sensor](#depth-sensor)), Op Mode also switches itself, with no button press needed. Crossing ~4 ft (1.3 m) depth engages dive mode immediately (radios off fast). Returning to the surface reverts to surface mode automatically, but only after staying shallower than ~2 ft (0.6 m) for 30 seconds straight, so wave chop at the surface doesn't flap GPS/WiFi on and off. The manual menu toggle still works, but on a board with a depth sensor the automatic trigger can override it on the next check (e.g. forcing DIVE on the bench with no water reverts to SURFACE after ~30 s).

## Depth Sensor

An optional BlueRobotics Bar30 (MS5837-30BA) connects to the nav board's J3 header for live depth + water temperature. It's fully optional; boards without one work exactly as before.

- **Detection:** probed at boot; the nav device boot status screen shows a `Depth .. ok` or `.. FAIL` row like the other subsystems.
- **Zeroing:** the surface pressure is captured automatically a moment after boot, so power the unit on in air, not already submerged.
- **Water density:** SETUP > Water toggles salt (default) vs fresh water, which affects the pressure→depth conversion.
- **Display:** when present, depth is shown in small text on the bottom row of the nav screen, in the units selected by DISPLAY > Units.
- **Logging:** depth (m) and water temperature (°C) are recorded at every log level.
- **Dive trigger:** see **Automatic depth trigger** under [Op Mode](#op-mode-divesurface).

## Display Modes

Divers only ever see navigation mode. Debug mode is a bench build: set `DISPLAY_MODE 1` in [src/config.h](../src/config.h) (and `ENABLE_DEBUG_PACKET 1` so the nav device actually sends the sensor data) and reflash. It used to be a runtime toggle at **DISPLAY > Mode**, which nobody used and which sat one button press away from replacing the nav screen underwater.

### Navigation Mode (`DISPLAY_MODE 0`)

320×240 TFT. Updated at ~10 Hz, incrementally (only changed values redraw, to avoid flicker):

```
 ┌──────────────────────────────┐
 │ NAV  GP:OK  HM:SET  [▓▓▓░] W│  Status bar (24px)
 ├──────────────────────────────┤
 │ BRG            │ RNG         │
 │ 180T           │ 42m         │  Upper row
 ├────────────────┼─────────────┤
 │ HDG            │ SPD         │
 │ 063T           │ 23 m/m GPS  │  Lower row
 └────────────────┴─────────────┘
```

**Status bar** (top 24px):
- System state: `NAV` (green), `RDY` (cyan), `CAL` (yellow), `ERR` (red)
- GPS status: `GP:OK` (green), `GP:DG` (green, DGPS), `GP:--` (red, no fix)
- Home status: `HM:SET` (green) or `HM:---` (gray)
- GPS signal bars and WiFi / GPS-pos / GPS-spd / log-level indicators

**2×2 grid** (below status bar):
- **Upper-left (BRG)**: bearing to waypoint in degrees (0–360) + "T"/"M" suffix. Shows "---" when no waypoint is set.
- **Upper-right (RNG)**: range to waypoint in meters or feet. Shows "---" when no waypoint is set. Values of 1000 or more are shown in kilometers.
- **Lower-left (HDG)**: current heading in degrees (0–360) + "T"/"M" suffix
- **Lower-right (SPD)**: speed in m/min or ft/min. Source: "GPS" or "FLW" (flowmeter)

### Debug Mode (`DISPLAY_MODE 1`)

Requires `ENABLE_DEBUG_PACKET 1` on the nav device to send sensor data:

```
 MAG  -12.3   4.5  -8.1
      |M|= 15.2 uT
 ACC   0.02 -0.01  0.98
      |A|= 0.98 g
 GYR   .001 -.002  .000
 AHRS HDG: 063.2
 MAG  HDG: 058.7
 P: -2.1  R:  0.3
```

- **MAG**: calibrated magnetometer X/Y/Z (µT) + vector magnitude
- **ACC**: calibrated accelerometer X/Y/Z (g) + vector magnitude
- **GYR**: calibrated gyroscope X/Y/Z (rad/s)
- **AHRS HDG**: fused heading from the Mahony AHRS filter (with hdg_cal correction applied if loaded)
- **MAG HDG**: raw magnetic heading from atan2(Y,X), no tilt compensation
- **P/R**: pitch and roll in degrees

## How Dead Reckoning Works

Underwater, the device runs dead-reckoning integration at ~100 Hz:

- The flow sensor provides speed (or GPS speed, if the fix is fresh and passes the SOG deadband + COG coherence filter).
- The AHRS updates heading from gyro/accel/mag fusion. The Fourier heading correction is applied if `hdg_fourier.json` is loaded, and the motor-on heading offset if `motor_cal.json` is loaded.
- The nav model integrates position: `x += speed × sin(heading) × dt`, `y += speed × cos(heading) × dt`.

The display updates at 10 Hz. Every **Arrive WP** resets the integrated position to a known point, which is why error doesn't keep growing across a multi-leg dive.

## Configuration

Key settings in [src/config.h](../src/config.h):

| Constant | Default | Description |
|----------|---------|-------------|
| `DEFAULT_BASELINE_LAT` | 47.5 | Baseline latitude (°N) for local XY conversion |
| `DEFAULT_BASELINE_LON` | -122.5 | Baseline longitude (°W) for local XY conversion |
| `DEFAULT_USE_GPS_POSITION` | true | Use GPS lat/lon as position truth when available |
| `GPS_FIX_STALE_MS` | 3000 | Fall back to flowmeter speed if GPS fix older than 3s |
| `GPS_SOG_NOISE_FLOOR_KN` | 0.1 | SOG below this (knots) is always treated as noise |
| `GPS_SOG_TRUST_FLOOR_KN` | 2.0 | SOG above this (knots) is always trusted |
| `GPS_COG_COHERENCE_THRESH` | 0.85 | COG consistency required to trust mid-range SOG (0–1) |
| `GPS_COG_EMA_ALPHA` | 0.3 | COG EMA smoothing factor (~3–4 sample window at 1 Hz) |
| `FLOW_K_FACTOR` | 1.0 | Flow sensor pulses per L/min — updated automatically by Speed cal |
| `FLOW_CROSS_SECTION_M2` | 0.002 | Intake cross-section area in m² — set once to match DPV geometry |
| `DISPLAY_MODE` | 0 | Display mode: 0 = Navigation, 1 = Debug |
| `ENABLE_DEBUG_PACKET` | 0 | Nav device sends DebugPacket: 0 = off, 1 = on |
| `NVS_POS_SAVE_INTERVAL_MS` | 30000 | How often estimated position is written to NVS (ms) |
| `WATER_DENSITY_FRESH_KG_M3` / `WATER_DENSITY_SALT_KG_M3` | 997 / 1025 | Water density for depth calc — selected at runtime via SETUP > Water |
| `DEPTH_DIVE_TRIGGER_M` | 1.3 | Depth (m) that auto-triggers dive mode (GPS+WiFi off) |
| `DEPTH_SURFACE_REVERT_M` / `DEPTH_SURFACE_REVERT_DWELL_S` | 0.6 / 30 | Depth + dwell time to auto-revert to surface mode |

Units (metric m/m·min⁻¹ vs imperial ft/ft·min⁻¹) are a runtime toggle, DISPLAY > Units, not a compile-time setting. It governs distance, speed, and depth together.

## Troubleshooting

- **"NO LINK" on display**: check the Serial1 wiring between devices. The nav device should be sending packets.
- **Heading off by a constant amount**: run **CAL > Hdg cal** to collect 12-point data. It cloud-fits and shows accept/reject automatically if the unit has WiFi. If errors are large (>10°) or inconsistent, redo mounted mag cal first.
- **Heading varies by orientation (tilt-dependent)**: soft-iron distortion isn't fully corrected. Redo mounted mag cal (it cloud-fits automatically, same as above).
- **One part of the compass rose is worse than the rest after Hdg cal**: check Dive Map's Calibration History for a "thin sector(s)" flag on that calibration and fill it via the web form rather than redoing all 12 points. See **[Filling a Thin Heading Sector](#filling-a-thin-heading-sector)**.
- **Position drifts without moving**: check the gyro calibration (it should be done at rest). The flow sensor may be noisy; increase `FLOW_AVG_PERIOD_S`.
- **GPS shows speed when stationary**: GPS position jitter creates phantom speed. The SOG deadband + COG coherence filter should reject this. Enable `GPS_DIAG_ENABLE` in nav_main.cpp to see raw SOG/COG and the filter's decisions.
- **GPS not used for position**: make sure `DEFAULT_USE_GPS_POSITION = true` in config.h. GPS needs a clear view of the sky (not available underwater).
- **Website shows an accepted calibration the unit isn't using**: web-accepted results only reach the unit when you press **Check for updates** under **Calibration Cloud Sync** on `tern.local`.
