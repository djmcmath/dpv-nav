# DPV-Nav User Guide

This guide is in three parts:

1. **[Calibration](#calibration)**: done once for a new installation, and again whenever the physical setup changes, or if, for any reason, a calibration check fails. Summarised here; the full walkthrough is [user-calibration-guide.md](user-calibration-guide.md).
2. **[Diving with DPV-Nav](#diving-with-dpv-nav)**: the routine from the night before, through the dive, to the surface.
3. **[Reference](#reference)**: menus, screens, settings and troubleshooting.

---

# Calibration

**The full walkthrough is in
[user-calibration-guide.md](user-calibration-guide.md)** — why each step exists,
the per-step procedure and screens, what a good result looks like, and what
invalidates what. It used to live here; it outgrew a section. This is the
summary.

Do these in order. Each step is fitted on top of the one before it, which is
why redoing an early step means redoing the later ones. The numbers match the
CAL menu and the step strip across the top of every cal screen.

| # | Step | Where | Solves for |
|---|---|---|---|
| 1 | **[Baseline](user-calibration-guide.md#step-1--baseline-cal--baseline-1)** (`CAL > Baseline (1)`) | **off** the DPV, in your hands | The unit's own hard and soft iron, fit over a full sphere |
| 1a | **[Gap fill](user-calibration-guide.md#step-1a--gap-fill-cal--fill-gaps-1a)** (`CAL > Fill gaps (1a)`, optional) | **off** the DPV | The orientations step 1 missed. Merged into the baseline, not a redo |
| 2 | **[Mounted](user-calibration-guide.md#step-2--mounted-cal--mounted-2)** (`CAL > Mounted (2)`) | **on** the DPV, on a stand | The scooter's magnetic signature, as a constrained fit on top of the baseline |
| 3 | **[Heading](user-calibration-guide.md#step-3--heading-cal--heading-3)** (`CAL > Heading (3)`) | on the DPV, against a known-good compass | Whatever heading error survives steps 1–2; also ties the unit to north |
| 4 | **[Speed](user-calibration-guide.md#step-4--speed-cal--speed-cal)** (`CAL > Speed cal`) | in the water, known distance | The flow sensor's k-factor |

Gyro and accel calibration run automatically at first boot if their files are
absent. That sequence runs over the **USB serial port**, not the display — so if
a new unit powers on but never connects to the display, plug in USB and expect
to walk through it.

**Every cal screen works the same way.** Across the top, the step strip
(`1 1a 2 3`) marks the step you're on, and the number on the right counts toward
whatever ends it: samples, cells or marks. One line near the bottom says what to
do now: red means something is blocking you, yellow means adjust, green means
good. Under it are two keys, one per button: **BTN1 on the left, BTN2 on the
right**, each labelled with what it does right now.

- **BTN2 keeps going:** FINISH, MARK, ACCEPT, OK.
- **Hold BTN1 for 2 seconds to throw the work away:** QUIT while collecting,
  REJECT on a result. The left key fills red while you hold it; let go early and
  nothing happens. A quick press only shows a reminder, so a stray tap can't
  cost you a collection.
- Holding both buttons for 2 seconds still resets the display, and never quits a
  cal.

**Every step uses the same cloud round trip.** When collection finishes, the unit
uploads the samples (if it has WiFi and a linked account), the server fits them,
and the display shows **Uploading** followed by the result: one number, with
**GOOD**, **OK** or **POOR** beside it. **ACCEPT** (BTN2) installs it
immediately, with no reboot, and the next screen says what's next. Holding BTN1
to **REJECT** keeps whatever was active before. With no WiFi, the samples are
still saved to the unit, and you can upload them later from `tern.local`, or use
the offline Python fit.

**Before a dive, check it.** The 4-point check is in
[Checking a finished calibration](user-calibration-guide.md#checking-a-finished-calibration),
and is also step one of [The Night Before](#the-night-before) below.

---

# Diving with DPV-Nav

Navigation is only as good as the last known position it starts from, so most of this routine is about two things: making sure the heading is trustworthy, and getting a good GPS fix into the log right before you descend and right after you surface. The post-dive track correction anchors on those fixes, and on any waypoints you reported during the dive.  The farther apart truth-points are (GPS fixes or marked landmark arrivals), the more accurate the corrected track will be.  If you mark GPS at the descent line, do a dive with no marked waypoints, and report GPS at the same point at the end, the track correction will have no idea where you actually went on the dive, aside from dragging the final estimated position to the second GPS fix (rubber sheet style correction).  

## The Night Before

- **4-point heading check.** Set **DISPLAY > Heading** to **MAG** (your compass reads magnetic, so compare like with like). Point the DPV at N, E, S and W on your calibrated compass and compare each against the display. Each should agree within a couple of degrees. If any cardinal is off by more than ~5°, redo **CAL > Heading (3)**; if the errors are large (more than 10°) or inconsistent, redo **CAL > Mounted (2)** first. Set the heading display back to **TRUE** (or whatever you navigate in) when you're done.
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
3. **Descend.** With a depth sensor, dive mode switches on by itself at about 10 ft (3 m). GPS and WiFi stay on through the surface swim, even with the unit riding a metre down in the wave-wash. Without one, optionally, switch it yourself: **SETUP > Op Mode → DIVE**. Dive mode turns off GPS and WiFi; the LED drops to a faint green heartbeat every 4 s. Calibration is removed from the menu during DIVE mode.

## During the Dive

Do the dive just like you normally would, except better informed. The display shows bearing and range to the selected waypoint, your heading, and your speed.

- **Arrive WP whenever you arrive at any marked point.** This snaps the dead-reckoning estimated position to that waypoint's true lat/lon, which clears the error built up on the leg behind you.
- **Select WP** to set a new destination whenever you like. **Select WP → HOME** takes you back to your starting point.
- **Mark** anything you want to find again later: with the menu closed, a single **BTN2** press. The screen flashes **MARKED** (or a red **NOT LOGGING** if logging is off, because Marks are only recorded in the log).
- **Take occasional current measurements** with **Current**: hold station pointing upstream for up to 60 s (see [Measuring the Current](#measuring-the-current)). In a typical lake dive, that might mean one reading in the shallows and another at 40 m. On a trip to the Alki Fishing Reef, it might mean one near the beginning, one at the reef, and one at the end.

## On Surfacing

1. **Leave dive mode.** With a depth sensor, the unit returns to surface mode after staying shallower than ~6.5 ft (2 m) for 15 seconds. A safety stop at 3–5 m stays in dive mode. The LED stays dark during that dwell, then starts again from red as the GPS comes back. Without a depth sensor, switch it yourself: **SETUP > Op Mode → SURF**.
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

3. **If recalibration is needed**: open the menu (BTN1) → **CAL > Mounted (2)** to re-collect magnetometer samples for the installed position, or **CAL > Heading (3)** to collect new heading correction data.

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
| Dark | Powered off. Also briefly after surfacing: GPS stays off for the 15 s surface dwell, then the sequence starts again from red. |

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
│   ├── Baseline (1)   — magnetometer calibration, device off DPV (full sphere coverage), cloud-fit on completion
│   ├── Fill gaps (1a) — patches thin/empty regions on an installed Baseline cal (needs its target map, which accepting the baseline loads)
│   ├── Mounted (2)    — bin-aware magnetometer calibration, device on DPV (corrects DPV mag signature), cloud-fit on completion
│   ├── Heading (3)    — Fourier heading calibration: guided 12-point collection, then cloud-fit on completion
│   └── Speed cal      — interactive flow-meter k-factor calibration (swim a known distance)
├── OFF            — (surface) enter deep sleep (two presses to confirm; wake: hold both buttons ~1s)
└── Close          — leave the menu, change nothing
```

In dive mode the menu is: Select WP, Arrive WP, Mark, Current, Log, DISPLAY, SETUP (Op Mode only), Close. If dive mode switches on while you are inside CAL or on a hidden item, the menu moves you back to the root or on to the next visible item.

Toggle items show their current state (e.g. "Units: m", "Op Mode: SURF", "Log: OFF") and stay open after toggling. Non-toggle items (Baseline, Fill gaps, Mounted, Heading, Mark, Current) run and close the menu. Select WP and Arrive WP open the waypoint selector.

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

**Automatic depth trigger:** if a depth sensor is installed (see [Depth Sensor](#depth-sensor)), Op Mode also switches itself, with no button press needed. Crossing ~10 ft (3 m) depth engages dive mode immediately (radios off). That is deep enough that the surface swim never trips it, even with the unit a metre down in the wave-wash, and a real descent passes it in seconds. Returning to the surface reverts to surface mode automatically after staying shallower than ~6.5 ft (2 m) for 15 seconds straight. The 2 m exit is shallower than a safety stop, so the stop stays in dive mode, and the 1 m gap between the two depths keeps the mode from flapping. The manual menu toggle still works, but on a board with a depth sensor the automatic trigger can override it on the next check (e.g. forcing DIVE on the bench with no water reverts to SURFACE after ~15 s).

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
| `DEPTH_DIVE_TRIGGER_M` | 3.0 | Depth (m) that auto-triggers dive mode (GPS+WiFi off) |
| `DEPTH_SURFACE_REVERT_M` / `DEPTH_SURFACE_REVERT_DWELL_S` | 2.0 / 15 | Depth + dwell time to auto-revert to surface mode |
| `DEPTH_OTA_UNDERWATER_M` | 0.6 | OTA updates are refused deeper than this |

Units (metric m/m·min⁻¹ vs imperial ft/ft·min⁻¹) are a runtime toggle, DISPLAY > Units, not a compile-time setting. It governs distance, speed, and depth together.

## Troubleshooting

- **"NO LINK" on display**: check the Serial1 wiring between devices. The nav device should be sending packets.
- **Heading off by a constant amount**: run **CAL > Heading (3)** to collect 12-point data. It cloud-fits and shows the result automatically if the unit has WiFi. If errors are large (>10°) or inconsistent, redo mounted mag cal first.
- **Heading varies by orientation (tilt-dependent)**: soft-iron distortion isn't fully corrected. Redo mounted mag cal (it cloud-fits automatically, same as above).
- **One part of the compass rose is worse than the rest after the heading cal**: check Dive Map's Calibration History for a "thin sector(s)" flag on that calibration and fill it via the web form rather than redoing all 12 points. See **[If one section of the circle comes out thin](user-calibration-guide.md#if-one-section-of-the-circle-comes-out-thin)**.
- **Position drifts without moving**: check the gyro calibration (it should be done at rest). The flow sensor may be noisy; increase `FLOW_AVG_PERIOD_S`.
- **GPS shows speed when stationary**: GPS position jitter creates phantom speed. The SOG deadband + COG coherence filter should reject this. Enable `GPS_DIAG_ENABLE` in nav_main.cpp to see raw SOG/COG and the filter's decisions.
- **GPS not used for position**: make sure `DEFAULT_USE_GPS_POSITION = true` in config.h. GPS needs a clear view of the sky (not available underwater).
- **Website shows an accepted calibration the unit isn't using**: web-accepted results only reach the unit when you press **Check for updates** under **Calibration Cloud Sync** on `tern.local`.
