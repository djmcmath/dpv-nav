#include "cloud_client.h"

#include <ArduinoJson.h>
#include <string.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>

#include "../config.h"

namespace cloud {

// ---------------------------------------------------------------------------
// TLS
// ---------------------------------------------------------------------------
//
// No CA pinning: every WiFiClientSecure below calls setInsecure(), so the
// server's certificate chain is never validated. Traffic is still TLS
// (encrypted in transit), just not authenticated against a trust anchor.
// This used to pin divemap.diverdaniel.com's root CA as a compiled-in PEM,
// which meant every CA rotation needed a USB reflash of every unit in the
// field -- it broke for real on 2026-08-22 when Let's Encrypt moved to a new
// root hierarchy. Deliberately not replaced with a refreshable-CA scheme:
// the payloads here are dive calibration data, not anything sensitive, and
// the actual security boundary is the bearer token issued by the device-auth
// flow below (validated server-side against a DB-stored hash, independent of
// TLS validation) -- see docs/cloud-calibration-plan.md for the full
// reasoning.

// ---------------------------------------------------------------------------
// Token persistence (NVS, mirrors util/nvs_state.cpp's Preferences pattern)
// ---------------------------------------------------------------------------

static constexpr char AUTH_NS[] = "cloud_auth";

static String loadToken() {
    Preferences prefs;
    if (!prefs.begin(AUTH_NS, /*readOnly=*/true)) return "";
    String token = prefs.getString("token", "");
    prefs.end();
    return token;
}

static void saveToken(const String& token) {
    Preferences prefs;
    if (!prefs.begin(AUTH_NS, /*readOnly=*/false)) return;
    prefs.putString("token", token);
    prefs.end();
}

bool isAuthorized() {
    return loadToken().length() > 0;
}

// ---------------------------------------------------------------------------
// Shared request helpers
// ---------------------------------------------------------------------------

static String apiUrl(const char* path) {
    String url = "https://";
    url += CLOUD_API_HOST;
    url += path;
    return url;
}

// Extracts the backend's structured error body -- {"error": "<category>",
// "status": ..., "details": "<specific reason>"} (see errors.rs). "details"
// is what's actionable ("file not found: ...", "insufficient_samples", etc);
// "error" alone is just the HTTP status category ("Internal Server Error")
// and isn't worth showing on its own. Falls back to "error", then the raw
// body, if "details" is absent (e.g. a response not routed through errors.rs).
static String extractError(const String& body, int httpCode) {
    if (body.length() > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, body) == DeserializationError::Ok) {
            if (doc["details"].is<const char*>()) {
                return String((const char*)doc["details"]);
            }
            if (doc["error"].is<const char*>()) {
                return String((const char*)doc["error"]);
            }
        }
        return body;
    }
    return "request failed (HTTP " + String(httpCode) + ")";
}

// ---------------------------------------------------------------------------
// Device-auth bootstrap (RFC 8628)
// ---------------------------------------------------------------------------

bool beginAuthorize(String& deviceCodeOut, String& userCodeOut, String& errorOut) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.setTimeout(CLOUD_HTTP_TIMEOUT_MS);

    if (!https.begin(client, apiUrl("/api/device/authorize"))) {
        errorOut = "could not start connection";
        return false;
    }
    https.addHeader("Content-Type", "application/json");
    int code = https.POST("{}");
    String body = https.getString();
    https.end();

    if (code != 200) {
        errorOut = extractError(body, code);
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        errorOut = "malformed authorize response";
        return false;
    }

    deviceCodeOut = String((const char*)doc["device_code"]);
    userCodeOut = String((const char*)doc["user_code"]);

    if (deviceCodeOut.length() == 0) {
        errorOut = "malformed authorize response (no device_code)";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Non-blocking account-link poll state machine (mirrors wifi::update()).
// ---------------------------------------------------------------------------

static AuthPollStatus gAuthPollStatus    = AuthPollStatus::IDLE;
static String         gAuthDeviceCode;
static String         gAuthLastError;
static uint32_t       gAuthPollStartMs   = 0;
static uint32_t       gAuthNextPollDueMs = 0;

void startAuthorizePoll(const String& deviceCode) {
    gAuthDeviceCode    = deviceCode;
    gAuthLastError     = "";
    gAuthPollStartMs   = millis();
    gAuthNextPollDueMs = gAuthPollStartMs;  // poll on the first update() tick
    gAuthPollStatus    = AuthPollStatus::POLLING;
}

void cancelAuthorizePoll() {
    gAuthPollStatus = AuthPollStatus::IDLE;
    gAuthDeviceCode = "";
}

AuthPollStatus getAuthorizePollStatus() {
    return gAuthPollStatus;
}

String lastAuthorizeError() {
    return gAuthLastError;
}

void updateAuthorizePoll() {
    if (gAuthPollStatus != AuthPollStatus::POLLING) return;

    uint32_t now = millis();
    if (now - gAuthPollStartMs >= CLOUD_AUTH_POLL_TIMEOUT_MS) {
        gAuthPollStatus = AuthPollStatus::EXPIRED;
        return;
    }
    if (now < gAuthNextPollDueMs) return;

    // Always polls at the local default interval rather than honoring the
    // server's advisory `interval` field -- simpler, and the server's
    // "slow_down" response already makes a shorter local interval
    // self-correcting (we just keep polling).
    gAuthNextPollDueMs = now + CLOUD_AUTH_POLL_INTERVAL_MS;

    WiFiClientSecure pollClient;
    pollClient.setInsecure();
    HTTPClient pollHttps;
    pollHttps.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
    if (!pollHttps.begin(pollClient, apiUrl("/api/device/token"))) {
        gAuthLastError  = "could not start connection";
        gAuthPollStatus = AuthPollStatus::ERROR;
        return;
    }
    pollHttps.addHeader("Content-Type", "application/json");

    JsonDocument reqDoc;
    reqDoc["device_code"] = gAuthDeviceCode;
    String reqBody;
    serializeJson(reqDoc, reqBody);

    int pollCode = pollHttps.POST(reqBody);
    String pollBody = pollHttps.getString();
    pollHttps.end();

    if (pollCode == 200) {
        JsonDocument respDoc;
        if (deserializeJson(respDoc, pollBody) != DeserializationError::Ok ||
            !respDoc["access_token"].is<const char*>()) {
            gAuthLastError  = "malformed token response";
            gAuthPollStatus = AuthPollStatus::ERROR;
            return;
        }
        saveToken(String((const char*)respDoc["access_token"]));
        gAuthPollStatus = AuthPollStatus::APPROVED;
        return;
    }

    JsonDocument errDoc;
    String pollError;
    if (deserializeJson(errDoc, pollBody) == DeserializationError::Ok &&
        errDoc["error"].is<const char*>()) {
        pollError = String((const char*)errDoc["error"]);
    }

    if (pollError == "authorization_pending" || pollError == "slow_down") {
        return;  // keep polling
    }
    if (pollError == "access_denied") {
        gAuthPollStatus = AuthPollStatus::DENIED;
        return;
    }
    if (pollError == "expired_token") {
        gAuthPollStatus = AuthPollStatus::EXPIRED;
        return;
    }

    gAuthLastError  = pollError.length() > 0 ? pollError : "authorization failed";
    gAuthPollStatus = AuthPollStatus::ERROR;
}

// ---------------------------------------------------------------------------
// Calibration upload + fit
// ---------------------------------------------------------------------------

static const char* basenameOf(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

CalibrationResult runCalibrationUpload(const char* mode, const char* csvPath, const char* outputPath) {
    CalibrationResult result;

    String token = loadToken();
    if (token.length() == 0) {
        result.errorMessage = "device not authorized -- run cloud setup first";
        return result;
    }

    // Step 1: upload the raw CSV, streamed directly from LittleFS rather than
    // read into a single heap buffer first. The buffer approach this replaced
    // (`new uint8_t[len]`) crashed hard once a collection got large enough --
    // ~1000 samples (~85KB CSV) reliably failed a single contiguous
    // allocation against the WiFi-stack-constrained heap, and the resulting
    // uncaught bad_alloc took the whole device down (panic -> reboot) instead
    // of failing gracefully. Streaming avoids needing that allocation at all.
    // A 409 (duplicate, same content already uploaded) is not a failure --
    // the backend hands back the existing upload's id, which works exactly
    // as well for the calibrate call.
    String uploadId;
    {
        File csvFile = LittleFS.open(csvPath, FILE_READ);
        if (!csvFile) {
            result.errorMessage = "could not read samples file";
            return result;
        }
        size_t csvLen = csvFile.size();

        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient https;
        https.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
        if (!https.begin(client, apiUrl("/api/device/uploads"))) {
            csvFile.close();
            result.errorMessage = "could not start connection";
            return result;
        }
        https.addHeader("Authorization", "Bearer " + token);
        https.addHeader("X-Upload-Kind", "calibration_raw");
        https.addHeader("X-Upload-Filename", basenameOf(csvPath));
        https.addHeader("Content-Type", "application/octet-stream");

        int code = https.sendRequest("POST", &csvFile, csvLen);
        csvFile.close();
        String body = https.getString();
        https.end();

        if (code != 201 && code != 409) {
            result.errorMessage = extractError(body, code);
            return result;
        }

        JsonDocument doc;
        if (deserializeJson(doc, body) != DeserializationError::Ok) {
            result.errorMessage = "malformed upload response";
            return result;
        }
        uploadId = (code == 409) ? String((const char*)doc["upload_id"])
                                  : String((const char*)doc["id"]);
        if (uploadId.length() == 0) {
            result.errorMessage = "malformed upload response (no id)";
            return result;
        }
    }

    // Step 2: trigger the fit.
    JsonDocument calJson;  // holds the response's cal_json sub-object for step 3
    {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient https;
        https.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
        String path = "/api/device/uploads/" + uploadId + "/calibrate";
        if (!https.begin(client, apiUrl(path.c_str()))) {
            result.errorMessage = "could not start connection";
            return result;
        }
        https.addHeader("Authorization", "Bearer " + token);
        https.addHeader("Content-Type", "application/json");

        JsonDocument reqDoc;
        reqDoc["mode"] = mode;
        String reqBody;
        serializeJson(reqDoc, reqBody);

        int code = https.POST(reqBody);
        String body = https.getString();
        https.end();

        if (code != 200) {
            result.errorMessage = extractError(body, code);
            return result;
        }

        JsonDocument doc;
        if (deserializeJson(doc, body) != DeserializationError::Ok) {
            result.errorMessage = "malformed calibrate response";
            return result;
        }

        result.calibrationId = String((const char*)doc["calibration_id"]);
        result.qualityBand   = String((const char*)doc["quality_band"]);
        result.rmsPct        = doc["rms_pct"] | 0.0f;
        result.recommendation = String((const char*)doc["recommendation"]);
        result.coverageGaps  = doc["coverage_gaps"] | -1;
        calJson.set(doc["cal_json"]);
    }

    // Step 3: write the fitted result to LittleFS in the shape
    // storage::loadMagCalibration already reads.
    File out = LittleFS.open(outputPath, FILE_WRITE);
    if (!out) {
        result.errorMessage = "fit succeeded but could not write " + String(outputPath);
        return result;
    }
    serializeJson(calJson, out);
    out.close();

    result.ok = true;
    return result;
}

bool respondToCalibration(const String& calibrationId, bool accepted) {
    String token = loadToken();
    if (token.length() == 0) return false;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
    String path = "/api/calibrations/" + calibrationId;
    if (!https.begin(client, apiUrl(path.c_str()))) return false;
    https.addHeader("Authorization", "Bearer " + token);
    https.addHeader("Content-Type", "application/json");

    JsonDocument reqDoc;
    reqDoc["accepted"] = accepted;
    String reqBody;
    serializeJson(reqDoc, reqBody);

    int code = https.PATCH(reqBody);
    https.end();

    return code == 200;
}

// ---------------------------------------------------------------------------
// Install sync + archival backup (divemap's calibration-install-sync-plan.md)
// ---------------------------------------------------------------------------

bool fetchCalibrationStatus(std::vector<CalStatusEntry>& outEntries, String& errorOut) {
    outEntries.clear();

    String token = loadToken();
    if (token.length() == 0) {
        errorOut = "device not authorized -- run cloud setup first";
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
    if (!https.begin(client, apiUrl("/api/device/calibrations/status"))) {
        errorOut = "could not start connection";
        return false;
    }
    https.addHeader("Authorization", "Bearer " + token);

    int code = https.GET();
    String body = https.getString();
    https.end();

    if (code != 200) {
        errorOut = extractError(body, code);
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok || !doc.is<JsonArray>()) {
        errorOut = "malformed status response";
        return false;
    }
    for (JsonObject entry : doc.as<JsonArray>()) {
        CalStatusEntry e;
        e.mode           = String((const char*)(entry["mode"] | ""));
        e.calibrationId  = String((const char*)(entry["calibration_id"] | ""));
        e.resultUploadId = String((const char*)(entry["result_upload_id"] | ""));
        if (e.mode.length() > 0 && e.calibrationId.length() > 0 && e.resultUploadId.length() > 0) {
            outEntries.push_back(e);
        }
    }
    return true;
}

bool fetchCalTargets(uint8_t outGrid[60], bool& degradedOut,
                      uint8_t outRollGrid[60][MAG_CAL_ROLL_SECTORS], bool& hasRollOut,
                      String& errorOut) {
    memset(outGrid, 0, 60);
    degradedOut = false;
    memset(outRollGrid, 0, 60 * MAG_CAL_ROLL_SECTORS);
    hasRollOut = false;

    String token = loadToken();
    if (token.length() == 0) {
        errorOut = "device not authorized -- run cloud setup first";
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
    if (!https.begin(client, apiUrl("/api/device/calibrations/targets"))) {
        errorOut = "could not start connection";
        return false;
    }
    https.addHeader("Authorization", "Bearer " + token);

    int code = https.GET();
    String body = https.getString();
    https.end();

    if (code == 404) {
        // Not necessarily an alarm -- covers three distinct backend reasons
        // (no accepted baseline at all / accepted baseline has no coverage
        // field / coverage field present but malformed), which used to get
        // collapsed into one hardcoded "no graded baseline to target yet"
        // here regardless of which one it actually was. Pass the backend's
        // own `details` message through instead so the three are
        // distinguishable from the diver-visible result text (see
        // handlers/device.rs's calibration_targets for the exact strings).
        errorOut = extractError(body, code);
        return false;
    }
    if (code != 200) {
        errorOut = extractError(body, code);
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        errorOut = "malformed targets response";
        return false;
    }

    // The grid must be exactly the size the device renders. A short or long
    // array means the server and firmware disagree about the grid shape, and
    // guessing which cells the values belong to would send the diver to the
    // wrong orientations -- refuse instead.
    JsonArray grid = doc["grid"];
    if (grid.isNull() || grid.size() != 60) {
        errorOut = "targets grid was not 60 cells";
        return false;
    }
    const int bands   = doc["elev_bands"]  | 0;
    const int sectors = doc["hdg_sectors"] | 0;
    if (bands != MAG_CAL_BASELINE_ELEV_BANDS || sectors != MAG_CAL_BASELINE_HDG_SECTORS) {
        errorOut = "targets grid shape does not match this firmware";
        return false;
    }

    for (int i = 0; i < 60; i++) {
        int v = grid[i] | 0;
        // Clamp unknown codes to "ok" -- an unfamiliar status from a newer
        // processor should look uninteresting, not send the diver somewhere.
        outGrid[i] = (v >= 0 && v <= 3) ? (uint8_t)v : (uint8_t)0;
    }
    degradedOut = doc["degraded"] | false;

    // roll_grid is optional (absent for pre-roll or degraded calibrations --
    // see the server's `targets_grid_from_coverage`/`roll_grid_from_coverage`
    // in dive-map's device.rs). Same refuse-rather-than-guess discipline as
    // the flat grid above: a shape that doesn't match this firmware's own
    // MAG_CAL_ROLL_SECTORS is treated as "no roll data", not as roll data
    // read into the wrong slots.
    JsonArray rollGrid = doc["roll_grid"];
    const int rollSectors = doc["roll_sectors"] | 0;
    if (!rollGrid.isNull() && rollSectors == MAG_CAL_ROLL_SECTORS &&
        rollGrid.size() == 60 * (size_t)MAG_CAL_ROLL_SECTORS) {
        for (int i = 0; i < 60; i++) {
            for (int k = 0; k < MAG_CAL_ROLL_SECTORS; k++) {
                int v = rollGrid[i * MAG_CAL_ROLL_SECTORS + k] | 0;
                outRollGrid[i][k] = (v >= 0 && v <= 3) ? (uint8_t)v : (uint8_t)0;
            }
        }
        hasRollOut = true;
    }
    return true;
}

bool downloadUpload(const String& uploadId, const char* outputPath, String& errorOut) {
    String token = loadToken();
    if (token.length() == 0) {
        errorOut = "device not authorized -- run cloud setup first";
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
    String path = "/api/device/uploads/" + uploadId + "/raw";
    if (!https.begin(client, apiUrl(path.c_str()))) {
        errorOut = "could not start connection";
        return false;
    }
    https.addHeader("Authorization", "Bearer " + token);

    int code = https.GET();
    if (code != 200) {
        errorOut = extractError(https.getString(), code);
        https.end();
        return false;
    }

    // Streamed straight to LittleFS, same reasoning as runCalibrationUpload's
    // CSV upload: avoid a single contiguous heap buffer for what could be a
    // sizeable file.
    File out = LittleFS.open(outputPath, FILE_WRITE);
    if (!out) {
        https.end();
        errorOut = "could not open " + String(outputPath) + " for write";
        return false;
    }
    https.writeToStream(&out);
    out.close();
    https.end();
    return true;
}

bool confirmInstalled(const String& calibrationId, String& errorOut) {
    String token = loadToken();
    if (token.length() == 0) {
        errorOut = "device not authorized -- run cloud setup first";
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
    String path = "/api/device/calibrations/" + calibrationId + "/confirm-installed";
    if (!https.begin(client, apiUrl(path.c_str()))) {
        errorOut = "could not start connection";
        return false;
    }
    https.addHeader("Authorization", "Bearer " + token);
    https.addHeader("Content-Type", "application/json");

    int code = https.POST("");
    String body = https.getString();
    https.end();

    if (code != 200) {
        errorOut = extractError(body, code);
        return false;
    }
    return true;
}

bool uploadBackup(const char* kind, const char* filePath, String& errorOut) {
    String token = loadToken();
    if (token.length() == 0) {
        errorOut = "device not authorized -- run cloud setup first";
        return false;
    }

    File f = LittleFS.open(filePath, FILE_READ);
    if (!f) {
        errorOut = "could not read " + String(filePath);
        return false;
    }
    size_t len = f.size();

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
    if (!https.begin(client, apiUrl("/api/device/uploads"))) {
        f.close();
        errorOut = "could not start connection";
        return false;
    }
    https.addHeader("Authorization", "Bearer " + token);
    https.addHeader("X-Upload-Kind", kind);
    https.addHeader("X-Upload-Filename", basenameOf(filePath));
    https.addHeader("Content-Type", "application/octet-stream");

    int code = https.sendRequest("POST", &f, len);
    f.close();
    String body = https.getString();
    https.end();

    if (code != 201 && code != 409) {
        errorOut = extractError(body, code);
        return false;
    }
    return true;
}

bool fetchLatestBackup(const char* kind, const char* outputPath, String& errorOut) {
    String token = loadToken();
    if (token.length() == 0) {
        errorOut = "device not authorized -- run cloud setup first";
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
    String path = "/api/device/uploads?kind=" + String(kind) + "&latest=true";
    if (!https.begin(client, apiUrl(path.c_str()))) {
        errorOut = "could not start connection";
        return false;
    }
    https.addHeader("Authorization", "Bearer " + token);

    int code = https.GET();
    String body = https.getString();
    https.end();

    if (code != 200) {
        errorOut = extractError(body, code);
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok || !doc.is<JsonArray>()) {
        errorOut = "malformed uploads response";
        return false;
    }
    JsonArray arr = doc.as<JsonArray>();
    if (arr.size() == 0) {
        errorOut = "no backup found";
        return false;
    }
    String uploadId = String((const char*)(arr[0]["id"] | ""));
    if (uploadId.length() == 0) {
        errorOut = "malformed uploads response (no id)";
        return false;
    }
    return downloadUpload(uploadId, outputPath, errorOut);
}

}  // namespace cloud
