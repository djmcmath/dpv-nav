#pragma once

#include <Arduino.h>

// tern.local's "Calibration Install Sync" surface
// (divemap/docs/architecture/calibration-install-sync-plan.md): pulls
// accepted-but-not-installed mag calibrations down to the unit, mirrors
// accel/gyro/speed cal data to the cloud for disaster recovery, and restores
// from that mirror. Sits between web_server.cpp (which owns HTTP routing
// only) and net/cloud_client.h (which owns the outbound HTTPS calls) --
// this module owns the mode<->file mapping and apply/backup orchestration.
//
// Every entry point below is a blocking call (one or more outbound HTTPS
// round trips) meant to be invoked directly from a web_server.cpp handler,
// same tradeoff cloud_client.h's own docs already accept for CAL-menu-
// triggered uploads: the diver just clicked a button and isn't mid-dive.
namespace cal_sync {

// Call once from setup(), after LittleFS is mounted.
void init();

// Call once per main-loop tick. Opportunistically retries any
// confirm-installed call that didn't land after a successful local apply
// (see the plan's Risks section) -- cheap no-op when nothing is pending.
void update();

// Local-only render of what the last "check for updates" / restore call
// found, for the tern.local page to show on load without a network round
// trip. Resets to "not checked yet" on reboot -- this device does not
// persist calibration history locally, only the cloud does.
String statusJson();

// Best-effort, fire-and-forget backup right after an accel/gyro/speed cal
// routine finishes -- called from nav_main.cpp at each of that cal type's
// completion points. Does nothing (not even attempting the network call)
// when WiFi isn't connected; the local cal already completed and installed
// by the time this runs, so a skipped backup here is just caught later by
// the "Back up calibration now" tern.local action instead.
void backupIfConnected(const char* kind, const char* path);

// GET /api/device/calibrations/status, then for every out-of-sync mode:
// fetch its result, stage it, apply it (reloadCalibrationFiles()), and
// confirm the install. Returns a short outcome string suitable for direct
// display on the page (e.g. "Installed baseline cal from upload ..." or
// "You're running the most recent cal data for everything").
String checkForUpdates();

// Uploads the current on-device accel/gyro/speed cal files as archival
// backups. Mag/hdg have no separate backup concept here -- the cloud's
// accepted `calibrations` row already *is* the backup (see the plan's §5) --
// so this only ever touches the three on-device-only cal types.
String backUpNow();

// Restores from the most recent cloud backup for one cal type. `kind` is
// "baseline" | "mounted" | "hdg" | "accel" | "gyro" | "speed". For the mag
// modes this delegates to checkForUpdates() (mechanically identical, per the
// plan); for accel/gyro/speed it pulls the latest `*_cal_backup` upload and
// applies it directly, since those have no accept/reject or `calibrations`
// row to compare against.
String restoreBackup(const char* kind);

}  // namespace cal_sync
