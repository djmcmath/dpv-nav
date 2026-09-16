#pragma once

#include <Arduino.h>

// Reloads calibration JSON files from LittleFS into the in-memory state used
// by nav processing, without the boot-time blocking-sweep fallbacks
// loadCalibration() has. Exposed so net/cal_sync.cpp can apply a freshly
// staged cloud result (or a restored backup) the same way the existing
// "Reload Cal Files" web action does, without duplicating the
// two-stage-chain / legacy / per-type loading logic here.
void reloadCalibrationFiles();

// Result of retryCalibrationUpload() -- a summary of a calibration-CSV
// upload+fit round trip, suitable for serializing straight into an HTTP
// response.
struct CalRetryResult {
    bool   ok = false;            // fit succeeded
    bool   installable = false;   // false for gap-fill: no on-device accept/reject
    String qualityBand;
    float  rmsPct = 0.0f;
    String recommendation;
    String calibrationId;
    String error;                 // set whenever !ok
};

// Re-runs the cloud upload+fit+notify flow (same one a live CAL-menu run
// performs) for a raw calibration sample CSV already sitting on LittleFS --
// e.g. one left behind by an automatic upload that failed at collection
// time. `filename` must exactly match one of the known sample-CSV paths
// (baseline/mounted/gap-fill/hdg); anything else, a live CAL already in
// progress, or no WiFi connection returns ok=false with `error` set.
// Exposed so net/web_server.cpp's tern.local "Upload to cloud" retry action
// can trigger it without reaching into nav_main.cpp's private cal-tracking
// state.
CalRetryResult retryCalibrationUpload(const char* filename);

// Why a firmware update must not start right now, or nullptr if it may.
// Covers what only nav_main.cpp can see: dive mode, depth, logging, a running
// calibration or current hold. (WiFi connectivity is net/ota.cpp's own check.)
const char* otaBlockedReason();

// FW_VERSION the display reported in its boot LINK_HELLO; "" if it never did
// (link failed, or a pre-OTA display build that doesn't send one).
const char* displayFwVersion();

// Clears it, so the next LINK_HELLO decides -- ota.cpp uses this to confirm a
// just-updated display restarted on the new version rather than rolling back.
void forgetDisplayFwVersion();
