# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

DPV-Nav is an inertial navigation system for underwater DPVs (Diver Propulsion Vehicles / scooters). It provides tilt-compensated heading using AHRS (Mahony filter), dead-reckoning position via flowmeter integration, and displays bearing/distance to home on a small OLED.

**Target Hardware:** ESP32 (FeatherESP32), LSM6DS33 (accel/gyro), LIS3MDL (magnetometer), flow sensor, OLED display
**Language:** C++ (Arduino framework)
**Main entry point:** [firmware/src/main.cpp](firmware/src/main.cpp)

## Build & Development Commands

### Compile and Generate IntelliSense Database
```bash
cd firmware
arduino-cli compile -b esp32:esp32:featheresp32 --only-compilation-database --build-path build --clean
```

**In VS Code:** `Ctrl-Shift-P` → Run Task → "Arduino: Generate compile_commands.json"

This regenerates `firmware/build/compile_commands.json` for IntelliSense. Run this after modifying includes or adding new files.

### Upload to Device
Configure serial port in [.vscode/arduino.json](.vscode/arduino.json) (currently `COM4`), then use Arduino IDE or arduino-cli to upload.

## Code Architecture

### Namespace Organization

Code is organized into namespaces by subsystem:
- `imu::` - Sensor drivers and raw I2C reads (LSM6DS33 accel/gyro, LIS3MDL mag)
- `mahony::` - AHRS filter (quaternion-based attitude/heading reference)
- `math::` - Orientation calculations and conversions (quaternion ↔ Euler)
- `ui::` - Display output (console_update for OLED/serial)
- `logging::` - Data logging system (SPIFFS-based)
- `storage::` - Calibration persistence (JSON files in SPIFFS)
- `dpvnav::` - Main application (setup/loop)

### Directory Structure

```
firmware/src/
├── main.cpp, main.h           # App entry, sensor init, main loop
├── board_pins.h, config.h     # Hardware pin definitions and config constants
├── drivers/                   # Low-level sensor/peripheral drivers
│   ├── lsm6ds33.cpp/h         # Accel/gyro driver
│   ├── lis3mdl.cpp/h          # Magnetometer driver
│   ├── display.cpp/h          # OLED display driver
│   └── flow_sensor.cpp/h      # Flowmeter driver
├── sensors/                   # High-level sensor interfaces
│   ├── imu.cpp/h              # Unified IMU API (init, read, calibrate)
│   └── calib.cpp/h            # Calibration structures (Calib3, MagCalib)
├── math/                      # AHRS and math utilities
│   ├── mahony.cpp/h           # Mahony filter (gyro + accel + mag fusion)
│   └── orientation.cpp/h      # Quaternion/Euler conversions, heading calculation
├── nav/                       # Navigation logic
│   ├── ui_controller.cpp/h    # Display updates and user interface
│   ├── nav_model.cpp          # Dead-reckoning position model
│   └── state.h                # State machine definitions (BOOT/CAL/NAV/ERROR)
├── util/                      # Utilities
│   ├── logging.cpp/h          # Data logging to SPIFFS
│   └── storage.cpp/h          # Calibration save/load (JSON)
└── types/
    └── types.h                # Core data types (Vec3i16, Vec3f, Calib3, MagCalib, etc.)
```

### Data Flow Pipeline

1. **Raw I2C reads** ([sensors/imu.cpp](firmware/src/sensors/imu.cpp)): I2C transaction → `Vec3i16` (raw counts)
2. **Unit conversion** ([sensors/imu.h](firmware/src/sensors/imu.h)): Raw counts → physical units (g, rad/s, µT)
3. **Calibration** ([sensors/calib.h](firmware/src/sensors/calib.h)):
   - Accel/Gyro: `Calib3` applies bias subtraction + scale factor
   - Magnetometer: `MagCalib` applies hard-iron offset + soft-iron matrix (3×3)
4. **AHRS fusion** ([math/mahony.h](firmware/src/math/mahony.h)): Normalized sensor vectors → `Quaternion` (orientation)
5. **Euler extraction** ([math/orientation.h](firmware/src/math/orientation.h)): Quaternion → Roll/Pitch/Yaw (rad)
6. **Heading calculation**: Yaw + magnetic declination → True heading (deg)
7. **Dead reckoning** (future): Flowmeter + heading → Position (distance/bearing to home)

### Sensor Configuration Pattern

Sensors are configured at startup via structs passed to `imu::init()`. See [main.cpp](firmware/src/main.cpp) lines 21-25:

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

The system supports automatic calibration with persistence to SPIFFS (flash storage):

**Magnetometer** ([main.cpp:52-73](firmware/src/main.cpp#L52-L73)):
- On first boot: Min/max sweep calibration (rotate device through all orientations for 10 sec)
- Saves hard-iron offset + soft-iron matrix to `mag_cal.json`
- On subsequent boots: Loads from SPIFFS (skip calibration unless file missing)

**Gyroscope** ([main.cpp:75-79](firmware/src/main.cpp#L75-L79)):
- On first boot: Sample at rest for 10 sec to measure bias
- Saves bias to `gyro_cal.json`

**Accelerometer** ([main.cpp:81-85](firmware/src/main.cpp#L81-L85)):
- On first boot: 6-point orientation sequence (X+/X-/Y+/Y-/Z+/Z- facing down)
- Saves bias + scale to `accel_cal.json`

To force recalibration, delete JSON files from SPIFFS or hold a button at boot (TODO: [main.cpp:54](firmware/src/main.cpp#L54)).

**Advanced Field Calibration (Soft-Iron):**

The built-in `calibrateMagnetometer()` function only computes hard-iron offset (bias), setting soft-iron matrix to identity. For ±5° heading accuracy on a DPV with variable magnetic signature, proper soft-iron calibration is required using ellipsoid fitting:

1. **Data Collection** ([util/mag_cal_collect.cpp](firmware/src/util/mag_cal_collect.cpp)): Collect raw mag samples to CSV on SPIFFS during field rotation
2. **Export Data**: Dump CSV to serial terminal and copy to PC
3. **Ellipsoid Fitting** ([tools/mag_calibration.py](tools/mag_calibration.py)): Python script computes hard-iron + soft-iron matrix using least-squares
4. **Import Calibration**: Upload generated `calib_mag_cal.json` to SPIFFS or hardcode in `main.cpp`

See [docs/mag-calibration-workflow.md](docs/mag-calibration-workflow.md) for complete procedure. This workflow is essential for real-world DPV deployment where the magnetometer is mounted on the DPV with motors, batteries, and other magnetic sources.

### I2C / Wire Usage

All sensor I2C operations use a shared global `TwoWire` reference (`gWire`). The LSM6DS33 supports two addresses (0x6A or 0x6B, depending on SA0 pin); code auto-detects in [imu.cpp:77](firmware/src/sensors/imu.cpp#L77).

I2C pins defined in [board_pins.h](firmware/src/board_pins.h): SDA=23, SCL=22.

### Status Enum Returns

Many functions return `imu::ImuStatus` (`Ok`, `NotInitialized`, `BusError`, `WhoAmIMismatch`). Always check return values; I2C errors fail silently otherwise. See [main.cpp:99-113](firmware/src/main.cpp#L99-L113) for error handling pattern.

### Mahony Filter Integration

See [main.cpp:15-16](firmware/src/main.cpp#L15-L16):

```cpp
MahonyState ahrs;  // Holds quaternion + integral feedback
MahonyParams params{ .kp = 0.5f, .ki = 0.0f, .useMag = false };
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
Euler angle extraction can experience gimbal lock at pitch = ±90°. The quaternion representation in `MahonyState.q` is singularity-free. See [main.cpp:138-140](firmware/src/main.cpp#L138-L140) for gimbal lock detection debug code.

## Common Development Workflows

### Adding a New Sensor Driver
1. Create header in `firmware/src/drivers/` with init + read functions
2. Add sensor address/register constants at top of .cpp
3. Use `gWire` (shared TwoWire) for I2C; return `ImuStatus`
4. Call init in `imu::init()` or separately in `main.cpp` setup
5. Read raw data in loop, apply calibration before passing to AHRS

### Tweaking IMU Configuration
Modify `ImuConfig` or `AxisMap` in [main.cpp](firmware/src/main.cpp) before `imu::init()` call. Axis mapping is critical for heading accuracy. Test by rotating device and verifying heading changes match physical rotation direction.

### Debugging Sensor Communication
- Check WHO_AM_I register (LSM6DS33 = 0x69, LIS3MDL = 0x3D)
- Verify I2C address and pins in [board_pins.h](firmware/src/board_pins.h)
- Use `Serial.print()` to log raw reads and confirm data flow
- See [docs/calibration-guide.md](docs/calibration-guide.md) for sensor setup details

### Modifying Calibration Parameters
Calibration timing and sample counts are hardcoded in [main.cpp](firmware/src/main.cpp):
- `imu::calibrateMagnetometer(magCal, 10000)` - 10 sec mag calibration
- `imu::calibrateGyroscope(gyroCal, 10000)` - 10 sec gyro calibration
- `imu::calibrateAccelerometer(accelCal, 2500)` - 2.5 sec per orientation (15 sec total)

To force recalibration, manually delete the JSON files from SPIFFS via serial commands or filesystem access.

## Key Files Reference

- [firmware/src/main.cpp](firmware/src/main.cpp) — App entry point, sensor init, calibration, main loop
- [firmware/src/sensors/imu.cpp](firmware/src/sensors/imu.cpp) — Unified IMU driver (LSM6DS33 + LIS3MDL), I2C read/write, calibration routines
- [firmware/src/sensors/imu.h](firmware/src/sensors/imu.h) — Public IMU API (ImuConfig, ImuStatus, read functions)
- [firmware/src/math/mahony.h](firmware/src/math/mahony.h) — AHRS filter (quaternion update from gyro/accel/mag)
- [firmware/src/math/orientation.h](firmware/src/math/orientation.h) — Quaternion ↔ Euler conversions, heading calculation
- [firmware/src/types/types.h](firmware/src/types/types.h) — Core data types (Vec3i16, Vec3f, Calib3, MagCalib, ImuConfig, AxisMap)
- [firmware/src/sensors/calib.h](firmware/src/sensors/calib.h) — Calibration application functions
- [firmware/src/util/storage.h](firmware/src/util/storage.h) — Calibration save/load (JSON to SPIFFS)
- [firmware/src/util/logging.h](firmware/src/util/logging.h) — Data logging system (CSV-like format)
- [docs/calibration-guide.md](docs/calibration-guide.md) — User-facing calibration instructions
- [docs/data-logging-guide.md](docs/data-logging-guide.md) — Data logging usage and format
