# DPV-Nav User Guide

## Hardware Setup
- **Nav device**: ESP32 with LSM6DS33/LIS3MDL IMU, Adafruit GPS, hall-effect flow sensor
- **Display device**: ESP32 with ST7789 TFT (320×240), two buttons
- Devices connected via Serial1 link (wired)

## Power On at the Truck / Dock

1. Power on both devices. Nav device boots first, display shows "Waiting..." until link established.

2. **Self-test sequence** (automatic):
    - IMU present — checks WHO_AM_I registers for LSM6DS33 + LIS3MDL
    - Calibration load — loads mag/gyro/accel calibration from LittleFS
    - If no calibration files found, runs interactive calibration (90s mag sweep, 10s gyro at rest, 6-point accel)
    - Fourier heading calibration loaded from `hdg_fourier.json` if present (silently skipped if absent)
    - GPS initialized (streams NMEA, does not block waiting for fix)
    - Flow sensor initialized (ISR-based pulse counting)
    - **NVS state restored** — previous session's toggle states (GPS, WiFi, op mode, log level) and last estimated position are reloaded from ESP32 NVS flash
    - Display shows boot status screen with pass/fail for each subsystem

3. **Quick heading sanity check**: Compare displayed heading against a phone compass or known bearing.

4. **If recalibration needed**: Open menu (BTN1) → **CAL > Mounted** to re-collect magnetometer samples for the installed position, or **CAL > Hdg cal** to collect new heading correction data.

## Setting Home

HOME is always waypoint index 0. Its lat/lon is updated automatically whenever GPS signal quality reaches ≥ 3 bars. No manual action is needed — by the time you enter the water the HOME waypoint reflects the last good GPS fix.

To navigate back to your starting point: open **NAV > Select WP**, cycle to **HOME**, and confirm. The display will switch to showing bearing and range back to the HOME position.

## Setting Waypoints

Named waypoints are managed via the web interface at `tern.local` (or `192.168.4.1` when connected to the Tern AP).

On the web page, scroll to the **Waypoints** section:
- The table shows all stored waypoints (name, lat, lon). HOME cannot be deleted.
- To add or update: enter a name (max 19 characters), latitude, and longitude, then click **Save**. You can paste a `lat, lon` pair directly into the Lat field and both fields fill automatically.
- To delete: click the **Delete** button next to any non-HOME waypoint.

Up to 200 named waypoints can be stored. They are saved to `/config/waypoints.json` on the nav device's LittleFS and persist across reboots.

## Menu System

Press **BTN1** to open the on-screen menu. The menu appears in the lower half of the display while the status bar and bearing/range continue updating above.

### Menu Navigation
| Button | Action |
|--------|--------|
| BTN1 short press | Open menu / cycle to next item |
| BTN2 short press | Select item (enter submenu, execute action, or go back) |
| BTN1 + BTN2 held 2s | Reset display device |
| No button for 15s | Menu auto-closes |

### Menu Structure
```
MENU
├── OFF            — enter deep sleep (wake: hold both buttons ~1s)
├── NAV
│   ├── Select WP  — open waypoint selector: navigate TO a named waypoint
│   ├── Arrive WP  — open waypoint selector: snap current position to a known waypoint
│   ├── Mark       — mark current position in logs
│   └── Op Mode    — toggle dive/surface mode (shows DIVE or SURF)
├── CAL
│   ├── Baseline   — bin-aware magnetometer calibration, device off DPV (full sphere coverage)
│   ├── Mounted    — bin-aware magnetometer calibration, device on DPV (corrects DPV mag signature)
│   ├── Hdg cal    — Fourier heading calibration: guided 12-point collection, then offline fit + upload
│   └── Speed cal  — interactive flow-meter k-factor calibration (swim a known distance)
├── INPUT
│   ├── GPS Pos    — toggle GPS position on/off (shows current state)
│   ├── GPS Spd    — toggle GPS speed on/off
│   ├── WiFi       — toggle WiFi on/off
│   └── Logging    — cycle log level: off / low / high
└── DISPLAY
    ├── Mode       — toggle debug vs navigate display
    ├── Spd/ETA    — toggle speed vs ETA readout
    ├── Units      — toggle meters vs feet
    └── Heading    — toggle magnetic vs true heading
```

Toggle items show their current state (e.g., "Units: m", "Op Mode: SURF") and stay open after toggling. Non-toggle items (Baseline, Mounted, Hdg cal, Mark) execute and close the menu. Select WP and Arrive WP open the waypoint selector UI.

### Waypoint Selector UI (NAV > Select WP and Arrive WP)

Selecting **NAV > Select WP** or **NAV > Arrive WP** opens a full-screen waypoint picker:

```
NAV TO:           (or "ARRIVED AT:")
─────────────────────────────────────
  Bomber Line
  1 / 4
─────────────────────────────────────
BTN1: next   BTN2: confirm
```

- **BTN1** cycles forward through the waypoint list (wraps around)
- **BTN2** confirms the selection:
  - **Select WP**: sets that waypoint as the navigation destination; display switches to bearing/range
  - **Arrive WP**: snaps the dead-reckoning position to that waypoint's lat/lon — use this when you arrive at a known location to reset accumulated error before the next leg
- After BTN2 confirms, the selector closes and the display returns to navigation

**HOME** is always the first waypoint (index 0). User-defined waypoints appear after it in the order they were added.

Waypoints are managed via the web interface (see **Setting Waypoints** below).

All toggle states (GPS Pos, GPS Spd, WiFi, Op Mode, Log level, and all Display settings) are saved to NVS immediately on change and restored on next boot.

### Fourier Heading Calibration (CAL > Hdg cal)

After magnetometer calibration, systematic heading errors (5–20°) typically remain due to the DPV's magnetic geometry. Heading cal collects 12 data points for an offline Fourier fit that reduces residual error to ~2°.

**When to use:** After completing baseline + mounted mag cal. Run in a magnetically clean environment (not a garage with a concrete floor).

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
   The large number is the current live raw heading. Watch it settle before pressing.

2. Align the DPV to **0° (North)**. Wait for the heading readout to stop changing.

3. Press **BTN2**. The step advances to 030°, then 060°, … through all 12 steps at 30° intervals.

4. After Step 12, the nav device saves `/hdg_samples.csv` and shows:
   ```
   HDG CAL
   ─────────────────
   12 points saved

   Export /hdg_samples.csv
   Run fourier_fit.py
   Upload hdg_fourier.json
   ─────────────────
   Press BTN2 to exit
   ```

5. Press **BTN2** to return to normal navigation.

**Part 2 — Offline fit and upload:**

1. Download `/hdg_samples.csv` from the nav device LittleFS.
2. Run: `python tools/fourier_fit.py hdg_samples.csv`
3. Upload the resulting `hdg_fourier.json` to nav device LittleFS root.
4. Reboot — the correction loads automatically on next boot.

### Speed Calibration (CAL > Speed cal)

Speed cal measures how accurately the flow sensor reports speed by swimming a known distance and timing it. The computed k-factor is saved and averaged with up to 5 previous runs.

**Workflow:**

1. Select **CAL > Speed cal** from the menu. The menu closes and a distance-selection screen appears:
   ```
   SPEED CAL
   Set distance:
       300 ft
   BTN1: change
   BTN2: confirm
   ```
2. Press **BTN1** to cycle through distances: 150 → 200 → 250 → 300 → 350 → 400 → 450 → 500 → 150 ft (wraps). Press **BTN2** to confirm.

3. The display switches to a waiting screen. **Start the DPV** — the run begins automatically once the flow sensor detects motion above ~0.3 m/s.

4. Swim the selected distance in a straight line at typical cruising speed. The display shows a large elapsed-time counter:
   ```
   SPEED CAL
   RUNNING
        47
   300ft target
   Stop=slow/turn
   ```

5. The run ends automatically when either:
   - **Flow drops** below ~0.08 m/s (DPV stopped)
   - **Heading changes > 90°** after at least 30 seconds of stable heading (cross the finish and swing the scooter to signal done)

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
- A calm, straight stretch ≥150 ft works best — a pool lane, a wall, a pier
- The longer the distance, the more accurate the result
- After 3–6 runs the rolling average stabilises; use ACCEPT each time
- Use RESET+ACCEPT when the DPV has been serviced or the impeller changed

### Magnetometer Calibration Progress Screen (Baseline and Mounted)

When a bin-aware calibration is running (Baseline or Mounted), the display switches to a bin-coverage grid:

```
BASELINE CAL   (or "MOUNTED CAL")

     N NE  E SE  S SW  W NW  N NE  E SE
 60° [  ][  ][██][██][██][██][██][  ][  ][  ][  ][  ]
 30° [██][██][██][██][██][██][██][██][██][██][██][██]
  0° [██][██][██][██][██][██][██][██][██][██][██][██]
-30° [██][██][██][██][██][██][██][██][██][██][██][██]
-60° [  ][  ][  ][██][██][██][██][  ][  ][  ][  ][  ]

Hdg: 037°  Pit: -2°       45/60 bins green
```

Colors: **green** = fully covered, **yellow** = partial, **red** = sparse, **black** = empty.

The display returns to normal navigation automatically when all required bins turn green.

**Op Mode (Dive/Surface):** The device boots in the **last saved mode** (surface mode on first boot). Before entering the water, toggle to **dive mode** via NAV > Op Mode — this disables GPS processing and turns off the WiFi radio, and the selection is saved to NVS immediately. On surfacing, toggle back to surface mode to re-enable both.

## Display Modes

The display mode can be toggled at runtime via the **DISPLAY > Mode** menu item (or set at compile time via `DISPLAY_MODE` in [src/config.h](../src/config.h)).

### Navigation Mode (`DISPLAY_MODE 0`)

320×240 TFT. Updated at ~10 Hz, incremental (only changed values redraw to avoid flicker):

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
- **Upper-left (BRG)**: Bearing to home in degrees (0-360) + "T"/"M" suffix. Shows "---" when no home set
- **Upper-right (RNG)**: Range to home in meters or feet. Shows "---" when no home set. Values ≥1000 shown as "1.2k"
- **Lower-left (HDG)**: Current heading in degrees (0-360) + "T"/"M" suffix
- **Lower-right (SPD)**: Speed in m/min or ft/min. Source: "GPS" or "FLW" (flowmeter)

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

- **MAG**: Calibrated magnetometer X/Y/Z (µT) + vector magnitude
- **ACC**: Calibrated accelerometer X/Y/Z (g) + vector magnitude
- **GYR**: Calibrated gyroscope X/Y/Z (rad/s)
- **AHRS HDG**: Fused heading from Mahony AHRS filter (with hdg_cal correction applied if loaded)
- **MAG HDG**: Raw magnetic heading from atan2(Y,X), no tilt compensation
- **P/R**: Pitch and roll in degrees

## Dive Navigation Workflow

1. At the surface: use **NAV > Select WP** to select your destination waypoint. Switch to dive mode (**NAV > Op Mode → DIVE**) to disable GPS and WiFi.
2. Descend with DPV, unit active.
3. Device runs dead-reckoning integration at ~100 Hz:
    - Flow sensor updates speed (or GPS speed, if fix is fresh and passes SOG deadband + COG coherence filter)
    - AHRS updates heading from gyro/accel/mag fusion; Fourier heading correction applied if `hdg_fourier.json` loaded; motor-on heading offset applied if `motor_cal.json` loaded
    - Nav model integrates position: `x += speed × sin(heading) × dt`, `y += speed × cos(heading) × dt`
4. Display updates at 10 Hz, showing bearing, range, heading, and speed.
5. **At an intermediate known location** (e.g. a mooring ball or prior waypoint): use **NAV > Arrive WP** to snap the dead-reckoning position to that waypoint's true lat/lon. This resets accumulated error for the next leg.
6. To navigate home: use **NAV > Select WP** → HOME.
7. On surfacing: switch back to surface mode (**NAV > Op Mode → SURF**) to re-enable GPS and WiFi.

## Recommended Calibration Order (New Installation)

1. **Baseline mag cal** — device off DPV, full sphere coverage. Run offline Python fit, upload `mag_base.json`.
2. **Mounted mag cal** — device installed on DPV, horizontal rotations. Run offline Python fit, upload `mag_mount.json`.
3. **Fourier heading cal** — collect 12 points on DPV at 30° intervals, run `fourier_fit.py` offline, upload `hdg_fourier.json`.
4. **Speed cal** — 3–6 runs over a known distance to dial in the flow sensor k-factor.

Gyro and accel calibrate automatically on first boot if their files are absent.

## Button Reference

| Button | Action | Effect |
|--------|--------|--------|
| BTN1 short press | Open / cycle menu | Opens menu (if closed) or moves to next item (if open) |
| BTN2 short press | Select menu item | Enters submenu, executes action, or goes back |
| BTN1 + BTN2 held 2s | Reset | Resets the display device |
| No button for 15s | Auto-close | Menu closes, returns to full nav display |

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
| `DISPLAY_UNITS_IMPERIAL` | 0 | Units: 0 = metric (m, m/min), 1 = imperial (ft, ft/min) |
| `ENABLE_DEBUG_PACKET` | 0 | Nav device sends DebugPacket: 0 = off, 1 = on |
| `NVS_POS_SAVE_INTERVAL_MS` | 30000 | How often estimated position is written to NVS (ms) |

## Troubleshooting

- **"NO LINK" on display**: Check Serial1 wiring between devices. Nav device should be sending packets.
- **Heading off by constant amount**: Run **CAL > Hdg cal** to collect 12-point data, fit, and upload `hdg_fourier.json`. If errors are large (>10°) or inconsistent, redo mounted mag cal first.
- **Heading varies by orientation (tilt-dependent)**: Soft-iron distortion not fully corrected — redo mounted mag cal and run offline ellipsoid fitting.
- **Position drifts without moving**: Check gyro calibration (should be done at rest). Flow sensor may be noisy — increase `FLOW_AVG_PERIOD_S`.
- **GPS shows speed when stationary**: GPS position jitter creates phantom speed. The SOG deadband + COG coherence filter should reject this. Enable `GPS_DIAG_ENABLE` in nav_main.cpp to see raw SOG/COG and filter decisions.
- **GPS not used for position**: Ensure `DEFAULT_USE_GPS_POSITION = true` in config.h. GPS needs clear sky view (not available underwater).
