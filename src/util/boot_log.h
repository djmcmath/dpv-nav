#pragma once

#include <Arduino.h>

// Persistent (NVS-backed) record of resets and of logging's boot-time
// cleanup pass -- the two things a mid-dive brownout makes impossible to
// diagnose from Serial alone, because nobody is watching Serial underwater.
//
// Two independent jobs:
//  1. Remember *why* the chip came up (brownout vs. panic vs. a deliberate
//     RESET/OTA/power-off) across a ring of recent boots, plus whatever
//     cleanupOldLogs() deleted on each of those boots. Survives `uploadfs`
//     (NVS is a separate flash partition from LittleFS).
//  2. Remember the path of whatever log file is open, so that if a crash
//     interrupts the session, the very next boot's cleanupOldLogs() pass can
//     recognize that file as "was being written, not stale" -- the in-RAM
//     guard that normally does this (logging.cpp's gLogPath) is reset to
//     empty on every boot and so cannot tell the two cases apart on its own.
namespace boot_log {

// Call once, as early in setup() as possible -- before anything else touches
// NVS or LittleFS -- so the reset reason reflects what actually happened
// last, not this boot's own activity. Starts a new ring-buffer entry and
// primes protectedLogPath()'s cache from whatever the previous session left.
void recordBoot();

// Persist the path of the log file now open. Call from logging.cpp's
// openNextFile() on success. Overwrites whatever was there.
void setActiveLogPath(const char* path);

// Clear the persisted path. Call from logging.cpp's closeFile() -- a
// deliberate close (OFF, a level change, or POWER_OFF) means the file is
// complete and no longer needs crash protection.
void clearActiveLogPath();

// The path persisted by the *previous* session, read once per boot before
// cleanupOldLogs() runs. "" if none (clean last shutdown, or first boot ever).
const char* protectedLogPath();

// Record that cleanupOldLogs() deleted `path` (size `sizeBytes`), attached to
// this boot's ring-buffer entry. `wasProtected` should always be false now --
// true means a file matching protectedLogPath() got deleted anyway, i.e. the
// exclusion in logging.cpp's findOldestLog() has regressed.
void recordCleanupDeletion(const char* path, uint32_t sizeBytes, bool wasProtected);

// JSON array of recorded boots, oldest first: reset reason and any cleanup
// deletions recorded against each. Served at /api/boot-log.
String statusJson();

}  // namespace boot_log
