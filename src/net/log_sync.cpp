#include "log_sync.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <string.h>
#include <vector>

#include "cloud_client.h"
#include "wifi_manager.h"
#include "../config.h"
#include "../util/logging.h"

namespace log_sync {

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

static constexpr const char* LOG_DIR     = "/logs";
static constexpr const char* RECORD_FILE = "/log_uploads.json";

// How long isUploading() is true before the first blocking upload starts --
// long enough for the 10 Hz NavPacket carrying FLAG2_UPLOADING to reach the
// display, so it shows the upload screen instead of "NO LINK".
static constexpr uint32_t ANNOUNCE_MS = 300;

// Re-scan /logs this often while connected, so a log closed after the unit was
// already on WiFi (the diver stops logging in the car) gets picked up without
// a reconnect.
static constexpr uint32_t RESCAN_INTERVAL_MS = 30000;

// After a failed upload, wait this long before trying again. A fresh
// association clears it -- reconnecting is the diver saying "try now".
static constexpr uint32_t RETRY_BACKOFF_MS = 5 * 60 * 1000;

// ---------------------------------------------------------------------------
// Uploaded-file record
// ---------------------------------------------------------------------------

struct Record {
    String   name;  // basename, e.g. "20260908-001.csv"
    uint32_t size;
};

static std::vector<Record> gRecords;

static void buildPath(char* out, size_t len, const char* name) {
    snprintf(out, len, "%s/%s", LOG_DIR, name);
}

static const char* basenameOf(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// Drop records whose file is gone or has changed size, then write what's left.
// This is the whole delete-clears-the-flag mechanism: no caller has to notify
// us, because a record only survives as long as the file it describes does.
static void pruneAndSave() {
    std::vector<Record> kept;
    kept.reserve(gRecords.size());
    for (const auto& r : gRecords) {
        char path[logging::LOG_PATH_MAX];
        buildPath(path, sizeof(path), r.name.c_str());
        File f = LittleFS.open(path, FILE_READ);
        if (!f) continue;
        uint32_t size = f.size();
        f.close();
        if (size != r.size) continue;
        kept.push_back(r);
    }
    gRecords.swap(kept);

    JsonDocument doc;
    JsonArray    arr = doc.to<JsonArray>();
    for (const auto& r : gRecords) {
        JsonObject o = arr.add<JsonObject>();
        o["n"] = r.name;
        o["s"] = r.size;
    }
    File out = LittleFS.open(RECORD_FILE, FILE_WRITE);
    if (!out) {
        Serial.println("[LOGSYNC] Warning: could not write " + String(RECORD_FILE));
        return;
    }
    serializeJson(doc, out);
    out.close();
}

static void loadRecords() {
    gRecords.clear();
    File in = LittleFS.open(RECORD_FILE, FILE_READ);
    if (in) {
        JsonDocument doc;
        if (deserializeJson(doc, in) == DeserializationError::Ok && doc.is<JsonArray>()) {
            for (JsonObject o : doc.as<JsonArray>()) {
                const char* n = o["n"];
                if (!n) continue;
                gRecords.push_back({ String(n), o["s"].as<uint32_t>() });
            }
        }
        in.close();
    }
    // A record that outlived its file is meaningless and, worse, could match a
    // later log by name -- clear those out before anything consults them.
    pruneAndSave();
}

bool isUploaded(const char* path, uint32_t size) {
    const char* name = basenameOf(path);
    for (const auto& r : gRecords) {
        if (r.size == size && r.name == name) return true;
    }
    return false;
}

void markUploaded(const char* path) {
    const char* name = basenameOf(path);
    File f = LittleFS.open(path, FILE_READ);
    if (!f) return;
    uint32_t size = f.size();
    f.close();

    for (auto& r : gRecords) {
        if (r.name == name) {
            r.size = size;
            pruneAndSave();
            return;
        }
    }
    gRecords.push_back({ String(name), size });
    pruneAndSave();
}

// ---------------------------------------------------------------------------
// Pass state
// ---------------------------------------------------------------------------

enum class Phase : uint8_t {
    IDLE,      // nothing to do, or waiting out a backoff
    ANNOUNCE,  // isUploading() true, letting a NavPacket reach the display
    UPLOADING, // one file per update() call
};

static Phase                gPhase       = Phase::IDLE;
static std::vector<String>  gPending;    // basenames still to send this pass
static uint8_t              gDone        = 0;
static uint8_t              gTotal       = 0;
static uint32_t             gAnnouncedMs = 0;
static uint32_t             gLastScanMs  = 0;
static uint32_t             gRetryAtMs   = 0;
static bool                 gWasConnected = false;
static String               gLastError;

// Collect the basenames of closed, not-yet-uploaded logs into `out`.
static void scanPending(std::vector<String>& out) {
    out.clear();
    const char* openPath = logging::currentPath();

    File dir = LittleFS.open(LOG_DIR);
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f) {
        String name = f.name();
        uint32_t size = f.size();
        f.close();
        f = dir.openNextFile();

        if (!name.endsWith(".csv")) continue;
        char path[logging::LOG_PATH_MAX];
        buildPath(path, sizeof(path), name.c_str());
        if (strcmp(path, openPath) == 0) continue;  // still being written
        if (size == 0) continue;

        bool known = false;
        for (const auto& r : gRecords) {
            if (r.size == size && r.name == name) { known = true; break; }
        }
        if (!known) out.push_back(name);
    }
    dir.close();
}

void init() {
    LittleFS.mkdir(LOG_DIR);
    loadRecords();
    Serial.printf("[LOGSYNC] Init OK, %u log(s) already uploaded\n",
                  (unsigned)gRecords.size());
}

void update() {
    uint32_t now = millis();

    bool connected = wifi::isStaConnected();
    if (!connected) {
        // Drop any half-finished pass; it restarts from a fresh scan on the
        // next association rather than resuming against a stale file list.
        gWasConnected = false;
        gPhase  = Phase::IDLE;
        gDone   = 0;
        gTotal  = 0;
        gPending.clear();
        return;
    }

    bool justConnected = !gWasConnected;
    gWasConnected = true;
    if (justConnected) {
        // A new association is the diver asking for this. Scan now and forget
        // any backoff left over from the last network.
        gLastScanMs = 0;
        gRetryAtMs  = 0;
    }

    if (!cloud::isAuthorized()) return;  // unlinked unit: nothing to upload to

    switch (gPhase) {
        case Phase::IDLE: {
            if (gRetryAtMs && now < gRetryAtMs) return;
            if (gLastScanMs && now - gLastScanMs < RESCAN_INTERVAL_MS) return;
            gLastScanMs = now ? now : 1;
            scanPending(gPending);
            if (gPending.empty()) return;
            gTotal       = (uint8_t)(gPending.size() > 255 ? 255 : gPending.size());
            gDone        = 0;
            gAnnouncedMs = now;
            gPhase       = Phase::ANNOUNCE;
            Serial.printf("[LOGSYNC] %u log(s) to upload\n", (unsigned)gPending.size());
            return;
        }

        case Phase::ANNOUNCE:
            if (now - gAnnouncedMs < ANNOUNCE_MS) return;
            gPhase = Phase::UPLOADING;
            return;

        case Phase::UPLOADING: {
            if (gPending.empty()) {
                Serial.printf("[LOGSYNC] Pass complete, %u uploaded\n", (unsigned)gDone);
                gPhase = Phase::IDLE;
                gDone  = 0;
                gTotal = 0;
                return;
            }
            String name = gPending.front();
            gPending.erase(gPending.begin());

            char path[logging::LOG_PATH_MAX];
            buildPath(path, sizeof(path), name.c_str());

            String err;
            // Blocking; stalls this loop for up to CLOUD_HTTP_TIMEOUT_MS. A
            // 409 (server already holds these exact bytes) counts as success
            // inside uploadBackup, which is what makes a lost record cheap:
            // re-uploading wastes bandwidth, it never duplicates a dive.
            if (cloud::uploadBackup("dive_log", path, err)) {
                markUploaded(path);
                if (gDone < 255) gDone++;
                gLastError = "";
                Serial.printf("[LOGSYNC] Uploaded %s\n", path);
            } else {
                gLastError = err;
                Serial.printf("[LOGSYNC] Upload of %s failed: %s\n", path, err.c_str());
                // Abandon the rest of the pass. Whatever stopped this file --
                // no route, a stale token (which surfaces as the same http -3
                // a dropped connection does), a server error -- will almost
                // certainly stop the next one too, and retrying each in turn
                // means a stall of CLOUD_HTTP_TIMEOUT_MS per file with the
                // display frozen for all of it.
                gPending.clear();
                gRetryAtMs = now + RETRY_BACKOFF_MS;
                if (gRetryAtMs == 0) gRetryAtMs = 1;
            }
            return;
        }
    }
}

bool    isUploading() { return gPhase != Phase::IDLE; }
uint8_t doneCount()   { return isUploading() ? gDone : 0; }
uint8_t totalCount()  { return isUploading() ? gTotal : 0; }

String statusJson() {
    // Counted fresh into a scratch list rather than read off the pass, so the
    // page is right when nothing is in flight and harmless when something is.
    std::vector<String> pending;
    scanPending(pending);
    JsonDocument doc;
    doc["pending"]   = pending.size();
    doc["uploading"] = isUploading();
    doc["done"]      = doneCount();
    doc["total"]     = totalCount();
    doc["error"]     = gLastError;
    JsonArray arr = doc["uploaded"].to<JsonArray>();
    for (const auto& r : gRecords) arr.add(String(LOG_DIR) + "/" + r.name);
    String out;
    serializeJson(doc, out);
    return out;
}

}  // namespace log_sync
