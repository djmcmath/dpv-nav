# DPV-Nav User Guide

## Hardware Setup
- **Nav device**: ESP32 with LSM6DS33/LIS3MDL IMU, Adafruit GPS, hall-effect flow sensor
- **Display device**: ESP32 with SSD1351 OLED (128×96), two buttons, MCP23017 backlight
- Devices connected via Serial1 link (wired)

## Power On at the Truck / Dock

1. Power on both devices. Nav device boots first, display shows "Waiting..." until link established.

2. **Self-test sequence** (automatic):
    - IMU present — checks WHO_AM_I registers for LSM6DS33 + LIS3MDL
    - Calibration load — loads mag/gyro/accel calibration from LittleFS
    - If no calibration files found, runs interactive calibration (30s mag sweep, 10s gyro at rest, 6-point accel)
    - GPS initialized (streams NMEA, does not block waiting for fix)
    - Flow sensor initialized (ISR-based pulse counting)
    - **NVS state restored** — previous session's toggle states (GPS, WiFi, op mode, log level) and last estimated position are reloaded from ESP32 NVS flash
    - Display shows boot status lines: "Display...ok", "Backlight...ok", "Link...ok"

3. **Quick heading sanity check**: Compare displayed heading against a phone compass or known bearing.

4. **If recalibration needed**: Open menu (BTN1) → **CAL > Quick cal** (BTN2) to trigger magnetometer hard-iron recalibration. Rotate device through all orientations for 30 seconds. The OLED switches to a calibration progress screen automatically.

## Setting Home

- Open the menu (BTN1), navigate to **NAV > Home**, press BTN2 to set home
- This snapshots your current position as the home waypoint
- Display shows distance/bearing relative to home

## Menu System

Press **BTN1** to open the on-screen menu. The menu appears in the bottom third of the display while the status bar and bearing/range continue updating above.

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
├── NAV
│   ├── Outbound   — select outbound waypoint as destination
│   ├── Home       — set current position as home waypoint
│   ├── Mark       — mark current position in logs
│   └── Op Mode    — toggle dive/surface mode (shows DIVE or SURF)
├── CAL
│   ├── Quick cal  — 30-second hard-iron magnetometer sweep (bias only, updates saved to LittleFS)
│   ├── Full cal   — 120-second raw mag data collection for offline soft-iron ellipsoid fitting
│   └── Speed cal  — flow meter speed calibration (stub)
├── INPUT
│   ├── GPS Pos    — toggle GPS position on/off (shows current state)
│   ├── GPS Spd    — toggle GPS speed on/off
│   ├── WiFi       — toggle WiFi on/off
│   └── Logging    — cycle log level: high / med / off
└── DISPLAY
    ├── Mode       — toggle debug vs navigate display
    ├── Spd/ETA    — toggle speed vs ETA readout
    ├── Units      — toggle meters vs feet
    └── Heading    — toggle magnetic vs true heading
```

Toggle items show their current state (e.g., "Units: m", "Op Mode: SURF") and stay open after toggling. Non-toggle items (Home, Mark, Quick cal, Full cal) execute and close the menu.

All toggle states (GPS Pos, GPS Spd, WiFi, Op Mode, Log level, and all Display settings) are saved to NVS immediately on change and restored on next boot.

### Calibration Progress Screen

When Quick cal or Full cal is running, the normal nav display is replaced with a dedicated calibration screen:

```
MAG CAL
QUICK (hard-iron)         ← or "FULL (soft-iron)"

Remaining:  27s
[============        ]    ← green progress bar (coverage)
Coverage:  48%
[============        ]    ← cyan bar (same metric)

ROTATE DEVICE             ← blinks at 1 Hz
```

- **Remaining** — seconds left in the calibration
- **Coverage** — for quick cal: minimum axis coverage (how completely each X/Y/Z axis has been spanned); for full cal: time elapsed as a percentage
- The screen returns to normal navigation automatically when calibration finishes

**Op Mode (Dive/Surface):** The device boots in the **last saved mode** (surface mode on first boot). Before entering the water, toggle to **dive mode** via NAV > Op Mode — this disables GPS processing and turns off the WiFi radio, and the selection is saved to NVS immediately. On surfacing, toggle back to surface mode to re-enable both.

The menu definition is stored in `/menu.json` on LittleFS. To customize, edit `data/menu.json` and upload with `pio run -e display -t uploadfs`.

## Display Modes

The display mode can be toggled at runtime via the **DISPLAY > Mode** menu item (or set at compile time via `DISPLAY_MODE` in [src/config.h](../src/config.h)).

### Navigation Mode (`DISPLAY_MODE 0`)

Updated at ~10 Hz with 4-slot frame rotation (each region refreshes at ~2.5 Hz):

```
 ┌──────────────────────────────┐
 │ NAV  GP:OK  HM:SET           │  Status bar (12px)
 ├──────────────┬───────────────┤
 │ HDG          │ RNG           │
 │ 063M         │ 42m           │  Upper row (42px cells)
 ├──────────────┼───────────────┤
 │ BRG          │ SPD           │
 │ 180M         │ 23 m/m GPS   │  Lower row (40px cells)
 └──────────────┴───────────────┘
```

**Status bar** (top 12px):
- System state: `NAV` (green), `RDY` (cyan), `CAL` (yellow), `ERR` (red)
- GPS status: `GP:OK` (green), `GP:DG` (green, DGPS), `GP:--` (red, no fix)
- Home status: `HM:SET` (green) or `HM:---` (gray)

**2×2 grid** (below status bar, separated by cyan divider lines):
- **Upper-left (HDG)**: Current heading in degrees (0-360), size 3 font + "T"/"M" suffix (true/magnetic)
- **Upper-right (RNG)**: Range to home in meters or feet (configurable). Shows "---" when no home set. Values ≥1000 shown as "1.2k"
- **Lower-left (BRG)**: Bearing to home in degrees (0-360), size 2 font + "T"/"M" suffix. Shows "---" when no home set
- **Lower-right (SPD)**: Speed in m/min or ft/min (rounded whole number), size 2 font. Source: "GPS" or "FLW" (flowmeter). Units: "m/m" or "ft/m"

### Debug Mode (`DISPLAY_MODE 1`)

Requires `ENABLE_DEBUG_PACKET 1` on the nav device to send sensor data. All text size 1 (6×8 px):

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
- **AHRS HDG**: Fused heading from Mahony AHRS filter (green)
- **MAG HDG**: Raw magnetic heading from atan2(Y,X), no tilt compensation (yellow)
- **P/R**: Pitch and roll in degrees

## Dive Navigation Workflow

1. At the surface: set home (NAV > Home), then switch to dive mode (NAV > Op Mode → DIVE) to disable GPS and WiFi.
2. Descend with DPV, unit active.
3. Device runs dead-reckoning integration at ~100 Hz:
    - Flow sensor updates speed (or GPS speed, if fix is fresh and passes SOG deadband + COG coherence filter — see Configuration)
    - AHRS updates heading from gyro/accel/mag fusion
    - Nav model integrates position: `x += speed × sin(heading) × dt`, `y += speed × cos(heading) × dt`
4. Display updates at 10 Hz, showing heading, speed, and position.
5. If in surface mode and GPS has a fix and GPS position is enabled, position snaps to GPS truth. In dive mode, GPS is disabled and position relies entirely on dead reckoning.
6. To return home: follow the bearing shown on the bottom row.
7. On surfacing: switch back to surface mode (NAV > Op Mode → SURF) to re-enable GPS and WiFi.

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
| `DEFAULT_BASELINE_LAT` | 42.0 | Baseline latitude (°N) for local XY conversion |
| `DEFAULT_BASELINE_LON` | -122.0 | Baseline longitude (°W) for local XY conversion |
| `DEFAULT_USE_GPS_POSITION` | true | Use GPS lat/lon as position truth when available |
| `GPS_FIX_STALE_MS` | 3000 | Fall back to flowmeter speed if GPS fix older than 3s |
| `GPS_SOG_NOISE_FLOOR_KN` | 0.5 | SOG below this (knots) is always treated as noise |
| `GPS_SOG_TRUST_FLOOR_KN` | 2.0 | SOG above this (knots) is always trusted |
| `GPS_COG_COHERENCE_THRESH` | 0.85 | COG consistency required to trust mid-range SOG (0–1) |
| `GPS_COG_EMA_ALPHA` | 0.3 | COG EMA smoothing factor (~3–4 sample window at 1 Hz) |
| `FLOW_K_FACTOR` | 1.0 | Flow sensor pulses per L/min (calibrate to match sensor) |
| `FLOW_CROSS_SECTION_M2` | 0.002 | Intake cross-section area in m² (calibrate to match DPV) |
| `DISPLAY_MODE` | 0 | Display mode: 0 = Navigation, 1 = Debug |
| `DISPLAY_UNITS_IMPERIAL` | 0 | Units: 0 = metric (m, m/min), 1 = imperial (ft, ft/min) |
| `ENABLE_DEBUG_PACKET` | 0 | Nav device sends DebugPacket: 0 = off, 1 = on |
| `DEBUG_SEND_INTERVAL_MS` | 200 | Debug packet send rate in ms (5 Hz default) |
| `NVS_POS_SAVE_INTERVAL_MS` | 30000 | How often estimated position is written to NVS (ms) |

## Troubleshooting

- **"NO LINK" on display**: Check Serial1 wiring between devices. Nav device should be sending packets.
- **Heading wrong**: Verify axis mapping in nav_main.cpp. Ensure IMU is oriented correctly.
- **Position drifts without moving**: Check gyro calibration (should be done at rest). Flow sensor may be noisy — increase `FLOW_AVG_PERIOD_S`.
- **GPS shows speed when stationary**: GPS position jitter creates phantom speed. The SOG deadband + COG coherence filter should reject this. Enable `GPS_DIAG_ENABLE` in nav_main.cpp to see raw SOG/COG and filter decisions. Raise `GPS_SOG_NOISE_FLOOR_KN` if jitter SOG is higher than 0.5 kn.
- **GPS not used for position**: Ensure `DEFAULT_USE_GPS_POSITION = true` in config.h. GPS needs clear sky view (not available underwater).
