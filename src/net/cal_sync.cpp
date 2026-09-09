#include "cal_sync.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <string.h>
#include <Preferences.h>
#include <vector>

#include "cloud_client.h"
#include "wifi_manager.h"
#include "../nav_main.h"
#include "../util/hdg_cal.h"
#include "../util/storage.h"

namespace cal_sync {

// ---------------------------------------------------------------------------
// Mode <-> file mapping
// ---------------------------------------------------------------------------

struct ModeFiles {
    const char* mode;
    const char* activePath;
    const char* syncPath;  // staging area for a pulled-but-not-yet-applied result
};

static const ModeFiles MODE_FILES[] = {
    { "baseline", storage::MAG_BASE_FILE, "/mag_base_sync.json" },
    { "mounted",  storage::MAG_MOUNT_FILE, "/mag_mount_sync.json" },
    { "hdg",      hdg_cal::FILE_PATH,      "/hdg_fourier_sync.json" },
};

static const ModeFiles* findModeFiles(const String& mode) {
    for (auto& m : MODE_FILES) {
        if (mode == m.mode) return &m;
    }
    return nullptr;
}

struct BackupFiles {
    const char* kind;
    const char* path;
};

static const BackupFiles BACKUP_FILES[] = {
    { "accel_cal_backup", "/accel_cal.json" },
    { "gyro_cal_backup",  "/gyro_cal.json" },
    { "speed_cal_backup", "/speed_cal.json" },
};

static bool copyFile(const char* srcPath, const char* dstPath) {
    File src = LittleFS.open(srcPath, FILE_READ);
    if (!src) return false;
    File dst = LittleFS.open(dstPath, FILE_WRITE);
    if (!dst) {
        src.close();
        return false;
    }
    uint8_t buf[256];
    size_t n;
    while ((n = src.read(buf, sizeof(buf))) > 0) dst.write(buf, n);
    dst.close();
    src.close();
    return true;
}

// ---------------------------------------------------------------------------
// Pending-confirm NVS record (calibration-install-sync-plan.md's Risks
// section: "a failed confirm after a successful apply" must be retryable
// across reboots, not just within the tern.local session that triggered it).
// One string field per mag mode; empty = nothing pending.
// ---------------------------------------------------------------------------

static constexpr char PENDING_NS[] = "cal_pending";
static const char* PENDING_MODES[] = { "baseline", "mounted", "hdg" };

static void setPendingConfirm(const char* mode, const String& calId) {
    Preferences prefs;
    if (!prefs.begin(PENDING_NS, /*readOnly=*/false)) return;
    prefs.putString(mode, calId);
    prefs.end();
}

static String getPendingConfirm(const char* mode) {
    Preferences prefs;
    if (!prefs.begin(PENDING_NS, /*readOnly=*/true)) return "";
    String v = prefs.getString(mode, "");
    prefs.end();
    return v;
}

static void clearPendingConfirm(const char* mode) {
    setPendingConfirm(mode, "");
}

// ---------------------------------------------------------------------------
// Local-only last-check cache, for statusJson()
// ---------------------------------------------------------------------------

struct LastStatusEntry {
    String mode;
    bool   inSync;
};

static std::vector<LastStatusEntry> gLastStatus;
static bool gHasChecked = false;

String statusJson() {
    if (!gHasChecked) return "{\"checked\":false}";
    String json = "{\"checked\":true,\"modes\":[";
    for (size_t i = 0; i < gLastStatus.size(); i++) {
        if (i > 0) json += ",";
        json += "{\"mode\":\"" + gLastStatus[i].mode + "\",\"in_sync\":";
        json += gLastStatus[i].inSync ? "true" : "false";
        json += "}";
    }
    json += "]}";
    return json;
}

// ---------------------------------------------------------------------------
// Gap-fill target map
// ---------------------------------------------------------------------------
// Cached on flash so a gap-fill session can start at the water with no WiFi.
// Refreshed only by checkForUpdates(), which is an explicit diver action --
// there is no background poll, and a stale map is preferable to no map (the
// server re-grades the merged result either way).

static constexpr const char* CAL_TARGETS_FILE = "/cal_targets.json";

// Persist the freshly fetched map. Written to a temp path and renamed so a
// power loss mid-write can't leave a half-parsed grid behind -- gap-fill would
// happily start on it and send the diver to cells nobody flagged.
//
// Reports which specific step failed via errOut. This one function has three
// very different ways to fail (can't open temp file / wrote nothing despite
// a valid handle / rename onto the real path failed), and collapsing them
// into a single bool left no way to tell "flash is somehow wedged" from "just
// a rename hiccup" without a serial cable -- the exact gap that made a real
// 612KB-free, still-fails case undiagnosable from tern.local alone.
static bool saveTargets(const uint8_t grid[60], bool degraded,
                         const uint8_t rollGrid[60][MAG_CAL_ROLL_SECTORS], bool hasRoll,
                         String& errOut) {
    const char* tmpPath = "/cal_targets.tmp";
    File f = LittleFS.open(tmpPath, "w");
    if (!f) { errOut = "could not open " + String(tmpPath) + " for write"; return false; }

    f.print("{\"degraded\":");
    f.print(degraded ? "true" : "false");
    f.print(",\"grid\":[");
    for (int i = 0; i < 60; i++) {
        if (i > 0) f.print(",");
        f.print((int)grid[i]);
    }
    f.print("],\"has_roll\":");
    f.print(hasRoll ? "true" : "false");
    f.print(",\"roll_grid\":[");
    for (int i = 0; i < 60; i++) {
        for (int k = 0; k < MAG_CAL_ROLL_SECTORS; k++) {
            if (i > 0 || k > 0) f.print(",");
            f.print((int)rollGrid[i][k]);
        }
    }
    f.print("]}");
    // f.size() stats the path at the filesystem level; the prints above went
    // through buffered stdio (fwrite) and haven't necessarily reached that
    // layer yet for a payload this small. Without an explicit flush first,
    // size() reads the pre-write (empty) stat and this check fails every
    // time, real write or not -- confirmed against arduino-esp32's
    // VFSFileImpl::size()/write(), not a guess.
    f.flush();
    bool ok = (f.size() > 0);
    f.close();
    if (!ok) {
        LittleFS.remove(tmpPath);
        errOut = "wrote 0 bytes to " + String(tmpPath);
        return false;
    }

    LittleFS.remove(CAL_TARGETS_FILE);
    if (!LittleFS.rename(tmpPath, CAL_TARGETS_FILE)) {
        errOut = "rename " + String(tmpPath) + " -> " + String(CAL_TARGETS_FILE) + " failed";
        return false;
    }
    return true;
}

// Shared reader for loadTargets()/targetsAreDegraded()/loadRollTargets().
// Anything short of a complete, correctly sized grid is a hard failure: a
// partially populated map is worse than none, because the diver would trust
// it. `outRollGrid`/`outHasRoll` are optional (pass nullptr to skip) --
// loadTargets() doesn't need them, so it skips the extra parsing.
static bool readTargets(uint8_t* outGrid60, bool* outDegraded,
                         uint8_t outRollGrid60x4[60][MAG_CAL_ROLL_SECTORS], bool* outHasRoll) {
    if (!LittleFS.exists(CAL_TARGETS_FILE)) return false;
    File f = LittleFS.open(CAL_TARGETS_FILE, "r");
    if (!f) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;

    JsonArray grid = doc["grid"];
    if (grid.isNull() || grid.size() != 60) return false;

    if (outGrid60) {
        for (int i = 0; i < 60; i++) {
            int v = grid[i] | 0;
            outGrid60[i] = (v >= 0 && v <= 3) ? (uint8_t)v : (uint8_t)0;
        }
    }
    if (outDegraded) *outDegraded = doc["degraded"] | false;

    if (outRollGrid60x4 || outHasRoll) {
        bool hasRoll = doc["has_roll"] | false;
        JsonArray rollGrid = doc["roll_grid"];
        if (hasRoll && !rollGrid.isNull() && rollGrid.size() == 60 * (size_t)MAG_CAL_ROLL_SECTORS) {
            if (outRollGrid60x4) {
                for (int i = 0; i < 60; i++) {
                    for (int k = 0; k < MAG_CAL_ROLL_SECTORS; k++) {
                        int v = rollGrid[i * MAG_CAL_ROLL_SECTORS + k] | 0;
                        outRollGrid60x4[i][k] = (v >= 0 && v <= 3) ? (uint8_t)v : (uint8_t)0;
                    }
                }
            }
            if (outHasRoll) *outHasRoll = true;
        } else {
            if (outRollGrid60x4) memset(outRollGrid60x4, 0, 60 * MAG_CAL_ROLL_SECTORS);
            if (outHasRoll) *outHasRoll = false;
        }
    }
    return true;
}

bool loadTargets(uint8_t outGrid60[60]) {
    if (!outGrid60) return false;
    memset(outGrid60, 0, 60);
    return readTargets(outGrid60, nullptr, nullptr, nullptr);
}

bool targetsAreDegraded() {
    bool degraded = false;
    if (!readTargets(nullptr, &degraded, nullptr, nullptr)) return false;
    return degraded;
}

bool loadRollTargets(uint8_t outRollGrid60x4[60][MAG_CAL_ROLL_SECTORS]) {
    if (!outRollGrid60x4) return false;
    memset(outRollGrid60x4, 0, 60 * MAG_CAL_ROLL_SECTORS);
    bool hasRoll = false;
    if (!readTargets(nullptr, nullptr, outRollGrid60x4, &hasRoll)) return false;
    return hasRoll;
}

// ---------------------------------------------------------------------------
// Check for updates (mag/hdg install sync)
// ---------------------------------------------------------------------------

// Refresh the cached gap-fill target map. Always returns a fragment to append
// to checkForUpdates()'s outcome string -- including on failure. This used to
// swallow failures into a Serial-only log line, which is useless in practice:
// tern.local is checked over WiFi mid-cal-dive-prep, with no USB cable and no
// serial monitor anywhere nearby, so a diver had no way to learn gap-fill
// targets didn't sync until CAL > Fill gaps refused on-device, unhelpfully,
// hours later. `err` is already diver-readable text either way -- even the
// benign "no graded baseline yet" 404 case (fetchCalTargets in
// cloud_client.cpp) reads fine inline.
static String refreshTargets() {
    uint8_t grid[60];
    bool degraded = false;
    uint8_t rollGrid[60][MAG_CAL_ROLL_SECTORS];
    bool hasRoll = false;
    String err;
    if (!cloud::fetchCalTargets(grid, degraded, rollGrid, hasRoll, err)) {
        Serial.printf("[CAL_SYNC] Gap-fill targets not updated: %s\n", err.c_str());
        return " Gap-fill targets not updated: " + err + ".";
    }

    int targets = 0;
    for (int i = 0; i < 60; i++) if (grid[i] == 1 || grid[i] == 2) targets++;

    String saveErr;
    if (!saveTargets(grid, degraded, rollGrid, hasRoll, saveErr)) {
        // Free-byte count costs nothing to include and rules that theory in
        // or out without a separate trip to the Storage line on tern.local.
        size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
        Serial.printf("[CAL_SYNC] Could not write gap-fill targets to flash: %s (%u bytes free)\n",
                      saveErr.c_str(), (unsigned)freeBytes);
        String msg = " Gap-fill targets fetched but could not be saved to flash: ";
        msg += saveErr;
        msg += " (";
        msg += (unsigned)freeBytes;
        msg += " bytes free).";
        return msg;
    }
    Serial.printf("[CAL_SYNC] Gap-fill targets synced: %d cells need work%s\n",
                  targets, degraded ? " (gaps only -- older cal)" : "");

    if (targets == 0) return " Coverage is complete -- no gap-fill needed.";
    String msg = " Gap-fill targets updated: ";
    msg += targets;
    msg += " cells need work.";
    if (degraded) msg += " (Empty cells only -- re-grade the baseline to see thin ones.)";
    return msg;
}

String checkForUpdates() {
    std::vector<cloud::CalStatusEntry> entries;
    String err;
    gHasChecked = true;

    if (!cloud::fetchCalibrationStatus(entries, err)) {
        return "Could not check for updates: " + err;
    }
    if (entries.empty()) {
        gLastStatus.clear();
        // Cal files are current, but the target map still may not be -- the
        // diver can merge collections on the website without anything about
        // the *installed* cal changing, and those merges are exactly what
        // moves the target list.
        return "You're running the most recent cal data for everything." + refreshTargets();
    }

    gLastStatus.clear();
    String outcome;
    String firstError;

    for (auto& e : entries) {
        const ModeFiles* mf = findModeFiles(e.mode);
        bool ok = false;
        String stepErr;

        if (!mf) {
            stepErr = "unrecognized mode";
        } else if (!cloud::downloadUpload(e.resultUploadId, mf->syncPath, stepErr)) {
            // stepErr already set by downloadUpload.
        } else if (!copyFile(mf->syncPath, mf->activePath)) {
            stepErr = "could not write " + String(mf->activePath);
        } else {
            // Applied locally -- reload right away so a later step in this
            // same loop (or the diver checking the display) sees it.
            reloadCalibrationFiles();
            setPendingConfirm(e.mode.c_str(), e.calibrationId);
            String confirmErr;
            if (cloud::confirmInstalled(e.calibrationId, confirmErr)) {
                clearPendingConfirm(e.mode.c_str());
            }
            // A failed confirm doesn't undo the apply -- the device really is
            // running this result now, only the acknowledgment didn't land.
            // update() retries it opportunistically.
            ok = true;
        }

        gLastStatus.push_back({ e.mode, ok });
        if (ok) {
            if (!outcome.isEmpty()) outcome += ", ";
            outcome += e.mode;
        } else if (firstError.isEmpty()) {
            firstError = e.mode + ": " + stepErr;
        }
    }

    if (outcome.isEmpty()) return "Could not install any updates: " + firstError;
    String result = "Installed: " + outcome;
    if (!firstError.isEmpty()) result += " (one or more modes failed: " + firstError + ")";
    result += refreshTargets();
    return result;
}

// ---------------------------------------------------------------------------
// Archival backup for accel/gyro/speed
// ---------------------------------------------------------------------------

void backupIfConnected(const char* kind, const char* path) {
    if (!wifi::isStaConnected()) return;
    String err;
    if (cloud::uploadBackup(kind, path, err)) {
        Serial.printf("[CalSync] Backed up %s\n", path);
    } else {
        Serial.printf("[CalSync] Opportunistic backup of %s failed: %s\n", path, err.c_str());
    }
}

String backUpNow() {
    String outcome, firstError;

    for (auto& b : BACKUP_FILES) {
        if (!LittleFS.exists(b.path)) continue;  // nothing to back up yet
        String err;
        if (cloud::uploadBackup(b.kind, b.path, err)) {
            if (!outcome.isEmpty()) outcome += ", ";
            outcome += (b.path + 1);  // skip leading '/'
        } else if (firstError.isEmpty()) {
            firstError = String(b.path) + ": " + err;
        }
    }

    if (outcome.isEmpty() && firstError.isEmpty()) return "Nothing to back up yet.";
    if (outcome.isEmpty()) return "Backup failed: " + firstError;
    String result = "Backed up: " + outcome;
    if (!firstError.isEmpty()) result += " (failed: " + firstError + ")";
    return result;
}

String restoreBackup(const char* kind) {
    String k(kind);

    // Mechanically identical to check-for-updates -- there's no separate
    // backup concept for mag/hdg, the cloud's accepted row already is the
    // backup (calibration-install-sync-plan.md's §5).
    if (k == "baseline" || k == "mounted" || k == "hdg") {
        return checkForUpdates();
    }

    const char* uploadKind;
    const char* activePath;
    if (k == "accel") {
        uploadKind = "accel_cal_backup";
        activePath = "/accel_cal.json";
    } else if (k == "gyro") {
        uploadKind = "gyro_cal_backup";
        activePath = "/gyro_cal.json";
    } else if (k == "speed") {
        uploadKind = "speed_cal_backup";
        activePath = "/speed_cal.json";
    } else {
        return "Unknown calibration type.";
    }

    String restorePath = String(activePath) + ".restore";
    String err;
    if (!cloud::fetchLatestBackup(uploadKind, restorePath.c_str(), err)) {
        return "Restore failed: " + err;
    }
    if (!copyFile(restorePath.c_str(), activePath)) {
        LittleFS.remove(restorePath);
        return "Restore failed: could not write " + String(activePath);
    }
    LittleFS.remove(restorePath);

    // speed_cal is read fresh from its file on every use (see
    // speed_cal::load()'s call sites) -- no in-memory state to refresh.
    // accel/gyro are cached at boot, so they need the same reload step
    // check-for-updates uses.
    if (k != "speed") reloadCalibrationFiles();

    return "Restored " + k + " calibration from the most recent cloud backup.";
}

// ---------------------------------------------------------------------------
// Pending-confirm retry
// ---------------------------------------------------------------------------

static uint32_t gNextRetryDueMs = 0;
static constexpr uint32_t RETRY_INTERVAL_MS = 5 * 60 * 1000;  // 5 min

void init() {
    // Give WiFi a little time to come up before the first retry attempt --
    // not correctness-critical (update() just no-ops without a connection),
    // purely to avoid a doomed attempt on every single boot.
    gNextRetryDueMs = millis() + 15000;
}

void update() {
    uint32_t now = millis();
    if (now < gNextRetryDueMs) return;
    gNextRetryDueMs = now + RETRY_INTERVAL_MS;

    if (!wifi::isStaConnected()) return;

    for (auto mode : PENDING_MODES) {
        String calId = getPendingConfirm(mode);
        if (calId.isEmpty()) continue;
        String err;
        if (cloud::confirmInstalled(calId, err)) {
            clearPendingConfirm(mode);
            Serial.printf("[CalSync] Retried confirm-installed for %s -- succeeded\n", mode);
        }
    }
}

}  // namespace cal_sync
