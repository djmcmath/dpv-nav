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

**Know which board you are flashing.** Both environments share the one `data/`
directory, but only one of them is safe to overwrite:

```bash
pio run -e display -t uploadfs   # SAFE. The display board's FS holds only
                                 # /menu.json, and nothing on that board ever
                                 # writes to LittleFS. Nothing is lost.

pio run -e nav -t uploadfs       # DESTRUCTIVE. Erases dive logs, mag_base.json,
                                 # mag_mount.json, hdg_fourier.json, motor_cal.json,
                                 # speed_cal.json, cal_targets.json and the cal
                                 # archives. Pull anything you want off
                                 # tern.local first.
```

`data/menu.json` is read by the **display** board (`loadFromJSON()` in
[menu.cpp](src/menu/menu.cpp)). The web UI's file upload lives on the **nav**
board — `net/` is in `[env:nav]` only and the display env has no network stack
at all — so there is no way to push menu.json over WiFi. It needs
`-e display -t uploadfs` with that board on USB.

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
│   ├── speed_cal.cpp/h        # Speed cal k-factor history (LittleFS /speed_cal.json, rolling 6-run average)
│   ├── motor_cal.cpp/h        # Motor-on heading offset (load /motor_cal.json, single fixed correction)
│   └── mag_cal_orient.cpp/h   # Algebraic per-sample orientation for gap-fill cal — VERBATIM PORT of the server's callib/coverage.py (read its header first: the axis convention is a 180° trap)
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
6. **Heading calculation**: Yaw → `headingRawDeg` (0-360°) → `headingDeg` (Fourier hdg_cal correction applied if `/hdg_fourier.json` loaded, else same as raw, then motor-on heading offset added if `/motor_cal.json` loaded). Both values are sent in NavPacket (`heading_deg` = corrected, `heading_raw_deg` = pre-correction).
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

**Magnetometer (two-stage, menu-triggered, cloud-fit):**
- **Baseline** (CAL > Baseline): Device off DPV, full sphere coverage. No live grid (retired 2026-07-25 after six failed attempts, see [docs/baseline-cal-two-pass.md](docs/baseline-cal-two-pass.md)) — shows axis-range bars + live fit stats instead; diver presses BTN2 when coverage looks good enough (`FINISH_BASELINE_COLLECTION`). Collects samples to `/mag_baseline_samples.csv`.
- **Mounted** (CAL > Mounted): Device installed on DPV, horizontal rotations. Still shows the original live heading×elevation bin-coverage grid (narrower ±30° range sidesteps the pole problem Baseline's grid had) — auto-completes when required bins are green. Collects samples to `/mag_mounted_samples.csv`.
- **Both stages upload and fit in the cloud automatically** if the unit has WiFi at completion (`cloud::runCalibrationUpload`, same round trip described under Fourier Heading Calibration below): result is staged, not written over the active file, and the display shows an accept/reject screen with quality band + RMS. Accept copies staged → active and hot-reloads immediately.
- **No WiFi at completion:** display shows an offline notice; the CSV is still saved to LittleFS, so the old manual fallback still works — run `python tools/mag_calibration.py --mode baseline mag_baseline_samples.csv` (or `--mode mounted --base mag_base.json mag_mounted_samples.csv`), then upload the result to LittleFS root.
- **Filename matters for the manual fallback only** (the cloud path always writes the right name): the firmware only ever reads `/mag_base.json` and `/mag_mount.json` from the root (see [src/util/storage.h](src/util/storage.h)). Do **not** pass `--output` with a different name (e.g. `mag_mounted.json`) unless you rename it to `/mag_mount.json` on upload — otherwise the device silently ignores it and keeps running the previous (stale) calibration. The script warns if the output name won't be read.
- On boot: tries `mag_base.json` + `mag_mount.json` chain → falls back to legacy `mag_cal.json` → falls back to 90s blocking sweep.
- **Filling coverage gaps:** see "Guided Gap-Fill" below (device-side, Baseline only) and the Fourier Heading Calibration entry below (website form, hdg only) — different mechanisms because a 2-D orientation grid and a 1-D ring of headings need different gap detection.

**Gyroscope:** On first boot (no `gyro_cal.json`): 10s stationary bias sampling. Subsequent boots load from LittleFS.

**Accelerometer:** On first boot (no `accel_cal.json`): 6-point orientation sequence (X+/X-/Y+/Y-/Z+/Z- facing down, 2.5s each). Subsequent boots load from LittleFS.

**Fourier Heading Calibration (CAL > Hdg cal):**
- Optional, run after completing baseline + mounted mag cal. Run in a magnetically clean environment (living room, not garage).
- On-device: display guides user through 12 headings (0°, 30°, 60°, … 330°) at 30° intervals. User aligns DPV to each target and presses BTN2. Nav device records `(target, indicated)` pairs and saves to `/hdg_samples.csv`.
- **Same cloud round trip as baseline/mounted mag cal**, triggered from the 12th point's completion (`FINALIZE_HDG_CAL` in nav_main.cpp): if WiFi is connected, the CSV uploads automatically, the server fits it (auto-selecting 1–4 harmonics) and returns a quality band + RMS in **degrees** (`showCloudCalResult()` labels the shared field "°" instead of "%" for this mode), staged to `/hdg_fourier_pending.json`. Same accept/reject screen as mag cal; accept copies staged → active `hdg_fourier.json` and hot-reloads. **No WiFi:** offline notice shown, `/hdg_samples.csv` still saved — fall back to `python tools/fourier_fit.py hdg_samples.csv`, upload the resulting `hdg_fourier.json` manually.
- The JSON contains `n` (number of harmonics, 1–4) and `c` (Fourier coefficients array). Up to 4 harmonics (9 coefficients: DC + 4×cos/sin pairs).
- **Filling sector gaps:** the server also checks `sector_adequacy` on every fit — any gap > 45° between the collected **indicated** headings gets flagged (e.g. only 4 of 12 points falling in a 330°→060° span). Unlike Baseline's `CAL > Fill gaps`, there is no on-device flow for this at all — no firmware change, no menu item. The Dive Map website's Calibration History shows a "thin sector(s)" badge on the affected row; expanding it shows an editable-rows form (seeded from the suggested bearings) where the diver aims the DPV at each bearing, reads the indicated heading off the display, and types the `(actual, indicated)` pair in by hand. Submitting posts to `POST /calibrations/{id}/hdg-manual-samples`, which combines it with the original upload and re-fits, producing a new pending row to Accept/Reject on the website.
- **Two frames, and the 2026-08-27 fix.** Gap *detection* is in the indicated domain (that's what the Fourier series is a function of, so that's where it's unconstrained). Gap *suggestions* must be actual bearings, since that's all a diver can steer. `sector_adequacy` used to emit the indicated-frame division points and the form seeded them into the `actual` column — so the diver was told to steer a bearing that landed right next to a point already collected, adding no coverage and degrading the fit. It now interpolates between the two observed points bracketing the gap (never the fitted curve — inside the gap that's the least trustworthy thing available), rounds to `SUGGESTION_ROUNDING_DEG` (5°, the finest a diver holds on a small compass), and drops anything within `MIN_SUGGESTION_SEPARATION_DEG` (10°) of an already-collected bearing. `from_deg`/`to_deg`/`span_deg` stay indicated-frame; `suggested_headings`/`from_actual_deg`/`to_actual_deg` are actual-frame. Every pre-existing test used `actual == indicated` data, which is exactly why the bug was invisible.
- **Reading the indicated heading: `DISPLAY > Heading` → `RAW`.** The toggle cycles TRUE → MAG → RAW. RAW substitutes `heading_raw_deg` — which nav_main.cpp already sends as the pre-Fourier, pre-motor-offset *magnetic* heading, i.e. exactly what `CAPTURE_HDG_POINT` records — so `hdg_fourier.json` and `motor_cal.json` stay installed. This replaced a delete/zero/reload/restore ritual that had no backup path for `motor_cal.json`. RAW is a bench mode: it renders in yellow with an `R` suffix, and `nvs_disp::load()` deliberately reverts it to MAG on boot so a forgotten toggle can't leave the unit navigating uncorrected.
- **Accepting on the website does not push to the device by itself.** A web-accepted result (from this gap-fill form, or a merged Baseline gap-fill) only updates the database until the diver explicitly pulls it: on `tern.local`, under "Calibration Cloud Sync," press "Check for updates." A calibration accepted on-device via the normal accept/reject screen needs no such step.
- At runtime, `hdg_cal::apply()` evaluates the Fourier series to correct `headingRawDeg` → `headingDeg`. Loaded silently on boot from `/hdg_fourier.json` (skipped if absent).
- Display shows `heading_raw_deg` from NavPacket during cal prompts (pre-correction absolute readings, not residual corrections).

**Speed Calibration (CAL > Speed cal):**
- Swim a known distance (150–500 ft) at cruise speed; automatic run detection via flow threshold + heading-deviation stop.
- k-factor computed from total pulse count + true distance; stored as rolling 6-run average in `/speed_cal.json`.
- RESET+ACCEPT clears history (use after DPV service); ACCEPT adds to average; REJECT discards.

**Guided Gap-Fill (`CAL > Fill gaps`):**
- A *second* baseline pass that patches only the orientation cells the server flagged as thin or empty on the installed baseline cal. Uploads as a `baseline` collection and is combined with the existing one from the website's merge picker.
- Two hard preconditions, both refused with an on-screen message rather than degraded: a baseline cal must be installed, and a target map must have been synced. Sync happens via `tern.local`'s "Check for updates" button (not a device menu item — the on-device refusal text used to wrongly say `CAL > Check for updates`, fixed) — every click re-syncs the target map, cached at `/cal_targets.json`, in addition to whatever cal files it installs (`cal_sync.cpp`'s `refreshTargets()`, unconditional on both branches of `checkForUpdates()`). A sync failure (e.g. the accepted baseline predates coverage grading) now shows up in the result text on `tern.local` itself instead of only a Serial log line — this used to fail silently and was the actual cause of "Fill gaps" refusing with no obvious explanation.
- **Unlike baseline cal, this one gets a live grid** — because a good cal already exists, so orientation is computed algebraically per sample by [mag_cal_orient.h](src/util/mag_cal_orient.h) with no Mahony filter in the path. That is what separates it from the six failed attempts at live orientation feedback documented in [docs/baseline-cal-two-pass.md](docs/baseline-cal-two-pass.md) and the SPIKE block in [imu.cpp](src/sensors/imu.cpp).
- Which cells are targets is decided **server-side and never recomputed on the device** — the thin/empty thresholds are still being tuned, and keeping the judgement on the server means retuning them costs a sync, not a reflash.
- Per-cell acceptance caps (`MAG_CAL_GAPFILL_TARGET_CAP` / `_UNTARGETED_CAP` in [config.h](src/config.h)) keep gap-fill from creating the next over-weighted bin while closing an empty one.
- Auto-completes when every target is satisfied; BTN2 still finishes early, since a targeted cell can be physically unreachable.
- Host-side tests in [tools/README.md](tools/README.md) prove the port matches the server and that a flagged cell reaches the device as the same cell — run them after touching either side.
- **Roll coverage (2026-08-26):** the elevation×heading grid is deliberately roll-invariant (that's what makes tilt compensation work), so a cell can read fully green while every sample came from the same roll about the tracked axis -- a real, previously invisible coverage gap, not a UI bug (root-caused from real hardware CSVs; a geometry simulation confirmed restricting collection to "upright only" leaves a permanent unreachable cap on the sensor's own local sphere). Each cell now also grades roll diversity across `MAG_CAL_ROLL_SECTORS` (4: upright/right-side/upside-down/left-side, reusing the accelerometer-cal vocabulary), upright weighted `MAG_CAL_UPRIGHT_ROLL_WEIGHT`x the others. `util/mag_cal_orient.h`'s `reconstructRoll`/`rollSector` are a verbatim port of `callib/coverage.py`'s `reconstruct_roll`/`_roll_sector`, same discipline and same `tools/orient_equivalence.py` cross-check as the pitch/heading math. `imu.cpp` accumulates a local `g_binRollCounts[60][4]` per session and applies per-roll-sector acceptance caps (`MAG_CAL_GAPFILL_TARGET_CAP_OTHER_ROLL` for the non-upright sectors); a roll sector the *server* already considers ok/over from a prior accepted upload (`cal_sync::loadRollTargets()`, from `GET /calibrations/targets`' optional `roll_grid`) counts as satisfied from session start too, so gap-fill doesn't re-ask for coverage a diver already has. `display.cpp`'s `showCalGrid` renders this as a **persistent** 4-triangle widget next to the orientation readout — not a mode swap with the main grid (explicitly rejected as confusing) — always reflecting whichever cell `current_bin` currently points at. See [docs/calibration-guide.md](docs/calibration-guide.md)'s "Why you sometimes have to hold the unit upside-down or on its side" and dive-map's [calibration-grid-conventions.md](../dive-map/docs/architecture/calibration-grid-conventions.md) for the full cross-system convention reference.

**Motor-On Heading Correction:**
- The Fourier heading cal (above) is collected with the motor off (bench procedure). A running motor adds a roughly constant magnetic bias to heading, independent of heading angle or motor speed.
- Run after Fourier cal: do a reciprocal-leg test on a known bearing with the motor running, or derive the offset automatically from a reciprocal-leg GPS run via `tools/correct_track.py`.
- Stored as a single value in `/motor_cal.json`: `{ "heading_offset_deg": -3.0 }`. Positive = compass reads low (add to get true heading); negative = compass reads high (subtract from indicated).
- Loaded by `motor_cal::load()` at boot and on "Reload Cal Files" from the web page; applied on top of the Fourier-corrected heading (`headingDeg += heading_offset_deg`). Absent/invalid file → offset defaults to `0.0` (no correction).

To force recalibration, delete the corresponding JSON file from LittleFS. See [docs/calibration-guide.md](docs/calibration-guide.md).

**Shipped:** the cloud round trip described above (baseline/mounted/hdg all fit
server-side, with accept/reject on-device) replaced the old "export CSV → run the
Python tool on a laptop → upload result" workflow as the default path. See
[docs/cloud-calibration-plan.md](docs/cloud-calibration-plan.md) and divemap's
[heading-cal-cloud-plan.md](../dive-map/docs/architecture/heading-cal-cloud-plan.md)
and [calibration-session-merge-plan.md](../dive-map/docs/architecture/calibration-session-merge-plan.md)
for the implementation history. The manual laptop workflow still exists as the
offline fallback when the unit has no WiFi at cal time.

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
- **45-second idle timeout**: Menu auto-closes; reopening within `MENU_RESUME_WINDOW_MS` (2 min) resumes at the same item rather than at the root

### Menu Structure
```
MENU (root)
├── Nav:     Select WP, Arrive WP, Mark, Op Mode
├── Cal:     Baseline, Fill gaps, Mounted, Hdg cal, Speed cal
├── Config:  GPS, WiFi, Log, Water, Link acct
├── Display: Mode, Spd/ETA, Units, Heading
├── OFF      — power off nav device (two presses; see below)
└── Close    — auto-generated, leaves the menu
```

Each submenu has an auto-generated ".." back item; the root gets "Close". Both
carry `Action::BACK` — do not go back to identifying them by label, the old
`strcmp(label, "..")` test is precisely why the root could not have its own
exit. Toggle items (GPS, Units, Mode, Op Mode, etc.) show current state inline
and stay open after toggle.

### Menu Safety Invariants (do not regress these)

Three defects here cost a real dive in August 2026: the diver could not find
`Config > Log` underwater and shut the unit down by hand to stop logging, and
separately powered it off while trying to select `Display`. The fixes are
load-bearing, not cosmetic.

1. **`OFF` is never index 0, and never adjacent-by-wraparound to a real action.**
   It used to be root item 0, which made it both the item the menu opened on and
   the item `Display` wrapped onto. It now sits second-to-last with the no-op
   `Close` behind it, so overshooting the end of the list is harmless.
2. **`POWER_OFF` takes two presses.** The first BTN2 arms it (`gPowerOffArmed`,
   item repaints as `OFF:SURE?`), the second fires. `next()`, `close()` and
   `open()` all disarm.
3. **BTN2 cannot act on a menu that BTN1 opened in the same pass.**
   `handleButtons()` snapshots `menu::isOpen()` before running the BTN1 branch.
   Without that snapshot, a two-button tap — the same gesture that wakes the
   unit — released both buttons inside one 50 ms debounce window and ran
   open-then-select back to back, firing root item 0 sight-unseen.
4. **A modal screen that claims a button pass must claim both buttons.**
   `handleModalButtons()` returns true when a full-screen mode owned the pass and
   `handleButtons()` then calls `consumePendingReleases()`. Modals only ever
   handle the one button they care about; the other one's release used to sit
   with `fired == false` and fire into the menu the instant the modal exited.

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
Menu structure is loaded from `/menu.json` on the **display** board's LittleFS at boot. If the file is missing, `loadDefaults()` is used — keep the two in sync, or the menu a given unit shows depends on whether its filesystem was ever flashed. The JSON maps action IDs to `menu::Action` enum values. To customize the menu, edit [data/menu.json](data/menu.json) and upload with `pio run -e display -t uploadfs`.

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
4. **Diagnostics and `debug_axes` use `mag` (not `magNED`)** — they use `atan2(my, mx)`, which works in the left-handed frame **at level**. That qualifier matters: see rule 6.
5. **The LIS3MDL BDU (Block Data Update) must be enabled** (CTRL_REG5 = 0x40) or mag readings will byte-tear and fluctuate wildly
6. **Anything that applies an accel-derived rotation to the mag vector must un-mirror mag Y first.** `atan2(my, mx)` being correct at level does *not* generalize — the mirror only cancels when no rotation is applied. `mag_cal_orient.cpp` / `callib/coverage.py` un-mirror internally and pair it with `atan2(-yh, xh)`; callers still pass the mirrored vector. This was gotten wrong from 2026-07-26 to 2026-08-26 and cost up to 180° of heading and 90° of elevation at any real tilt, while every test passed — because `axis_test` sampled only level headings, the nav logs hold ≤±5° of tilt, and the synthetic cross-checks built their samples from the same assumption. **A level-only measurement certifies nothing about this.**
7. **Validate orientation changes against `tools/fixtures/`** via `tools/frame_fixture_check.py` — 13 poses captured from real hardware at known attitudes, the only test here whose ground truth is not the code. Re-capture with the `axis_test` serial command.

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
- **Boot mag cal (brand-new device)**: if none of mag_base.json/mag_mount.json/mag_cal.json exist, mag runs with an identity (no-op) calibration and `BOOT_MAG_CAL_OK` stays unset — no automatic sweep. The diver runs CAL > Baseline (+ Mounted) from the menu when ready. (`imu::calibrateMagnetometer()` still exists but is no longer called at boot.)
- **Gyro cal (boot)**: `imu::calibrateGyroscope(gyroCal, 10000)` — 10 sec at rest
- **Accel cal (boot)**: `imu::calibrateAccelerometer(accelCal, 2500)` — 2.5 sec per orientation (15 sec total)
- **Baseline (rough-scan)**: axis-range-bar collection ends when the diver presses BTN2 (no fixed time, no bin grid)
- **Mounted bin-aware cal**: sample collection runs until required bins are green (no fixed time)

To force recalibration, delete the JSON files from LittleFS and reboot. Preferred workflow: CAL > Baseline (off DPV) → cloud-fit + accept on-device (WiFi) or offline fit + upload `mag_base.json` (no WiFi); then CAL > Mounted (on DPV), same either/or.

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
- [src/util/motor_cal.h](src/util/motor_cal.h) — Motor-on heading offset: `load()` (reads `/motor_cal.json`, single `heading_offset_deg` field)
- [docs/overview.md](docs/overview.md) — Project overview, architecture, feature summary
- [docs/user-guide.md](docs/user-guide.md) — User-facing guide: boot, display, buttons, dive workflow
- [docs/calibration-guide.md](docs/calibration-guide.md) — Sensor calibration (mag baseline + mounted two-stage workflow, gyro, accel, speed)
- [docs/ahrs-orientation.md](docs/ahrs-orientation.md) — AHRS Mahony filter, Euler angles, heading
- [docs/data-logging-guide.md](docs/data-logging-guide.md) — Data logging system (CSV to LittleFS)
