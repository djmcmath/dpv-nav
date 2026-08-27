#pragma once

#include <Arduino.h>

#include "../config.h"  // MAG_CAL_ROLL_SECTORS

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

// Reads the last-synced gap-fill target map off LittleFS into `outGrid60`
// (60 per-cell status codes, elev-major: 0=ok, 1=thin, 2=empty, 3=over).
// Returns false when no map has been synced, or the stored one is unusable --
// in which case gap-fill must refuse to start rather than show a grid that
// means nothing. See src/util/mag_cal_orient.h.
//
// Deliberately a local read with no network call: the diver may well be at the
// water with no WiFi by the time they run gap-fill, and the map is only
// refreshed by checkForUpdates(), which is an explicit action.
bool loadTargets(uint8_t outGrid60[60]);

// Reads the last-synced per-roll-sector coverage statuses into
// `outRollGrid60x4` (MAG_CAL_ROLL_SECTORS entries per cell, same status
// codes/ordering as loadTargets()). Lets the persistent gap-fill roll widget
// start a cell already knowing which roll sectors a prior accepted upload
// already covered, instead of assuming zero roll coverage every session.
//
// Returns false whenever no roll data was synced -- an older server build,
// or a calibration fit before roll coverage existed (see dive-map's
// device.rs::roll_grid_from_coverage) -- in which case the caller should
// fall back to session-local-only roll tracking, exactly as gap-fill behaved
// before this existed. Same local-read-only contract as loadTargets().
bool loadRollTargets(uint8_t outRollGrid60x4[60][MAG_CAL_ROLL_SECTORS]);

// True when the stored map was reconstructed server-side from bare sample
// counts and therefore knows only *empty* cells, not thin ones (an older
// calibration, fit before per-cell status existed). The display says so rather
// than implying a partial map is a complete one. Meaningless if loadTargets()
// returned false.
bool targetsAreDegraded();

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
