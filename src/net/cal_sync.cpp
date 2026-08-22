#include "cal_sync.h"

#include <LittleFS.h>
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
// Check for updates (mag/hdg install sync)
// ---------------------------------------------------------------------------

String checkForUpdates() {
    std::vector<cloud::CalStatusEntry> entries;
    String err;
    gHasChecked = true;

    if (!cloud::fetchCalibrationStatus(entries, err)) {
        return "Could not check for updates: " + err;
    }
    if (entries.empty()) {
        gLastStatus.clear();
        return "You're running the most recent cal data for everything.";
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
