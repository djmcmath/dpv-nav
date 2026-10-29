# Data Logging System

## Overview

The logging system writes dive logs to ESP32 LittleFS flash storage. LOW is the level a diver runs: a CSV that uploads itself to Dive Map. MID and HIGH are diagnostics: every sensor field, in a compact binary file that is decoded on a laptop.

## Log Levels

Cycled from the display with `CONFIG > Log`: OFF → LOW → MID → HIGH → OFF. A file's
format is fixed when it opens (`getActiveLevel()`), not when the diver next changes
the selection.

| Level | Wire/NVS value | File | Rate | Gated | Contents | Cost |
|---|---|---|---|---|---|---|
| OFF | 0 | — | — | — | nothing is open | — |
| LOW | 1 | `.csv` | 1 s | yes | timestamp, local time, heading, speed + source, position, lat/lon + source, GPS sats/HDOP, depth, water temp, `mag_temp_c` | ~103 B/row, ~360 KiB/hour |
| MID | 2 | `.bin` | 1 s | yes | LOW's fields plus raw and calibrated mag/accel/gyro, pitch, roll | 77 B/row, ~270 KiB/hour |
| HIGH | 3 | `.bin` | 1 s | no | same as MID | 77 B/row, ~270 KiB/hour |

MID and HIGH differ only in gating. Use MID to record a whole dive with every
field; use HIGH on the bench, where a unit sitting still is the thing being
measured. The display's level badge reads `L0`/`L1`/`LM`/`L2` for OFF/LOW/MID/HIGH.

Until October 2026 HIGH was value 2 and MID value 3, and both wrote CSV. The value
is sent in the 2-bit `FLAG_LOG_LEVEL` field, so **the nav and display boards must
be flashed together** across that change. The level saved in NVS carries over
correctly: the new numbering is stored under a new key (`log_lvl`), and a value
left under the old `log_level` key is translated on first read.

### Idle gating (LOW and MID)

A dive's log is usually bracketed by dead time: the scooter sits powered on the
beach while the diver gears up, and again afterwards. While the unit is **idle**,
LOW and MID write one row per minute (`LOG_IDLE_HEARTBEAT_MS`) instead of one per
second. Idle means all of these have held for a minute (`LOG_IDLE_ENTER_MS`):

- shallower than 1 m (`LOG_IDLE_MAX_DEPTH_M`)
- a live GPS position (`pos_src` is `G`)
- speed exactly 0
- position within 15 m of where the quiet spell began (`LOG_IDLE_MAX_DRIFT_M`)

Any one failing restores full rate on that very sample. No GPS — indoors, dive
mode, GPS switched off — means never idle. The one-minute entry delay is also what
keeps the surfacing fix: the first minute of GPS rows after a dive, which the track
correction anchors on, is always written at full rate. Mark, waypoint and current
rows are never gated. The logic is [log_idle.h](../src/util/log_idle.h), tested by
`tools/logidle_test/run.sh`.

The track processor reads any step longer than `DEFAULT_MAX_DT_S` as a gap in the
log. That is 90 s (raised from 30 s for this), so heartbeat rows are one continuous
log; keep the heartbeat well under it. It also integrates each row's speed over the
time since the previous row, so when idle ends the logger first writes the last
still sample it was holding and then the row that ended idle. The first moving row
therefore spans one tick, never the idle gap.

### Binary logs (MID and HIGH)

`/logs/YYYYMMDD-NNN.bin`: a 16-byte header, then fixed 77-byte records
([log_record.h](../src/util/log_record.h)). Values are scaled integers — accel and
gyro at the step the old CSV printed, mag at 0.01 µT, finer than the sensor's own
0.0146 µT step. As CSV these rows were ~240 bytes and filled the 768 KiB partition
in under an hour; a two-hour HIGH log is now ~540 KiB.

The unit does no decoding. Download the file from `tern.local` and run:

```bash
python tools/decode_binlog.py 20261011-003.bin     # writes 20261011-003.csv
```

The CSV has the columns the old HIGH log had, so existing scripts
(`tools/circle_audit.py`, `tools/mag_temp_fit.py`) read it unchanged. **Binary logs
upload like CSV ones** (since 0.7.23), automatically and from the `tern.local`
button. Dive Map's track processor decodes them itself, with a copy of
`decode_binlog.py`, and keeps the magnetometer, accel, gyro and attitude columns in
the assembled and corrected dive tracks. A record-layout change has to land in
`log_record.h`, `tools/decode_binlog.py` *and* `dive-map/track-processor/tracklib/binlog.py`,
and dive-map has to deploy before the firmware that writes it.
`.csv` and `.bin` share one sequence number per date.

**Nothing prunes while a log is open.** `cleanupOldLogs()` runs at boot only, and
only until 32 KiB is free, so the space a long log needs has to be free before it
starts. Check the Storage line on `tern.local` and delete old logs first.

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
| `G` | GPS fix **that the confidence gate accepted** — nav's position is that fix (plus DR for rows between 1 Hz fixes, up to `GPS_FIX_STALE_MS`). Before 2026-10-14 any fresh fix was tagged `G`, including ones the gate rejected, so older logs can hold `G` rows whose lat/lon is really nav's estimate (often the NVS-restored position just after boot or surfacing). Since the same date the CSV's `gps_satellites`/`gps_hdop` are filled on any row with a fix, `E` included, so a rejected fix's HDOP is still recorded. |
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
