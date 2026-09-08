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

Log files are stored on LittleFS at `/logs/YYYYMMDD-NNN.csv`, where `NNN` is
sequential within that date:

```
/logs/20260908-001.csv    first log started on 8 Sep 2026
/logs/20260908-002.csv    second log that day
/logs/20260909-001.csv    next day, back to 001
```

The date comes from the system clock (GPS or NTP). When neither has set it yet,
the newest date prefix already on the unit is reused rather than inventing one,
so filenames stay in order; on a unit that has never had a clock at all, the
prefix is `00000000`. The sequence number is worked out from the directory each
time a file is opened, not from a counter, so deleting logs can never hand a
name to a later file.

Both properties matter beyond tidiness:

- **Name order is age order**, which is how `cleanupOldLogs()` picks what to
  prune when space runs low (see `log_names::olderThan` for the one case where
  a plain string compare gets this wrong).
- **Names are not reused**, which is what lets `net/log_sync.cpp` remember an
  upload by name. Logs written before this scheme are named `NNN.csv`; they are
  left alone and always rank as the oldest files present.

## Uploading

Closed logs upload themselves. Once the unit associates with a known network
and has been linked to a Dive Map account, `net/log_sync.cpp` sends every log
it has not already sent, one per main-loop tick. The normal post-dive flow is
just: stop logging, enable WiFi, walk away.

Two things it will not do:

- It never uploads the log currently being written. Half a file would upload
  now and the finished file again later as different bytes, which the server's
  content-hash dedupe cannot collapse — so **stop logging before connecting**,
  or the dive waits for the next pass.
- It never runs during a calibration, whose own cloud upload is blocking.

Uploads are blocking, so the display shows an "uploading" screen and the unit
is unresponsive until the pass finishes. A failed upload backs off for five
minutes; reconnecting retries immediately.

The record of what has been uploaded lives in `/log_uploads.json`, keyed by
filename *and* size. An entry whose file no longer exists is dropped whenever
that record is rewritten, so deleting a log clears its uploaded flag — by the
file manager, by `Delete Selected`, by the low-space prune, or by a
`-t uploadfs` wipe, without any of those knowing log_sync exists.

`tern.local`'s file manager still has a per-file **Upload to cloud** button as
the force/retry path, and marks anything it sends so the automatic pass does
not send it twice. Uploading the same bytes twice is harmless either way — the
server returns 409 and the device treats that as success — so a lost record
costs bandwidth, never a duplicate dive.

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

## Future Optimization: Skip Logging While Stationary

Not implemented — flagged during 2026-07-23 flash/partition planning as a possible
future step, not a current plan.

Worst-case sizing for the LittleFS partition (see `partitions_nav.csv`) currently
assumes a continuous ~2hr dive at 1 log row/sec (~500KB, per field data at 1 Hz
logging — `LOG_LOW_INTERVAL_MS`/`LOG_HIGH_INTERVAL_MS` in `config.h`). In practice a
meaningful chunk of that window is the diver stationary — kitting up on the beach
before the dive starts, or holding position looking at a wreck — where DR position
isn't advancing and a log row adds little value. Gating logging on the same
flow-threshold signal already used elsewhere (`DR_MIN_FLOW_SPEED_MS` in `config.h`,
or the `SPEED_CAL_START_THRESHOLD_HZ`/`SPEED_CAL_STOP_THRESHOLD_MS` pair used by speed
cal's run detection) would shrink the realistic worst-case log size, giving more
partition margin without changing the LittleFS allocation. Would need a decision on
whether HIGH-level (diagnostic) logging should still log at rest, since that mode is
sometimes used specifically to debug stationary sensor behavior.

## Troubleshooting

| Problem | Fix |
|---------|-----|
| "LittleFS mount failed" | Check `board_build.filesystem = littlefs` in platformio.ini |
| "Could not open log file" | Check free space: `LittleFS.usedBytes()` vs `LittleFS.totalBytes()` |
| Data corruption after power loss | System auto-flushes every ~1 KB; add explicit flush after critical entries if needed |
