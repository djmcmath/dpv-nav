# Copilot Instructions for DPV-Nav

## Project Overview
DPV-Nav is an inertial navigation system for underwater DPVs (Diver Propulsion Vehicles / scooters). It provides tilt-compensated heading, dead-reckoning position via flowmeter, and heading/distance to home on a small OLED display.

**Target Hardware:** ESP32 (FeatherESP32), LSM6DS33 (accel/gyro), LIS3MDL (magnetometer), flow sensor, OLED display  
**Language:** C++ (Arduino framework)

## Critical Architecture Patterns

### Namespace Organization
Code is organized into namespaces by subsystem:
- `imu::` - Sensor drivers and raw data reads (LSM6DS33 accel/gyro, LIS3MDL mag)
- `mahony::` - AHRS (attitude/heading reference) using Mahony filter  
- `ui::` - Display/output (console_update for OLED)
- `dpvnav::` - Main app (setup/loop in [firmware/src/main.cpp](firmware/src/main.cpp))

### Sensor Configuration Pattern
See [firmware/src/main.cpp](firmware/src/main.cpp) lines 14-24: Sensors are configured at startup via structs passed to `imu::init()`:
```cpp
imu::ImuConfig imuConfig{ .accel_g_fullscale = 16.0f, .gyro_dps_fullscale = 2000.0f, ... };
imu::AxisMap imuAxisMap{ .x_axis = +1, .y_axis = +2, .z_axis = +3 };
```
The `AxisMap` struct handles board orientation (maps logical X/Y/Z axes to physical sensor axes with sign).

### Data Flow
1. **Raw reads** (imu.cpp): I2C reads → `Vec3i16` (raw counts)
2. **Unit conversion** (imu.h): Raw counts → physical units (g, rad/s, µT)  
3. **Calibration** (calib.h): Apply bias & scale (Calib3 for accel/gyro, MagCalib with soft-iron for mag)
4. **AHRS** (mahony.h): Normalized vectors → Quaternion orientation  
5. **Navigation**: Heading/position to display

### I2C / Wire Usage
All sensor I2C operations use a shared `TwoWire` reference. The LSM6DS33 supports two addresses (0x6A or 0x6B, depending on SA0 pin); code auto-detects in [imu.cpp](firmware/src/sensors/imu.cpp) line 77.

## Build & Compilation

### Compilation Command
```bash
arduino-cli compile -b esp32:esp32:featheresp32 --only-compilation-database --build-path build --clean
```
**In VS Code:** Ctrl-Shift-P → Run Task → "Arduino: Generate compile_commands.json" (sets up IntelliSense).

### Key Directories
- `firmware/src/` — Main source (mirrors `build/sketch/src/` after Arduino build)
- `firmware/build/` — Generated artifacts (compile_commands.json, build_opt.h, etc.)
- `docs/` — Calibration & user guides

## Project-Specific Conventions

### Struct-Based Configuration
Configuration uses struct initialization with named fields, not function params (e.g., `ImuConfig`, `MahonyParams`, `Calib3`). Keeps init calls readable and decouples config from init logic.

### Status Enum Returns
Many functions return `ImuStatus` (OK, NotInitialized, BusError, WhoAmIMismatch). Always check return values; many operations fail silently on I2C errors.

### Raw vs. Converted Data
- `readAccelRaw()` / `readGyroRaw()` / `readMagRaw()` → `Vec3i16` (raw sensor counts)
- `readAccel_g()` / `readGyro_rad_s()` / `readMag_uT()` → `Vec3f` (physical units)  
When adding new sensor reads, follow this pattern.

### Calibration System
- **Accel/Gyro:** Simple bias (subtract) + scale (multiply): `Calib3` in [sensors/calib.h](firmware/src/sensors/calib.h)
- **Magnetometer:** Hard-iron offset (bias) + soft-iron correction (3×3 matrix): `MagCalib`  
Not yet loaded from EEPROM (TODO in main.cpp line 54).

### Mahony Filter Integration
See [main.cpp](firmware/src/main.cpp) lines 11-13:
- `MahonyState` holds quaternion (orientation) + integral feedback
- `MahonyParams` controls gains (kp, ki) and mag enable/disable
- Call `mahonyUpdate()` in loop with normalized accel/gyro/mag vectors

## Common Workflows

### Adding a New Sensor Driver
1. Create header in `firmware/src/drivers/` with init + read functions
2. Add sensor address/register constants at top of .cpp
3. Use `gWire` (shared TwoWire) for I2C; return `ImuStatus`
4. Call init in `imu::init()` or separately in `main.cpp` setup
5. Read raw data in loop, apply calibration before passing to AHRS

### Tweaking IMU Configuration
Modify `ImuConfig` or `AxisMap` in [main.cpp](firmware/src/main.cpp) before `imu::init()` call. Axis mapping is critical for heading accuracy.

### Debugging Sensor Communication
- Check WHO_AM_I register (LSM6DS3 = 0x69, LIS3MDL = 0x3D)
- Verify I2C address (SDA=23, SCL=22 in [board_pins.h](firmware/src/board_pins.h))
- Serial.print() raw reads to confirm data flow
- See [calibration-guide.md](docs/calibration-guide.md) for sensor setup

## Critical Files Reference
- [firmware/src/main.cpp](firmware/src/main.cpp) — App entry point, sensor init, main loop  
- [firmware/src/sensors/imu.cpp](firmware/src/sensors/imu.cpp) — LSM6DS33 & LIS3MDL drivers, I2C read/write
- [firmware/src/sensors/imu.h](firmware/src/sensors/imu.h) — Public IMU API (ImuConfig, ImuStatus, read functions)
- [firmware/src/math/mahony.h](firmware/src/math/mahony.h) — AHRS filter (quaternion update)
- [firmware/src/types/types.h](firmware/src/types/types.h) — Core data types (Vec3i16, Vec3f, etc.)
