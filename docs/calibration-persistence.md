# Calibration Persistence (SPIFFS Storage)

## Overview

The DPV-Nav system can now save and load calibration data to/from JSON files on the ESP32's SPIFFS (SPI Flash File System). This allows you to:
- Perform calibration once
- Save results to flash
- Load on subsequent boots without re-calibrating every time

## Features

- **JSON Format**: Human-readable, easy to inspect and debug
- **SPIFFS Storage**: No SD card required, uses internal flash
- **Two Calibration Types**:
  - `MagCalib` - Magnetometer (hard-iron + soft-iron matrices)
  - `Calib3` - Accelerometer/Gyroscope (bias + scale)
- **Error Handling**: Returns success/failure status, detailed Serial output
- **Automatic Mounting**: SPIFFS mounts automatically on first access

## File Structure

Calibration files are stored in SPIFFS at paths like:
```
/calib/mag_cal.json
/calib/accel_cal.json
/calib/gyro_cal.json
```

### Example MagCalib JSON

```json
{
  "type": "MagCalib",
  "bias": {
    "x": 45.200000,
    "y": -28.700000,
    "z": 15.400000
  },
  "softIron": [
    [1.000000, 0.000000, 0.000000],
    [0.000000, 1.000000, 0.000000],
    [0.000000, 0.000000, 1.000000]
  ]
}
```

### Example Calib3 JSON

```json
{
  "type": "Calib3",
  "bias": {
    "x": 0.100000,
    "y": -0.050000,
    "z": 0.030000
  },
  "scale": {
    "x": 1.000000,
    "y": 1.000000,
    "z": 1.000000
  }
}
```

## API

### Magnetometer Calibration

#### `bool storage::saveMagCalibration(const char* filename, const MagCalib& cal)`

Save magnetometer calibration to JSON file.

**Parameters:**
- `filename` - Name of file (stored in `/calib/` directory, e.g., `"mag_cal.json"`)
- `cal` - MagCalib struct to save

**Returns:**
- `true` - File saved successfully
- `false` - Error occurred (check Serial output for details)

**Example:**
```cpp
#include "./util/storage.h"

MagCalib magCal;
imu::calibrateMagnetometer(magCal, 30000);
imu::setMagCalibration(magCal);

if (storage::saveMagCalibration("mag_cal.json", magCal)) {
  Serial.println("Calibration saved!");
} else {
  Serial.println("Failed to save calibration");
}
```

#### `bool storage::loadMagCalibration(const char* filename, MagCalib& cal)`

Load magnetometer calibration from JSON file.

**Parameters:**
- `filename` - Name of file to load (e.g., `"mag_cal.json"`)
- `cal` - Output buffer for loaded calibration

**Returns:**
- `true` - File loaded and parsed successfully
- `false` - File not found or parse error (check Serial output)

**Example:**
```cpp
MagCalib magCal;

// Try to load saved calibration
if (storage::loadMagCalibration("mag_cal.json", magCal)) {
  Serial.println("Loaded saved calibration");
  imu::setMagCalibration(magCal);
} else {
  Serial.println("No saved calibration, running new calibration...");
  imu::calibrateMagnetometer(magCal, 30000);
  imu::setMagCalibration(magCal);
  storage::saveMagCalibration("mag_cal.json", magCal);
}
```

### Accelerometer/Gyroscope Calibration

#### `bool storage::saveCalib3(const char* filename, const Calib3& cal)`

Save accel/gyro calibration to JSON file.

**Parameters:**
- `filename` - Name of file (stored in `/calib/` directory, e.g., `"accel_cal.json"`)
- `cal` - Calib3 struct to save

**Returns:**
- `true` - File saved successfully
- `false` - Error occurred

#### `bool storage::loadCalib3(const char* filename, Calib3& cal)`

Load accel/gyro calibration from JSON file.

**Parameters:**
- `filename` - Name of file to load
- `cal` - Output buffer for loaded calibration

**Returns:**
- `true` - File loaded and parsed successfully
- `false` - File not found or parse error

## Complete Integration Example

```cpp
#include "./sensors/imu.h"
#include "./sensors/calib.h"
#include "./util/storage.h"

MagCalib magCal{{0,0,0}, {{1,0,0},{0,1,0},{0,0,1}}};
Calib3 accelCal{{0,0,0},{1,1,1}};
Calib3 gyroCal{{0,0,0},{1,1,1}};

void setup() {
  Serial.begin(115200);
  
  // Initialize IMU
  if (!imu::init(imuConfig, imuAxisMap)) {
    Serial.println("IMU init failed");
    while(1);
  }

  // Try to load magnetometer calibration
  if (!storage::loadMagCalibration("mag_cal.json", magCal)) {
    // First boot or corrupted file - perform new calibration
    Serial.println("Running magnetometer calibration...");
    imu::calibrateMagnetometer(magCal, 30000);
    storage::saveMagCalibration("mag_cal.json", magCal);
  }
  imu::setMagCalibration(magCal);

  // Try to load accel calibration
  if (!storage::loadCalib3("accel_cal.json", accelCal)) {
    Serial.println("No accel calibration found (using defaults)");
  }

  // Try to load gyro calibration
  if (!storage::loadCalib3("gyro_cal.json", gyroCal)) {
    Serial.println("No gyro calibration found (using defaults)");
  }

  Serial.println("Calibration loaded/initialized!");
}

void loop() {
  // ... normal operation ...
}
```

## Serial Output Examples

### Successful Save

```
[STORAGE] Saved magnetometer calibration to /calib/mag_cal.json
```

### Successful Load

```
[STORAGE] Loaded magnetometer calibration from /calib/mag_cal.json
```

### File Not Found

```
[STORAGE] File not found: /calib/mag_cal.json
```

### Parse Error

```
[STORAGE] Error parsing JSON from /calib/mag_cal.json
```

## Storage Details

### File Sizes

- **MagCalib JSON**: ~200-300 bytes
- **Calib3 JSON**: ~150-200 bytes
- **Total for all calibrations**: <1 KB

### SPIFFS Capacity

- ESP32 typically has 4MB flash
- SPIFFS default partition: 1-2 MB
- Calibration files: <1 KB total
- Plenty of room for logging + calibration

### Permissions

- Files created with read/write permissions
- Can be overwritten by subsequent `save*()` calls
- Cannot be deleted via API (use SPIFFS.remove() if needed)

## Troubleshooting

### Issue: "SPIFFS mount failed"
**Cause:** SPIFFS partition not configured
**Fix:** Ensure SPIFFS is enabled in Arduino board configuration

### Issue: "Could not open /calib/..."
**Cause:** Directory path issue or disk full
**Fix:** Check SPIFFS free space: `SPIFFS.usedBytes()` vs `SPIFFS.totalBytes()`

### Issue: "Error parsing JSON"
**Cause:** File corrupted or not a valid calibration JSON
**Fix:** Delete the file, re-run calibration, and save again

### Issue: "File not found" on first boot
**Cause:** Expected - first boot has no saved calibration
**Fix:** This is normal - program should run calibration on first boot, then load it on subsequent boots

## Persistence Workflow

### First Boot (No Saved Calibration)

```
1. setup() called
2. loadMagCalibration() returns false (file doesn't exist)
3. Run calibrateMagnetometer() → 30 seconds
4. saveMagCalibration() saves to flash
5. setMagCalibration() enables calibration for reads
```

### Subsequent Boots (Saved Calibration Exists)

```
1. setup() called
2. loadMagCalibration() returns true
3. setMagCalibration() enables calibration immediately
4. Skip 30-second calibration, boot in <1 second
5. Use saved calibration for all reads
```

## Advanced: Manual Management

### View Files on Device

```cpp
// List all files in /calib/ directory
void listCalibFiles() {
  File root = SPIFFS.open("/calib");
  File file = root.openNextFile();
  
  while (file) {
    Serial.print("File: ");
    Serial.print(file.name());
    Serial.print(" Size: ");
    Serial.println(file.size());
    file = root.openNextFile();
  }
}
```

### Delete Calibration File

```cpp
// Remove saved calibration (force re-calibration on next boot)
void deleteCalibration() {
  if (SPIFFS.remove("/calib/mag_cal.json")) {
    Serial.println("Deleted mag calibration");
  } else {
    Serial.println("File not found or delete failed");
  }
}
```

### Inspect JSON File

Connect via Serial monitor and capture output, or use web dashboard (future feature) to download files.

## Future Enhancements

1. **Encrypted Storage** - Protect calibration with checksum/hash
2. **Versioning** - Track calibration versions and dates
3. **Multiple Profiles** - Store different calibrations for different conditions
4. **Auto-backup** - Keep previous calibration version
5. **Web Interface** - Download/upload calibration via WiFi
