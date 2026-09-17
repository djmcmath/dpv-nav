# Data Logging System

## Overview

The logging system provides persistent CSV-format data logging to ESP32 LittleFS flash storage. What each row contains depends on the log level (below): position and depth telemetry always, sensor and attitude data only at the higher levels.

## Log Levels

Cycled from the display with `CONFIG > Log`: OFF → LOW → MID → HIGH → OFF. Each
level writes its own header, so a file's schema is fixed when it opens
(`getActiveLevel()`), not when the diver next changes the selection.

| Level | Wire/NVS value | Interval | Columns | Cost |
|---|---|---|---|---|
| OFF | 0 | — | nothing is open | — |
| LOW | 1 | `LOG_LOW_INTERVAL_MS` (1 s) | timestamp, local time, heading, speed + source, position, lat/lon + source, GPS sats/HDOP, depth, water temp, `mag_temp_c` | ~103 B/row, ~360 KiB/hour |
| MID | 3 | `LOG_MID_INTERVAL_MS` (1.5 s) | LOW's columns plus `mag_x/y/z_cal`, `pitch_deg`, `roll_deg` | ~140 B/row, ~330 KiB/hour |
| HIGH | 2 | `LOG_HIGH_INTERVAL_MS` (1 s) | MID's columns plus raw mag/accel/gyro and calibrated accel/gyro | ~241 B/row, ~850 KiB/hour |

`mag_temp_c` is the **LIS3MDL's own die temperature**, enabled 2026-09-11
(`CTRL_REG1` TEMP_EN) and written at every level since 2026-09-14. The
magnetometer's offset moves with it — ~0.68 µT/°C horizontally on this unit —
and the firmware compensates the mag readings with it when `/mag_temp.json` is
installed (see [mag-temperature-compensation.md](./mag-temperature-compensation.md)).
So logs recorded with compensation active hold **compensated** `mag_*_raw` and
`mag_*_cal`. `water_temp_c` comes from the MS5837 in the nose, which barely sees a
board-local temperature change; never use it for magnetometer thermal work. The
die temperature's absolute value is not factory-trimmed, so use it for change,
not as a room thermometer. It logs as `nan` until a plausible reading has been
captured, and on MARK and CURRENT rows, whose sensor columns aren't sampled.

## Headings in logs are TRUE

Every heading the firmware writes is **true**, whatever the diver has chosen
under `Display > Heading`. That setting changes only what the screen shows, never
what is logged.

- `heading_deg` in every dive-log row is the fully corrected heading: AHRS
  heading → Fourier heading cal (applied in magnetic) → **+ `DEFAULT_DECLINATION_DEG`**
  → + motor offset ([nav_main.cpp](../src/nav_main.cpp), "Extract Euler angles").
- A `'C'` row's `heading_deg` (direction the current flows toward) is true.
- `/currents.csv` `toward_deg` and `held_deg` are true.
- `'M'` mark rows log `heading_deg = 0`, which is not a heading.

**The declination is a compile-time constant, not a lookup.** `DEFAULT_DECLINATION_DEG`
in [config.h](../src/config.h) is 14.7° E (southern Oregon). So:

- Magnetic is always recoverable exactly: `magnetic = heading_deg − 14.7`, mod 360.
- "True" is only true where the local declination is 14.7° E. For a dive somewhere
  else, correct with `true = heading_deg − 14.7 + local_declination`. If that
  constant ever changes, logs from before the change used the old value.

## `pos_src` — where a row's position came from

| Value | Meaning |
|---|---|
| `G` | GPS fix |
| `E` | dead-reckoning estimate |
| `W` | waypoint snap — the diver snapped position to a known waypoint in real time |
| `M` | diver mark. Written out of cadence by `logImmediate()`, with heading and speed left at zero. An annotation on the track, not a step of it. |
| `C` | **current hold.** A station-keeping measurement (`Nav > Current`) of up to 60 s; the diver can finish it early. |
| `L` | a landmark or position fix added *after* the dive. Never written by firmware — the website splices it in before correction. |

A `'C'` row **reuses two existing columns to carry something else**, which is the one thing
to know about it: `speed_ms` is the measured current magnitude in m/s, and `heading_deg` is
the direction the water flows **toward**, in degrees **true** (see "Headings in logs are TRUE" above). The diver points *upstream* during the hold, so
the compass reads where the current comes *from*; the firmware turns it round before
logging so that every consumer downstream reads one convention.

Nothing may integrate a `'C'` row as a step of the track. Its speed and heading are a
current vector, not a motion vector, and treating it as DR injects a leg at the current's
speed for the row's whole interval. `track-processor` holds it out exactly as it holds out
a mark.

Dead reckoning is suppressed for the whole hold (`nav_main.cpp`, the
`DR_MIN_FLOW_SPEED_MS` gate), so the surrounding `'E'` rows correctly log
`speed_ms = 0.000` — the scooter really is not making way over the ground.

### `/currents.csv` — every current hold, logging on or off

A `'C'` row only exists if a dive log is open. Every hold that produced a number is
**also** appended to `/currents.csv` on the nav filesystem, whether or not logging is on,
so a measurement is never lost to a forgotten Log toggle. Download it from the file list
at `tern.local`. At 64 KiB it rotates to `/currents.old.csv` (one generation kept).

| Column | Meaning |
|---|---|
| `timestamp_ms` | `millis()` at the finish — same clock as the dive log |
| `utc` | wall-clock UTC, empty if GPS/NTP never set the clock this power cycle |
| `current_ms`, `current_m_min` | averaged current magnitude |
| `toward_deg` | direction the water flows toward, **true** (same as the `'C'` row) |
| `held_deg` | circular mean of the heading the diver held (upstream), **true** |
| `held_s` | seconds from start to finish |
| `kept_s` | seconds that went into the average after edge trimming |
| `ended_early` | 1 if the diver pressed BTN1 to finish before 60 s |
| `lat`, `lon`, `pos_x_m`, `pos_y_m` | DR position at the finish |
| `depth_m`, `water_temp_c` | 0 if no depth sensor |
| `log_file` | the dive log open at the time, or empty — joins the row to its `'C'` row |

**How the average is formed.** Samples are binned per second. The second a finish-early
press lands in is dropped (it is the press). The median and MAD of the per-second means
set a limit of `max(0.05 m/s, 3 × 1.4826 × MAD)`; seconds beyond it are trimmed from
**each end only**, walking inward until the first in-limit second. Wobbles mid-hold stay
in — they are part of what was held. The result is the sample-weighted mean of what is
left. A hold with no complete second saves nothing, and the display says so.

**Adding a new `pos_src` value requires a coordinated dive-map change first.**
`validate_blocks` in `tracklib/correct.py` hard-fails on an unrecognised value, so a single
unknown row makes an entire dive uncorrectable.

**MID exists because HIGH cannot cover a dive.** The LittleFS partition is 768 KiB
(`partitions_nav.csv`), so HIGH fills it in under an hour while MID runs about 2.3
hours — and MID still carries everything a heading post-mortem needs
(`tools/circle_audit.py` reads exactly these columns). `cleanupOldLogs()` prunes
oldest-first to a 32 KiB floor, so overrunning costs you your *older* logs.

**MID is value 3, not 2.** It was appended rather than inserted so that existing
NVS records and the 2-bit `FLAG_LOG_LEVEL` field keep their meanings across a
firmware update, since the nav and display boards are flashed separately. The
display's level badge reads `L0`/`L1`/`LM`/`L2` for OFF/LOW/MID/HIGH.

> **The rest of this document is stale** (2026-09-11): it describes a
> `logEntry()`/`LogEntry`/`rotateLog()` API and a `log_<minutes>_<seconds>.csv`
> filename scheme that no longer exist. The real API is in
> [src/util/logging.h](../src/util/logging.h) — `log()`, `LogData`, `cycleLevel()`,
> `setLevel()`, `tick()` — and files are `/logs/NNN.csv`.

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
