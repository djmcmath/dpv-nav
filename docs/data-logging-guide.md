# Data Logging System

## Overview

The logging system provides persistent CSV-format data logging to ESP32 LittleFS flash storage. Each log entry contains raw sensor readings, calibrated values, and processed orientation data.

## API

```cpp
#include "util/logging.h"

logging::init();                         // Initialize LittleFS and create log file
logging::logEntry(entry);                // Write a data entry
logging::shutdown();                     // Close files and cleanup
logging::rotateLog();                    // Start a new log file
logging::isLogging();                    // Check if logging is active
logging::getLogPath();                   // Get current log file path
logging::getBytesLogged();               // Get bytes written so far
```

## LogEntry Structure

```cpp
logging::LogEntry entry{
  .timestamp_ms = millis(),
  .mag_raw      = magRawRead,       // Vec3i16
  .accel_raw    = accelRawRead,     // Vec3i16
  .gyro_raw     = gyroRawRead,      // Vec3i16
  .mag_cal      = magCalRead,       // Vec3f (calibrated)
  .accel_cal    = accelCalRead,     // Vec3f (calibrated)
  .gyro_cal     = gyroCalRead,      // Vec3f (calibrated)
  .heading_deg  = headingDeg,       // float (from AHRS)
  .roll_deg     = rollDeg,          // float (from AHRS)
  .pitch_deg    = pitchDeg          // float (from AHRS)
};
```

## CSV Format

Each log file has a header row followed by data rows:

```
timestamp_ms,mag_x_raw,mag_y_raw,mag_z_raw,accel_x_raw,...,heading_deg,roll_deg,pitch_deg
12345,1200,-450,800,16000,200,-100,...,234.5,2.1,-1.3
```

## File Storage

Log files are stored on LittleFS at paths like:
```
/logs/log_0_5.csv       (created at ~5 seconds uptime)
/logs/log_1_10.csv      (created at ~70 seconds uptime)
```

Filenames: `log_<minutes>_<seconds>.csv` based on uptime.

### Capacity

| Metric | Value |
|--------|-------|
| Entry size | ~500 bytes (CSV) |
| Entries per MB | ~2000 |
| Duration per MB at 100 Hz | ~6 minutes |
| ESP32 LittleFS partition | 1-2 MB typical |

## Usage

```cpp
void setup() {
  // ... sensor init ...
  if (!logging::init()) {
    Serial.println("Error: Could not initialize logging");
  }
}

void loop() {
  // ... read sensors, run AHRS ...

  logging::LogEntry entry{
    .timestamp_ms = millis(),
    .mag_raw = magRaw, .accel_raw = accelRaw, .gyro_raw = gyroRaw,
    .mag_cal = magCal, .accel_cal = accelCal, .gyro_cal = gyroCal,
    .heading_deg = headingDeg, .roll_deg = rollDeg, .pitch_deg = pitchDeg
  };
  logging::logEntry(entry);
}
```

## Analyzing Logs

### Python
```python
import pandas as pd
import matplotlib.pyplot as plt

df = pd.read_csv('log_0_5.csv')
plt.plot(df['timestamp_ms'], df['heading_deg'])
plt.xlabel('Time (ms)')
plt.ylabel('Heading (degrees)')
plt.show()
```

## Troubleshooting

| Problem | Fix |
|---------|-----|
| "LittleFS mount failed" | Check `board_build.filesystem = littlefs` in platformio.ini |
| "Could not open log file" | Check free space: `LittleFS.usedBytes()` vs `LittleFS.totalBytes()` |
| Data corruption after power loss | System auto-flushes every ~1 KB; add explicit flush after critical entries if needed |
