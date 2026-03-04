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
    - Display shows boot status lines: "Display...ok", "Backlight...ok", "Link...ok"

3. **Quick heading sanity check**: Compare displayed heading against a phone compass or known bearing.

4. **If recalibration needed**: Long-press BTN2 (2 seconds) to trigger magnetometer recalibration. Rotate device through all orientations for 10 seconds.

## Setting Home

- At the descent line or planned return point, **short-press BTN1** → "SET HOME"
- This snapshots your current position as the home waypoint
- Position resets to show distance/bearing relative to home
- Display bottom row changes from `X:+0 Y:+0` to `H:   0m 000°`

## Display Modes

The display mode is set at compile time via `DISPLAY_MODE` in [src/config.h](../src/config.h): `0` = Navigation (default), `1` = Debug.

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

1. Descend with DPV, unit active.
2. Device runs dead-reckoning integration at ~100 Hz:
    - Flow sensor updates speed (or GPS speed if fix available and <3s old)
    - AHRS updates heading from gyro/accel/mag fusion
    - Nav model integrates position: `x += speed × sin(heading) × dt`, `y += speed × cos(heading) × dt`
3. Display updates at 10 Hz, showing heading, speed, and position.
4. If GPS has a fix and GPS position is enabled (`DEFAULT_USE_GPS_POSITION = true` in config.h), position snaps to GPS truth. This is useful for surface testing but GPS is typically unavailable underwater.
5. To return home: follow the bearing shown on the bottom row.

## Button Reference

| Button | Action | Effect |
|--------|--------|--------|
| BTN1 short press | Toggle home | SET_HOME (if no home) or CLEAR_HOME (if home set) |
| BTN2 long press (2s) | Mag calibration | Triggers 10-second magnetometer recalibration |

## Configuration

Key settings in [src/config.h](../src/config.h):

| Constant | Default | Description |
|----------|---------|-------------|
| `DEFAULT_BASELINE_LAT` | 42.0 | Baseline latitude (°N) for local XY conversion |
| `DEFAULT_BASELINE_LON` | -122.0 | Baseline longitude (°W) for local XY conversion |
| `DEFAULT_USE_GPS_POSITION` | true | Use GPS lat/lon as position truth when available |
| `GPS_FIX_STALE_MS` | 3000 | Fall back to flowmeter speed if GPS fix older than 3s |
| `FLOW_K_FACTOR` | 1.0 | Flow sensor pulses per L/min (calibrate to match sensor) |
| `FLOW_CROSS_SECTION_M2` | 0.002 | Intake cross-section area in m² (calibrate to match DPV) |
| `DISPLAY_MODE` | 0 | Display mode: 0 = Navigation, 1 = Debug |
| `DISPLAY_UNITS_IMPERIAL` | 0 | Units: 0 = metric (m, m/min), 1 = imperial (ft, ft/min) |
| `ENABLE_DEBUG_PACKET` | 0 | Nav device sends DebugPacket: 0 = off, 1 = on |
| `DEBUG_SEND_INTERVAL_MS` | 200 | Debug packet send rate in ms (5 Hz default) |

## Troubleshooting

- **"NO LINK" on display**: Check Serial1 wiring between devices. Nav device should be sending packets.
- **Heading wrong**: Verify axis mapping in nav_main.cpp. Ensure IMU is oriented correctly.
- **Position drifts without moving**: Check gyro calibration (should be done at rest). Flow sensor may be noisy — increase `FLOW_AVG_PERIOD_S`.
- **GPS not used for position**: Ensure `DEFAULT_USE_GPS_POSITION = true` in config.h. GPS needs clear sky view (not available underwater).
