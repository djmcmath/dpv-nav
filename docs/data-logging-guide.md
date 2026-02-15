# Data Logging System

## Overview
The logging system provides persistent CSV-format data logging to ESP32 SPIFFS flash storage. Each log entry contains both raw sensor readings and calibrated/processed values.

## Features
- **CSV Format**: Easy to parse and analyze with standard tools (Excel, Python, MATLAB)
- **Dual Data Storage**: Both raw (int16) and calibrated (float) sensor values
- **Automatic Flushing**: Data written to flash approximately every 1KB to prevent loss on power failure
- **Log Rotation**: Support for creating multiple log files to manage flash space
- **Minimal Overhead**: Typical ~500 bytes per entry (configurable with formatting changes)

## Data Logged

### Raw Sensor Data
- Magnetometer (mag_x_raw, mag_y_raw, mag_z_raw) - int16
- Accelerometer (accel_x_raw, accel_y_raw, accel_z_raw) - int16
- Gyroscope (gyro_x_raw, gyro_y_raw, gyro_z_raw) - int16

### Calibrated Sensor Data
- Magnetometer (mag_x_cal, mag_y_cal, mag_z_cal) - float
- Accelerometer (accel_x_cal, accel_y_cal, accel_z_cal) - float
- Gyroscope (gyro_x_cal, gyro_y_cal, gyro_z_cal) - float

### Processed Data
- Heading (heading_deg) - float
- Roll (roll_deg) - float
- Pitch (pitch_deg) - float

## Usage

### Basic Setup (in main.cpp setup())
```cpp
#include "./util/logging.h"

void setup() {
  // ... other initialization ...
  
  // Initialize logging to SPIFFS
  if (!logging::init()) {
    Serial.println("Error: Could not initialize logging");
  }
}
```

### Log Data (in main.cpp loop())
```cpp
// At ~100 Hz or your desired sample rate:
logging::LogEntry entry{
  .timestamp_ms = millis(),
  .mag_raw = magRawRead,
  .accel_raw = accelRawRead,
  .gyro_raw = gyroRawRead,
  .mag_cal = {fx, fy, fz},        // Apply your calibration
  .accel_cal = {...},              // Apply your calibration
  .gyro_cal = {...},               // Apply your calibration
  .heading_deg = headingDeg,
  .roll_deg = 0.0f,                // Compute from AHRS quaternion
  .pitch_deg = 0.0f                // Compute from AHRS quaternion
};

if (!logging::logEntry(entry)) {
  Serial.println("Error: Failed to log entry");
}
```

### Shutdown (in cleanup or error handler)
```cpp
logging::shutdown();
```

## Storage Details

### SPIFFS Layout
```
/logs/
  ├── log_0_5.csv       (created at ~5 seconds uptime)
  ├── log_1_10.csv      (created at ~70 seconds uptime)
  └── ...
```

Filenames are generated as: `log_<minutes>_<seconds>.csv`

### Flash Size Considerations
- ESP32 typically has 4MB flash
- SPIFFS default partition: varies by board configuration
- Typical log entry: ~500 bytes = ~2000 entries/MB
- At 100 Hz: 6 minutes of logging per MB

### Performance
- Write time: ~1-2 ms per entry at 100 Hz
- CPU overhead: <5% at 100 Hz logging rate
- Flash endurance: Millions of erase cycles per sector

## Advanced Features

### Log Rotation
To create a new log file:
```cpp
logging::rotateLog();
```

### Status Queries
```cpp
// Check if logging is active
if (logging::isLogging()) {
  Serial.println(logging::getLogPath());
  Serial.print("Bytes written: ");
  Serial.println(logging::getBytesLogged());
}
```

## Future Enhancements
1. **Ring Buffer**: Limit SPIFFS usage to a fixed size
2. **Format Options**: Binary format for higher-density logging
3. **Compression**: Gzip compression for post-processing
4. **Time Sync**: Add GPS/RTC timestamp instead of uptime
5. **Selective Logging**: Log only when in NAV mode or motion detected
6. **Remote Access**: WiFi download of log files

## Troubleshooting

### "SPIFFS mount failed"
- Check that SPIFFS partition is defined in Arduino board configuration
- Try formatting: `SPIFFS.format()` before init (wipes all data!)

### "Could not open log file"
- Check free space: `SPIFFS.usedBytes()` vs `SPIFFS.totalBytes()`
- Verify permissions and paths

### Data Corruption After Power Loss
- System flushes approximately every 1KB
- Add explicit `fflush()` after critical entries if needed
- Consider adding checksum fields for validation

## Example: Processing Logs

### Python Script
```python
import pandas as pd

# Read log file
df = pd.read_csv('/path/to/log_0_5.csv')

# Extract calibrated mag data
mag_cal = df[['mag_x_cal', 'mag_y_cal', 'mag_z_cal']]

# Plot heading over time
import matplotlib.pyplot as plt
plt.plot(df['timestamp_ms'], df['heading_deg'])
plt.xlabel('Time (ms)')
plt.ylabel('Heading (degrees)')
plt.show()
```

### MATLAB Script
```matlab
data = readtable('log_0_5.csv');
plot(data.timestamp_ms, data.heading_deg)
xlabel('Time (ms)')
ylabel('Heading (degrees)')
```
