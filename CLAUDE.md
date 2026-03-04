# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

DPV-Nav is an inertial navigation system for underwater DPVs (Diver Propulsion Vehicles / scooters). It provides tilt-compensated heading using AHRS (Mahony filter), dead-reckoning position via flowmeter integration, and displays bearing/distance to home on a small OLED.

**Target Hardware:** ESP32 (FeatherESP32), LSM6DS33 (accel/gyro), LIS3MDL (magnetometer), GPS, flow sensor, OLED display
**Architecture:** Two-device system — nav device (sensors/AHRS/GPS/position) + display device (OLED/buttons), linked via Serial1 JSON packets at 10 Hz
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
- `display::` - OLED display driver (SSD1351, nav + debug screen rendering)
- `ui::` - Display output (console_update for OLED/serial)
- `logging::` - Data logging system (LittleFS-based)
- `storage::` - Calibration persistence (JSON files in LittleFS)

### Directory Structure

```
src/
├── nav_main.cpp               # Nav device entry point (sensors, AHRS, GPS, DR, serial link)
├── display_main.cpp           # Display device entry point (OLED rendering, buttons)
├── board_pins.h, config.h     # Hardware pin definitions and config constants
├── drivers/                   # Low-level sensor/peripheral drivers
│   ├── lsm6ds33.cpp/h         # Accel/gyro driver
│   ├── lis3mdl.cpp/h          # Magnetometer driver
│   ├── display.cpp/h          # OLED display driver (SSD1351)
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
├── util/                      # Utilities
│   ├── logging.cpp/h          # Data logging to LittleFS
│   └── storage.cpp/h          # Calibration save/load (JSON)
└── types/
    └── types.h                # Core data types (Vec3i16, Vec3f, Calib3, MagCalib, etc.)
lib/
└── dpvlink/
    └── dpvlink.h/cpp          # Inter-device packet format (NavPacket, DebugPacket, DisplayCmd, JSON wire format)
```

### Data Flow Pipeline (Nav Device)

1. **Raw I2C reads** ([sensors/imu.cpp](src/sensors/imu.cpp)): I2C transaction → `Vec3i16` (raw counts)
2. **Unit conversion** ([sensors/imu.h](src/sensors/imu.h)): Raw counts → physical units (g, rad/s, µT)
3. **Calibration** ([sensors/calib.h](src/sensors/calib.h)):
   - Accel/Gyro: `Calib3` applies bias subtraction + scale factor
   - Magnetometer: `MagCalib` applies hard-iron offset + soft-iron matrix (3×3)
4. **AHRS fusion** ([math/mahony.h](src/math/mahony.h)): Normalized sensor vectors → `Quaternion` (orientation)
5. **Euler extraction** ([math/orientation.h](src/math/orientation.h)): Quaternion → Roll/Pitch/Yaw (rad)
6. **Heading calculation**: Yaw → Heading (0-360°)
7. **Speed selection**: GPS speed (when fix fresh <3s) or flowmeter speed
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

**Magnetometer** ([nav_main.cpp:167-176](src/nav_main.cpp#L167-L176)):
- On first boot: Min/max sweep calibration (rotate device through all orientations for 30 sec)
- Saves hard-iron offset + soft-iron matrix to `/calib/mag_cal.json`
- On subsequent boots: Loads from LittleFS (skip calibration unless file missing)

**Gyroscope** ([nav_main.cpp:179-185](src/nav_main.cpp#L179-L185)):
- On first boot: Sample at rest for 10 sec to measure bias
- Saves bias to `/calib/gyro_cal.json`

**Accelerometer** ([nav_main.cpp:188-195](src/nav_main.cpp#L188-L195)):
- On first boot: 6-point orientation sequence (X+/X-/Y+/Y-/Z+/Z- facing down)
- Saves bias + scale to `/calib/accel_cal.json`

To force recalibration, delete JSON files from LittleFS or trigger via button (BTN2 long press = mag cal).

**Advanced Field Calibration (Soft-Iron):**

The built-in `calibrateMagnetometer()` function only computes hard-iron offset (bias), setting soft-iron matrix to identity. For ±5° heading accuracy on a DPV with variable magnetic signature, proper soft-iron calibration is required using ellipsoid fitting:

1. **Data Collection** ([util/mag_cal_collect.cpp](src/util/mag_cal_collect.cpp)): Collect raw mag samples to CSV on LittleFS during field rotation
2. **Export Data**: Dump CSV to serial terminal and copy to PC
3. **Ellipsoid Fitting** ([tools/mag_calibration.py](tools/mag_calibration.py)): Python script computes hard-iron + soft-iron matrix using least-squares
4. **Import Calibration**: Upload generated `calib_mag_cal.json` to LittleFS or hardcode in `nav_main.cpp`

See [docs/mag-calibration-workflow.md](docs/mag-calibration-workflow.md) for complete procedure. This workflow is essential for real-world DPV deployment where the magnetometer is mounted on the DPV with motors, batteries, and other magnetic sources.

### I2C / Wire Usage

All sensor I2C operations use a shared global `TwoWire` reference (`gWire`). The LSM6DS33 supports two addresses (0x6A or 0x6B, depending on SA0 pin); code auto-detects in [imu.cpp:77](src/sensors/imu.cpp#L77).

I2C pins defined in [board_pins.h](src/board_pins.h): SDA=23, SCL=22.

### Status Enum Returns

Many functions return `imu::ImuStatus` (`Ok`, `NotInitialized`, `BusError`, `WhoAmIMismatch`). Always check return values; I2C errors fail silently otherwise. See [nav_main.cpp:75-79](src/nav_main.cpp#L75-L79) for error handling pattern.

### Mahony Filter Integration

See [nav_main.cpp:23-24](src/nav_main.cpp#L23-L24):

```cpp
MahonyState ahrs;  // Holds quaternion + integral feedback
MahonyParams params{ .kp = 1.0f, .ki = 0.005f, .useMag = true };
```

- **`kp`**: Proportional gain (correction strength from accel/mag)
- **`ki`**: Integral gain (bias correction over time; 0 = disabled)
- **`useMag`**: Enable magnetometer fusion (set to `true` once mag is calibrated)

Update in main loop: `mahonyUpdate(ahrs, params, gyro_rad_s, accel, mag, dt)`

## Critical Conventions

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
Calibration timing and sample counts are hardcoded in [nav_main.cpp](src/nav_main.cpp):
- `imu::calibrateMagnetometer(magCal, 10000)` - 10 sec mag calibration
- `imu::calibrateGyroscope(gyroCal, 10000)` - 10 sec gyro calibration
- `imu::calibrateAccelerometer(accelCal, 2500)` - 2.5 sec per orientation (15 sec total)

To force recalibration, manually delete the JSON files from LittleFS via serial commands or filesystem access.

## Key Files Reference

- [src/nav_main.cpp](src/nav_main.cpp) — Nav device entry point: sensor init, AHRS, GPS, flow, position estimation, serial link
- [src/display_main.cpp](src/display_main.cpp) — Display device entry point: OLED rendering, buttons, serial link receive
- [src/nav/nav_model.h](src/nav/nav_model.h) — Position estimation API (dead reckoning + GPS truth + home waypoint)
- [src/nav/nav_model.cpp](src/nav/nav_model.cpp) — Position estimation implementation (flat-earth local XY)
- [src/sensors/imu.cpp](src/sensors/imu.cpp) — Unified IMU driver (LSM6DS33 + LIS3MDL), I2C read/write, calibration routines
- [src/sensors/imu.h](src/sensors/imu.h) — Public IMU API (ImuConfig, ImuStatus, read functions)
- [src/drivers/gps.cpp](src/drivers/gps.cpp) — GPS driver (Adafruit Ultimate GPS, NMEA parsing)
- [src/drivers/flow_sensor.cpp](src/drivers/flow_sensor.cpp) — Flow sensor driver (speed from hall-effect pulses)
- [src/drivers/display.cpp](src/drivers/display.cpp) — OLED display driver (nav mode: status bar + 2×2 grid; debug mode: sensor data)
- [src/math/mahony.h](src/math/mahony.h) — AHRS filter (quaternion update from gyro/accel/mag)
- [src/math/orientation.h](src/math/orientation.h) — Quaternion ↔ Euler conversions, heading calculation
- [src/types/types.h](src/types/types.h) — Core data types (Vec3i16, Vec3f, Calib3, MagCalib, ImuConfig, AxisMap)
- [src/config.h](src/config.h) — Configuration constants (flow sensor, GPS, position baseline, display mode/units, debug packet)
- [lib/dpvlink/dpvlink.h](lib/dpvlink/dpvlink.h) — Inter-device packet format (NavPacket, DebugPacket, DisplayCmd, PacketType discriminator)
- [src/sensors/calib.h](src/sensors/calib.h) — Calibration application functions
- [src/util/storage.h](src/util/storage.h) — Calibration save/load (JSON to LittleFS)
- [src/util/logging.h](src/util/logging.h) — Data logging system (CSV-like format)
- [docs/overview.md](docs/overview.md) — Project overview, architecture, feature summary
- [docs/user-guide.md](docs/user-guide.md) — User-facing guide: boot, display, buttons, dive workflow
- [docs/calibration-guide.md](docs/calibration-guide.md) — Sensor calibration (mag, gyro, accel) + persistence
- [docs/mag-calibration-workflow.md](docs/mag-calibration-workflow.md) — Advanced soft-iron magnetometer calibration (ellipsoid fitting)
- [docs/ahrs-orientation.md](docs/ahrs-orientation.md) — AHRS Mahony filter, Euler angles, heading
- [docs/data-logging-guide.md](docs/data-logging-guide.md) — Data logging system (CSV to LittleFS)
