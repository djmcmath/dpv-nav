#pragma once

#include <cstdint>
#include "log_record.h"
#include "log_names.h"

namespace logging {

// Longest log filename (log_names::NAME_BUF_LEN) and its full path under /logs.
// net/log_sync.cpp sizes its own buffers off these.
static constexpr size_t LOG_NAME_MAX = log_names::NAME_BUF_LEN;
static constexpr size_t LOG_PATH_MAX = 32;

// The numeric value is persisted in NVS (`log_level`) and shipped in NavPacket's
// 2-bit FLAG_LOG_LEVEL field, so the nav and display boards must be flashed
// together when it changes. (Until October 2026 HIGH was 2 and MID 3.)
//
//   LOW   CSV, the common columns. What a diver runs; uploads itself.
//   MID   binary, every field, gated like LOW -- a whole dive's worth of
//         diagnostics without logging the wait on the beach.
//   HIGH  binary, every field, never gated. For bench tests, where a unit
//         sitting still is the thing being measured.
//
// "Gated" means decimated to one row per LOG_IDLE_HEARTBEAT_MS while the unit
// is idle on the surface (util/log_idle.h). LogData and the binary record are
// in log_record.h.
enum class LogLevel : uint8_t { LEVEL_OFF = 0, LEVEL_LOW, LEVEL_MID, LEVEL_HIGH };

// Initialize logging system: mount LittleFS (if not already mounted),
// clean up old logs if free space is below threshold (logs with no dive in
// them go first, see logging.cpp's isNonDiveLog), determine next sequential
// file number. Starts in OFF state — no file is opened until cycleLevel()
// moves away from OFF.
bool init();

// Shutdown logging system (close file if open).
void shutdown();

// The level the diver has selected. Changes the instant cycleLevel() is called,
// which is what the menu label and the NavPacket log-level flags report — the
// file it names may not be open yet (see LOG_COMMIT_DELAY_MS).
LogLevel getLevel();

// The level actually in effect: what the open file's schema is, or LEVEL_OFF
// while nothing is open. Equals getLevel() once a selection has settled.
LogLevel getActiveLevel();

// Cycle OFF -> LOW -> MID -> HIGH -> OFF. Selecting OFF closes the open file at once.
// Selecting LOW, MID or HIGH only records the choice and (re)starts the settle timer;
// tick() opens the file once it has held still for LOG_COMMIT_DELAY_MS.
void cycleLevel();

// Set level directly (e.g. to restore from NVS on boot). Takes effect at once —
// a level restored from NVS was already committed on a previous power cycle.
// No-op if already at that level.
void setLevel(LogLevel level);

// Call from the main loop. Opens the file for a pending selection once
// it has been stable for LOG_COMMIT_DELAY_MS. (OFF never gets this far.)
void tick();

// Log one entry. No-op if level == OFF. Call at the loop's send rate: it keeps
// the per-level rate limit itself, and at LOW/MID stretches that to
// LOG_IDLE_HEARTBEAT_MS while the unit is idle, so it needs to see every sample
// to know when idle ends.
void log(const LogData& d);

// Log one entry immediately, bypassing the rate limit. Use for rare events
// (e.g., waypoint position corrections) that must not be silently dropped.
void logImmediate(const LogData& d);

// Full path of the file currently open for writing, or "" when none is.
// net/log_sync.cpp uses this to leave the in-progress log alone: uploading a
// partial file would land one upload now and a second, byte-different one
// when the dive ends, which the server can't dedupe and the assembler would
// see as two overlapping runs.
const char* currentPath();

// True if level != OFF.
bool isLogging();

// Path of the open log file, or "" if none is open (logging off, or still
// inside the LOG_COMMIT_DELAY_MS settle window).
const char* getLogPath();

}  // namespace logging
