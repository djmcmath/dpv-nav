# Data Logging Implementation Summary

## What Was Added

### 1. **logging.h** - Public API
Located: `firmware/src/util/logging.h`

Core functionality:
- `logging::init()` - Initialize SPIFFS and create log file
- `logging::logEntry(const LogEntry& entry)` - Write a data entry
- `logging::shutdown()` - Close files and cleanup
- `logging::rotateLog()` - Create a new log file
- `logging::isLogging()` - Check if logging is active
- `logging::getLogPath()` - Get current log file path
- `logging::getBytesLogged()` - Get bytes written

Data structure `LogEntry` includes:
- Timestamp (milliseconds)
- Raw sensor data (3 axes × 3 sensors)
- Calibrated sensor data (3 axes × 3 sensors)
- Processed data (heading, roll, pitch)

### 2. **logging.cpp** - Implementation
Located: `firmware/src/util/logging.cpp`

Features:
- Uses ESP32 **SPIFFS** (SPI Flash File System) for persistent storage
- Writes CSV format (human-readable, easy to parse)
- Automatic periodic flushing (~1KB intervals)
- Dynamic log file naming based on uptime
- CSV header with all field names

### 3. **storage.h** - Updated Documentation
Located: `firmware/src/util/storage.h`

Documented the flash storage approach and future options (NVS, EEPROM).

### 4. **Documentation Files**

#### `docs/data-logging-guide.md`
Complete reference guide including:
- Feature overview
- CSV data format specification
- Storage considerations and flash layout
- Performance metrics
- Troubleshooting guide
- Example Python/MATLAB analysis scripts
- Future enhancement ideas

#### `docs/logging-integration-example.cpp`
Complete working example showing:
- How to include the logging library
- Initialization in setup()
- Calibration application and logging in loop()
- Error handling
- TODO markers for roll/pitch extraction from quaternion

## How to Use

### Quick Start
1. Copy relevant initialization code from `docs/logging-integration-example.cpp` into your `firmware/src/main.cpp`
2. Call `logging::init()` in setup() after sensor initialization
3. Create a `logging::LogEntry` in your loop and call `logging::logEntry(entry)`
4. Optionally call `logging::shutdown()` on cleanup

### Log Files
- Stored in SPIFFS at paths like `/logs/log_0_5.csv`
- Filenames: `log_<minutes>_<seconds>.csv` based on uptime
- Download via USB UART or implement WiFi download feature
- Format: CSV with header row, human-readable and tool-compatible

### Data Flow
```
Raw Sensors → Apply Calibration → Create LogEntry → Write CSV → SPIFFS Flash
```

## Storage Capacity

**Typical Configuration:**
- Log entry size: ~500 bytes (CSV with newline)
- ESP32 SPIFFS: 1-2 MB typically available
- Duration per MB: 6 minutes at 100 Hz sampling
- **Example**: 2 MB = 12 minutes of continuous logging

**Optimization Tips:**
- Reduce sample rate to reduce file size
- Implement log rotation to limit file size
- Use binary format (future enhancement) for 10× compression
- Add selective logging (only when moving, etc.)

## Integration Checklist

- [ ] Copy `logging.h` and `logging.cpp` (already created)
- [ ] Include `#include "./util/logging.h"` in main.cpp
- [ ] Call `logging::init()` in setup()
- [ ] Create LogEntry structures with calibrated data in loop()
- [ ] Call `logging::logEntry(entry)` at desired rate
- [ ] (Optional) Add `logging::shutdown()` in error handler or cleanup
- [ ] Run "Arduino: Generate compile_commands.json" task
- [ ] Verify no compilation errors

## Next Steps

### Short Term
1. Integrate into main.cpp (copy code from example file)
2. Test logging with actual sensor data
3. Extract roll/pitch from AHRS quaternion for LogEntry
4. Verify CSV format and data accuracy

### Medium Term
1. Implement log download via USB UART or WiFi
2. Add ring buffer to limit SPIFFS usage
3. Create analysis scripts (Python/MATLAB)
4. Performance profiling under realistic conditions

### Long Term
1. Binary format option for higher density
2. Gzip compression
3. RTC timestamp integration
4. Selective logging based on motion/state
5. Remote WiFi access to logs
