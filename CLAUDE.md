# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

DPV-Nav is an inertial navigation system for underwater DPVs (Diver Propulsion Vehicles / scooters). It provides tilt-compensated heading using AHRS (Mahony filter), dead-reckoning position via flowmeter integration, and displays bearing/distance to home on a TFT display.

**Target Hardware:** ESP32 (FeatherESP32), LSM6DS33 (accel/gyro), LIS3MDL (magnetometer), GPS, flow sensor, ST7789 TFT (320×240)
**Architecture:** Two-device system — nav device (sensors/AHRS/GPS/position) + display device (TFT/buttons), linked via Serial1 JSON packets at 10 Hz
**Language:** C++ (Arduino framework via PlatformIO)
**Main entry points:** [src/nav_main.cpp](src/nav_main.cpp) (nav device), [src/display_main.cpp](src/display_main.cpp) (display device)

## Build & Development Commands

### Build (Compile)
```bash
pio run -e nav        # Build nav device only
pio run -e display    # Build display device only
pio run               # Build both
```

### Upload to Device
```bash
pio run -e nav -t upload       # Upload to nav device (COM6)
pio run -e display -t upload   # Upload to display device (COM4)
```

Upload ports are configured in `platformio.ini`. To change, edit `upload_port` in the relevant `[env:]` section.

### Serial Monitor
```bash
pio device monitor
```

### Upload LittleFS Filesystem
```bash
pio run -e nav -t uploadfs
```

### Clean Build
```bash
pio run -t clean
```

## Code Architecture

### Namespace Organization

Code is organized into namespaces by subsystem:
- `imu::` - Sensor drivers and raw I2C reads (LSM6DS33 accel/gyro, LIS3MDL mag)
- `mahony::` - AHRS filter (quaternion-based attitude/heading reference)
- `math::` - Orientation calculations and conversions (quaternion ↔ Euler)
- `nav::` - Position estimation (dead reckoning, GPS truth, home waypoint)
- `gps::` - GPS driver (Adafruit Ultimate GPS, NMEA parsing)
- `flow::` - Flow sensor driver (hall-effect pulse, speed calculation)
- `display::` - TFT display driver (ST7789 320×240, direct hardware writes, nav + debug screen rendering)
- `menu::` - Hierarchical menu system (JSON-configurable, button-driven, on display device)
- `logging::` - Data logging system (LittleFS-based)
- `storage::` - Calibration persistence (JSON files in LittleFS)
- `hdg_cal::` - Fourier heading calibration (load hdg_fourier.json, apply Fourier-series correction)
- `nvs_nav::` - Nav device runtime state persistence (ESP32 NVS via Preferences)
- `nvs_disp::` - Display device settings persistence (ESP32 NVS via Preferences)

### Directory Structure

```
src/
├── nav_main.cpp               # Nav device entry point (sensors, AHRS, GPS, DR, serial link)
├── display_main.cpp           # Display device entry point (TFT rendering, buttons)
├── board_pins.h, config.h     # Hardware pin definitions and config constants
├── drivers/                   # Low-level sensor/peripheral drivers
│   ├── lsm6ds33.cpp/h         # Accel/gyro driver
│   ├── lis3mdl.cpp/h          # Magnetometer driver
│   ├── display.cpp/h          # TFT display driver (ST7789 320×240, direct hardware writes)
│   ├── flow_sensor.cpp/h      # Flowmeter driver (hall-effect pulse)
│   └── gps.cpp/h              # GPS driver (Adafruit Ultimate GPS, NMEA)
├── sensors/                   # High-level sensor interfaces
│   ├── imu.cpp/h              # Unified IMU API (init, read, calibrate)
│   └── calib.cpp/h            # Calibration structures (Calib3, MagCalib)
├── math/                      # AHRS and math utilities
│   ├── mahony.cpp/h           # Mahony filter (gyro + accel + mag fusion)
│   └── orientation.cpp/h      # Quaternion/Euler conversions, heading calculation
├── nav/                       # Navigation logic
│   ├── nav_model.cpp/h        # Position estimation (dead reckoning + GPS truth + home)
│   ├── ui_controller.cpp/h    # Display updates and user interface
│   └── state.h                # State machine definitions (BOOT/CAL/NAV/ERROR)
├── menu/                      # Menu system (display device only)
│   ├── menu.cpp/h             # Hierarchical menu: state machine, rendering, JSON load, actions
├── util/                      # Utilities
│   ├── logging.cpp/h          # Data logging to LittleFS
│   ├── storage.cpp/h          # Calibration save/load (JSON)
│   ├── nvs_state.cpp/h        # Runtime state persistence (ESP32 NVS: toggles, position)
│   ├── hdg_cal.cpp/h          # Fourier heading calibration (load /hdg_fourier.json, apply Fourier-series correction)
│   └── speed_cal.cpp/h        # Speed cal k-factor history (LittleFS /speed_cal.json, rolling 6-run average)
└── types/
    └── types.h                # Core data types (Vec3i16, Vec3f, Calib3, MagCalib, etc.)
lib/
└── dpvlink/
    └── dpvlink.h/cpp          # Inter-device packet format (NavPacket, DebugPacket, DisplayCmd, JSON wire format)
data/
└── menu.json                  # Menu definition (uploaded to LittleFS on display device)
```

### Data Flow Pipeline (Nav Device)

1. **Raw I2C reads** ([sensors/imu.cpp](src/sensors/imu.cpp)): I2C transaction → `Vec3i16` (raw counts)
2. **Unit conversion** ([sensors/imu.h](src/sensors/imu.h)): Raw counts → physical units (g, rad/s, µT)
3. **Calibration** ([sensors/calib.h](src/sensors/calib.h)):
   - Accel/Gyro: `Calib3` applies bias subtraction + scale factor
   - Magnetometer: `MagCalib` applies hard-iron offset + soft-iron matrix (3×3)
4. **AHRS fusion** ([math/mahony.h](src/math/mahony.h)): Normalized sensor vectors → `Quaternion` (orientation)
5. **Euler extraction** ([math/orientation.h](src/math/orientation.h)): Quaternion → Roll/Pitch/Yaw (rad)
6. **Heading calculation**: Yaw → `headingRawDeg` (0-360°) → `headingDeg` (Fourier hdg_cal correction applied if `/hdg_fourier.json` loaded, else same as raw). Both values are sent in NavPacket (`heading_deg` = corrected, `heading_raw_deg` = pre-correction).
7. **Speed selection**: GPS speed is filtered through a two-stage gate before use. First, a SOG (Speed Over Ground) deadband rejects speeds below 0.5 kn as position jitter noise and trusts speeds above 2.0 kn unconditionally. Speeds in the middle zone must also pass a COG (Course Over Ground) coherence check — an EMA of sin/cos(COG) whose resultant length measures heading consistency (0=random, 1=steady). If GPS speed fails either check, flowmeter speed is used instead. See `GPS_SOG_NOISE_FLOOR_KN`, `GPS_SOG_TRUST_FLOOR_KN`, and `GPS_COG_COHERENCE_THRESH` in [config.h](src/config.h).
8. **Position estimation** ([nav/nav_model.h](src/nav/nav_model.h)): Dead-reckoning integration (speed × heading × dt) with optional GPS truth override
9. **Serial link**: `NavPacket` sent to display device at 10 Hz via JSON over Serial1. Optional `DebugPacket` at 5 Hz when `ENABLE_DEBUG_PACKET` is set.

### Position Estimation System

The position model ([nav/nav_model.cpp](src/nav/nav_model.cpp)) uses flat-earth local XY coordinates in meters, suitable for the <1 mile operating range of a dive:

- **Baseline position**: Default 42°N, 122°W (configurable in [config.h](src/config.h)); used for local XY ↔ lat/lon conversion
- **Dead reckoning** (100 Hz): `x += speed × sin(heading) × dt`, `y += speed × cos(heading) × dt`
- **GPS truth**: When GPS has a fresh fix and `DEFAULT_USE_GPS_POSITION` is true, position snaps to GPS lat/lon (converted to local XY)
- **Home waypoint**: BTN1 press sends SET_HOME → snapshots current (x,y) as home; display shows distance and bearing back to home
- **No home**: Display shows X/Y meters from starting point

Key functions in `nav::`:
- `init(baselineLat, baselineLon)` — set coordinate origin
- `updateDR(heading_deg, speed_ms, dt)` — dead-reckoning step (call every loop)
- `updateGPS(lat, lon)` — snap position to GPS truth (when available)
- `setPosition(x_m, y_m)` — directly set position (used on boot to restore NVS-saved position)
- `setHome()` / `clearHome()` — manage home waypoint
- `distanceToHome_m()` / `bearingToHome_deg()` — range/bearing to home

### Sensor Configuration Pattern

Sensors are configured at startup via structs passed to `imu::init()`. See [nav_main.cpp](src/nav_main.cpp):

```cpp
imu::ImuConfig imuConfig{
  .accel_g_fullscale = 16.0f,
  .gyro_dps_fullscale = 2000.0f,
  .mag_uT_fullscale = 4.0f,
  .sample_hz = 100
};

imu::AxisMap imuAxisMap{
  .x_axis = +1,  // +1=+X, -1=-X, +2=+Y, -2=-Y, +3=+Z, -3=-Z
  .y_axis = +2,
  .z_axis = +3
};
```

**`AxisMap`** handles physical board orientation by mapping logical axes (X/Y/Z in navigation frame) to physical sensor axes with sign flips. Critical for correct heading.

### Calibration System

The system supports automatic calibration with persistence to LittleFS (flash storage):

**Magnetometer (two-stage, menu-triggered, bin-aware):**
- **Baseline** (CAL > Baseline): Device off DPV, full sphere coverage. Collects samples to `/mag_baseline_samples.csv`. Run `python tools/mag_calibration.py mag_baseline_samples.csv`, upload `mag_base.json`.
- **Mounted** (CAL > Mounted): Device installed on DPV, horizontal rotations. Collects samples to `/mag_mounted_samples.csv`. Run `python tools/mag_calibration.py mag_baseline_samples.csv mag_mounted_samples.csv`, upload `mag_mount.json`.
- Both stages show a live heading×elevation bin-coverage grid on the display. Auto-completes when required bins are green.
- On boot: tries `mag_base.json` + `mag_mount.json` chain → falls back to legacy `mag_cal.json` → falls back to 90s blocking sweep.

**Gyroscope:** On first boot (no `gyro_cal.json`): 10s stationary bias sampling. Subsequent boots load from LittleFS.

**Accelerometer:** On first boot (no `accel_cal.json`): 6-point orientation sequence (X+/X-/Y+/Y-/Z+/Z- facing down, 2.5s each). Subsequent boots load from LittleFS.

**Fourier Heading Calibration (CAL > Hdg cal):**
- Optional, run after completing baseline + mounted mag cal. Run in a magnetically clean environment (living room, not garage).
- On-device: display guides user through 12 headings (0°, 30°, 60°, … 330°) at 30° intervals. User aligns DPV to each target and presses BTN2. Nav device records `(target, indicated)` pairs and saves to `/hdg_samples.csv`.
- Offline: download `/hdg_samples.csv` via web interface, run `python tools/fourier_fit.py hdg_samples.csv`, upload the resulting `hdg_fourier.json` to nav device LittleFS.
- The JSON contains `n` (number of harmonics, 1–4) and `c` (Fourier coefficients array). Up to 4 harmonics (9 coefficients: DC + 4×cos/sin pairs).
- At runtime, `hdg_cal::apply()` evaluates the Fourier series to correct `headingRawDeg` → `headingDeg`. Loaded silently on boot from `/hdg_fourier.json` (skipped if absent).
- Display shows `heading_raw_deg` from NavPacket during cal prompts (pre-correction absolute readings, not residual corrections).

**Speed Calibration (CAL > Speed cal):**
- Swim a known distance (150–500 ft) at cruise speed; automatic run detection via flow threshold + heading-deviation stop.
- k-factor computed from total pulse count + true distance; stored as rolling 6-run average in `/speed_cal.json`.
- RESET+ACCEPT clears history (use after DPV service); ACCEPT adds to average; REJECT discards.

To force recalibration, delete the corresponding JSON file from LittleFS. See [docs/calibration-guide.md](docs/calibration-guide.md) and [docs/mag-calibration-workflow.md](docs/mag-calibration-workflow.md).

### NVS State Persistence

Runtime toggle states and estimated position are persisted to ESP32 NVS (Non-Volatile Storage) using the Arduino `Preferences` library. This is separate from LittleFS calibration data.

**What is persisted:**

| Variable | NVS namespace | Key | Saved when |
|----------|--------------|-----|-----------|
| GPS position enabled | `nav_state` | `gps_pos` | On toggle |
| GPS speed enabled | `nav_state` | `gps_spd` | On toggle |
| WiFi enabled | `nav_state` | `wifi` | On toggle |
| Dive mode | `nav_state` | `dive_mode` | On toggle |
| Log level (0/1/2) | `nav_state` | `log_level` | On cycle |
| Estimated X position | `nav_state` | `pos_x` | Every `NVS_POS_SAVE_INTERVAL_MS` (30 s) |
| Estimated Y position | `nav_state` | `pos_y` | Every `NVS_POS_SAVE_INTERVAL_MS` (30 s) |
| Display mode (nav/debug) | `disp_state` | `debug_mode` | On toggle |
| Show ETA vs speed | `disp_state` | `show_eta` | On toggle |
| Imperial units | `disp_state` | `imperial` | On toggle |
| True heading | `disp_state` | `true_heading` | On toggle |

**On boot:** NVS state is loaded at the end of `setup()` in [nav_main.cpp](src/nav_main.cpp) (after WiFi/web server init). If NVS is empty (first boot), factory defaults are used. The restored position is applied via `nav::setPosition()`. Dive mode re-disables GPS and WiFi if it was active.

**API** ([src/util/nvs_state.h](src/util/nvs_state.h)):
```cpp
nvs_nav::State s = nvs_nav::load();   // load (returns defaults if uninitialized)
nvs_nav::save(s);                      // save full state (built from current globals via currentNavNvsState())
nvs_nav::savePosition(x_m, y_m);      // save only position fields (periodic)

nvs_disp::State d = nvs_disp::load(); // load display settings
nvs_disp::save(d);                     // save display settings
```

**Position save interval:** Configurable via `NVS_POS_SAVE_INTERVAL_MS` in [config.h](src/config.h) (default 30 s). Position is also included in any full `nvs_nav::save()` call triggered by a toggle, so it's always at least as fresh as the last toggle.

### I2C / Wire Usage

All sensor I2C operations use a shared global `TwoWire` reference (`gWire`). The LSM6DS33 supports two addresses (0x6A or 0x6B, depending on SA0 pin); code auto-detects in [imu.cpp:77](src/sensors/imu.cpp#L77).

I2C pins defined in [board_pins.h](src/board_pins.h): SDA=23, SCL=22.

### Status Enum Returns

Many functions return `imu::ImuStatus` (`Ok`, `NotInitialized`, `BusError`, `WhoAmIMismatch`). Always check return values; I2C errors fail silently otherwise. See [nav_main.cpp:75-79](src/nav_main.cpp#L75-L79) for error handling pattern.

### Mahony Filter Integration

See [nav_main.cpp](src/nav_main.cpp):

```cpp
MahonyState ahrs;
MahonyParams mahonyParams{ .kp = 1.0f, .ki = 0.002f, .useMag = true };
```

- **`kp`**: Proportional gain (0.5–2.0 range). Higher = faster correction from accel/mag, lower = smoother gyro-dominated response
- **`ki`**: Integral gain (0.0–0.01 range). Builds gyro bias estimate over time. Too high causes windup/oscillation
- **`useMag`**: Enable magnetometer fusion (set to `true` once mag is calibrated)

**CRITICAL:** All three sensor inputs must be in the **same right-handed NED body frame**. The mag reading requires Y-axis negation before passing to the filter — see "Magnetometer Coordinate Frame" section above. Update in main loop:

```cpp
imu::Vec3f magNED = { mag.x, -mag.y, mag.z };  // Fix mag to NED frame
mahonyUpdate(ahrs, mahonyParams, gyro, accel, magNED, dt);
```

## Menu System

The display device includes a hierarchical menu system ([src/menu/menu.h](src/menu/menu.h), [src/menu/menu.cpp](src/menu/menu.cpp)) for runtime configuration and actions.

### Button Mapping
- **BTN1 short press**: Open menu (when closed) or cycle to next item (when open)
- **BTN2 short press**: Select highlighted item (enter submenu, execute action, or go back)
- **BTN1 + BTN2 held 2s**: Reset display device (sends `DisplayCmd::RESET`)
- **15-second idle timeout**: Menu auto-closes

### Menu Structure
```
MENU (root)
├── OFF      — power off nav device
├── NAV:     Outbound, Home, Mark, Op Mode
├── CAL:     Baseline, Mounted, Hdg cal, Speed cal
├── INPUT:   GPS Pos, GPS Spd, WiFi, Logging
└── DISPLAY: Mode, Spd/ETA, Units, Heading
```

Each submenu has an auto-generated ".." (back) item. Toggle items (GPS Pos, Units, Mode, Op Mode, etc.) show current state inline and stay open after toggle.

**Speed cal** uses a multi-phase UI that takes over the display outside the menu system:
1. Menu closes → display enters distance-selection mode (`SpeedCalPhase::DIST_SELECT` in display_main.cpp)
2. BTN1 cycles distance (150–500 ft in 50 ft steps); BTN2 confirms and sends `START_SPEED_CAL` to nav device
3. Nav device state machine: WAITING (cal_mode=2) → RUNNING (cal_mode=3) → RESULT (cal_mode=4)
4. Display advances phase by watching cal_mode in incoming NavPackets (forward-only to avoid stale-packet race)
5. RESULT screen: BTN1 cycles RESET+ACCEPT / ACCEPT / REJECT; BTN2 sends the chosen `DisplayCmd`

**Op Mode (Dive/Surface):** Toggles operational mode. Surface mode (default at boot) keeps GPS and WiFi active. Dive mode disables both GPS processing and WiFi radio — suitable for underwater use where neither is available. Toggling back to surface re-initializes WiFi and GPS.

### Display Layout When Menu Is Open (320×240 TFT)
- **y=0–23**: Status bar (unchanged, live-updating)
- **y=24–119**: Nav data (2×2 grid: BRG/RNG/HDG/SPD, live-updating)
- **y=120–239**: Menu area (separator line, title, up to visible items with scroll)

### Menu Definition
Menu structure is loaded from `/menu.json` on LittleFS at boot. If the file is missing, a hardcoded default is used. The JSON maps action IDs to `menu::Action` enum values. To customize the menu, edit [data/menu.json](data/menu.json) and upload with `pio run -e display -t uploadfs`.

### Adding New Menu Actions
1. Add a new `menu::Action` enum value in [src/menu/menu.h](src/menu/menu.h)
2. Add a corresponding `DisplayCmd` value in [lib/dpvlink/dpvlink.h](lib/dpvlink/dpvlink.h) (for nav-device actions)
3. Wire the action in `executeAction()` in [src/menu/menu.cpp](src/menu/menu.cpp)
4. Handle the command in `handleDisplayCmd()` in [src/nav_main.cpp](src/nav_main.cpp)
5. Add the item to the hardcoded default menu and to [data/menu.json](data/menu.json)

## Display Rendering Architecture (ST7789 Direct Writes)

The display driver uses a 320×240 ST7789 TFT with direct hardware writes via the Adafruit ST7789 library. There is no offscreen framebuffer; drawing calls go directly to the display.

**Rendering approach:** Incremental updates at ~10 Hz — only changed values are erased and redrawn to avoid flicker. Full-screen modes (cal prompts, cal summaries, speed cal phases) redraw the full screen on each state transition.

**Key display functions** ([src/drivers/display.cpp](src/drivers/display.cpp)):
- `showNav(NavPacket)` — navigation screen: status bar + 2×2 grid (BRG/RNG/HDG/SPD), incremental updates
- `showDebug(DebugPacket)` — debug screen: raw sensor values (mag/accel/gyro XYZ, heading, pitch/roll)
- `showHdgFourierCalPrompt(int step, int total, float targetDeg, float indicatedDeg)` — heading cal step prompt with live heading readout
- `showHdgFourierCalDone(int nPoints)` — done screen with instructions to run offline fit and upload JSON
- `showMagCalProgress(...)` — bin-coverage grid for magnetometer calibration (Baseline/Mounted)

## Critical Conventions

### Magnetometer Coordinate Frame (IMPORTANT — Read Before Touching Axis Maps or Mahony)

The LSM6DS33 (accel/gyro) and LIS3MDL (magnetometer) have **different physical Y-axis directions** on this board. The axis maps in [nav_main.cpp](src/nav_main.cpp) correct for this:

```cpp
imu::AxisMap accelGyroMap{ .x_axis = +1, .y_axis = +2, .z_axis = +3 };  // NED reference
imu::AxisMap magMap{ .x_axis = +1, .y_axis = -2, .z_axis = +3 };        // LIS3MDL Y flipped
```

**The subtle trap:** After axis mapping + calibration, the mag output is in a **left-handed** frame for Y (positive Y = Left, not Right). This is why `atan2(mag_y, mag_x)` directly gives correct heading in `debug_axes` and diagnostics — the standard NED formula `atan2(-mag_y, mag_x)` is NOT needed because mag Y is already sign-flipped.

**But the Mahony AHRS filter requires all sensors in the same right-handed NED frame.** If mag and accel Y axes disagree, the filter's cross-product correction drives heading to `360° - true_heading` instead of `true_heading`, and rotation sense reverses. This manifests as a **stable but wrong heading** (not drift) with a consistent offset that varies by orientation.

**The fix** (in `nav_main.cpp` loop): mag.y is negated before passing to `mahonyUpdate()`:

```cpp
imu::Vec3f magNED = { mag.x, -mag.y, mag.z };
mahonyUpdate(ahrs, mahonyParams, gyro, accel, magNED, dt);
```

**Rules to prevent re-breaking this:**
1. **Never change `magMap` without also updating the `magNED` negation** — they are a matched pair
2. **Never pass `mag` directly to `mahonyUpdate()`** — always use `magNED`
3. **If you change the axis map, all mag calibration data must be re-collected** — the soft-iron matrix is frame-dependent
4. **Diagnostics and `debug_axes` use `mag` (not `magNED`)** — they use `atan2(my, mx)` which works in the left-handed frame
5. **The LIS3MDL BDU (Block Data Update) must be enabled** (CTRL_REG5 = 0x40) or mag readings will byte-tear and fluctuate wildly

### Struct-Based Configuration
All configuration uses struct initialization with named fields (not function params). Keeps init calls readable and decouples config from init logic.

### Raw vs. Converted Data
- `readAccelRaw()` / `readGyroRaw()` / `readMagRaw()` → `Vec3i16` (raw sensor counts)
- `readAccel_g()` / `readGyro_rad_s()` / `readMag_uT()` → `Vec3f` (physical units)
- `readAccel_g_raw_cal()` / `readGyro_rad_s_raw_cal()` / `readMag_raw_cal()` → Returns both raw and calibrated values

When adding new sensor reads, follow this pattern.

### Quaternion Gimbal Lock
Euler angle extraction can experience gimbal lock at pitch = ±90°. The quaternion representation in `MahonyState.q` is singularity-free.

## Common Development Workflows

### Adding a New Sensor Driver
1. Create header in `src/drivers/` with init + read functions
2. Add sensor address/register constants at top of .cpp
3. Use `gWire` (shared TwoWire) for I2C; return `ImuStatus`
4. Call init in `imu::init()` or separately in `nav_main.cpp` setup
5. Read raw data in loop, apply calibration before passing to AHRS

### Tweaking IMU Configuration
Modify `ImuConfig` or `AxisMap` in [nav_main.cpp](src/nav_main.cpp) before `imu::init()` call. Axis mapping is critical for heading accuracy. Test by rotating device and verifying heading changes match physical rotation direction.

### Debugging Sensor Communication
- Check WHO_AM_I register (LSM6DS33 = 0x69, LIS3MDL = 0x3D)
- Verify I2C address and pins in [board_pins.h](src/board_pins.h)
- Use `Serial.print()` to log raw reads and confirm data flow
- See [docs/calibration-guide.md](docs/calibration-guide.md) for sensor setup details

### Modifying Calibration Parameters
Calibration timing is configured at the call sites in [nav_main.cpp](src/nav_main.cpp):
- **Boot mag cal (first-run fallback)**: `imu::calibrateMagnetometer(magCal, 90000)` — 90 sec blocking sweep (only runs if both mag_base.json and mag_cal.json are absent)
- **Gyro cal (boot)**: `imu::calibrateGyroscope(gyroCal, 10000)` — 10 sec at rest
- **Accel cal (boot)**: `imu::calibrateAccelerometer(accelCal, 2500)` — 2.5 sec per orientation (15 sec total)
- **Baseline/Mounted bin-aware cal**: sample collection runs until required bins are green (no fixed time)

To force recalibration, delete the JSON files from LittleFS and reboot. Preferred workflow: CAL > Baseline (off DPV) → offline fit → upload `mag_base.json`; then CAL > Mounted (on DPV) → offline fit → upload `mag_mount.json`.

## Key Files Reference

- [src/nav_main.cpp](src/nav_main.cpp) — Nav device entry point: sensor init, AHRS, GPS, flow, position estimation, serial link
- [src/display_main.cpp](src/display_main.cpp) — Display device entry point: TFT rendering, buttons, menu integration, serial link receive
- [src/menu/menu.h](src/menu/menu.h) — Menu system API: data structures, state machine, display settings
- [src/menu/menu.cpp](src/menu/menu.cpp) — Menu implementation: rendering, navigation, JSON loading, action dispatch
- [data/menu.json](data/menu.json) — Menu definition file (uploaded to display device LittleFS)
- [src/nav/nav_model.h](src/nav/nav_model.h) — Position estimation API (dead reckoning + GPS truth + home waypoint)
- [src/nav/nav_model.cpp](src/nav/nav_model.cpp) — Position estimation implementation (flat-earth local XY)
- [src/sensors/imu.cpp](src/sensors/imu.cpp) — Unified IMU driver (LSM6DS33 + LIS3MDL), I2C read/write, calibration routines
- [src/sensors/imu.h](src/sensors/imu.h) — Public IMU API (ImuConfig, ImuStatus, read functions)
- [src/drivers/gps.cpp](src/drivers/gps.cpp) — GPS driver (Adafruit Ultimate GPS, NMEA parsing)
- [src/drivers/flow_sensor.cpp](src/drivers/flow_sensor.cpp) — Flow sensor driver (speed from hall-effect pulses)
- [src/drivers/display.cpp](src/drivers/display.cpp) — TFT display driver (ST7789 320×240; nav mode: status bar + 2×2 grid; debug mode: sensor data; hdg cal prompts + summary)
- [src/math/mahony.h](src/math/mahony.h) — AHRS filter (quaternion update from gyro/accel/mag)
- [src/math/orientation.h](src/math/orientation.h) — Quaternion ↔ Euler conversions, heading calculation
- [src/types/types.h](src/types/types.h) — Core data types (Vec3i16, Vec3f, Calib3, MagCalib, ImuConfig, AxisMap)
- [src/config.h](src/config.h) — Configuration constants (flow sensor, GPS, position baseline, display mode/units, debug packet)
- [lib/dpvlink/dpvlink.h](lib/dpvlink/dpvlink.h) — Inter-device packet format (NavPacket, DebugPacket, DisplayCmd, PacketType discriminator)
- [src/sensors/calib.h](src/sensors/calib.h) — Calibration application functions
- [src/util/storage.h](src/util/storage.h) — Calibration save/load (JSON to LittleFS)
- [src/util/nvs_state.h](src/util/nvs_state.h) — Runtime state persistence (ESP32 NVS): `nvs_nav::` (toggle states, position) and `nvs_disp::` (display settings)
- [src/util/hdg_cal.h](src/util/hdg_cal.h) — Fourier heading calibration: `load()` (reads `/hdg_fourier.json`), `apply(headingDeg, cal)` (evaluates Fourier series)
- [src/util/logging.h](src/util/logging.h) — Data logging system (CSV-like format)
- [src/util/speed_cal.h](src/util/speed_cal.h) — Speed cal k-factor history: `load()`, `save()`, `addMeasurement()`, `averageK()`, `reset()`
- [docs/overview.md](docs/overview.md) — Project overview, architecture, feature summary
- [docs/user-guide.md](docs/user-guide.md) — User-facing guide: boot, display, buttons, dive workflow
- [docs/calibration-guide.md](docs/calibration-guide.md) — Sensor calibration (mag, gyro, accel) + persistence
- [docs/mag-calibration-workflow.md](docs/mag-calibration-workflow.md) — Advanced soft-iron magnetometer calibration (ellipsoid fitting)
- [docs/ahrs-orientation.md](docs/ahrs-orientation.md) — AHRS Mahony filter, Euler angles, heading
- [docs/data-logging-guide.md](docs/data-logging-guide.md) — Data logging system (CSV to LittleFS)
