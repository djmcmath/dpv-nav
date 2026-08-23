#include "cloud_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>

#include "../config.h"

namespace cloud {

// ---------------------------------------------------------------------------
// TLS root CA
// ---------------------------------------------------------------------------
//
// divemap.diverdaniel.com's cert chain used to terminate directly at ISRG
// Root X1. That stopped being true once Let's Encrypt's ACME default profile
// cut over to the new "Generation Y" hierarchy on 2026-05-13: certs issued
// after that (ours renewed 2026-08-11) chain leaf -> YE1 -> ISRG Root YE ->
// ISRG Root X2 -> ISRG Root X1, cross-signed all the way down for compat
// with trust stores that don't know about YE/X2 yet. See
// https://letsencrypt.org/2025/11/24/gen-y-hierarchy.
//
// Pinning all three self-signed roots below (YE, X2, X1) rather than just X1
// lets BearSSL terminate the chain walk at whichever one it reaches first --
// two hops for the current cert instead of four -- and keeps this working
// across Let's Encrypt's next couple of root transitions without a firmware
// update each time. rootCaConfigured() below still fails closed (returns an
// error rather than falling back to WiFiClientSecure::setInsecure()) if this
// is ever emptied out.
//
// Self-signed PEMs fetched directly from:
//   https://letsencrypt.org/certs/gen-y/root-ye.pem
//   https://letsencrypt.org/certs/isrg-root-x2.pem
//   https://letsencrypt.org/certs/isrgrootx1.pem
static const char* CLOUD_ROOT_CA_PEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIB2TCCAWCgAwIBAgIRAKQCa6LvbHwg1AR+XmWmk4AwCgYIKoZIzj0EAwMwLjEL
MAkGA1UEBhMCVVMxDTALBgNVBAoTBElTUkcxEDAOBgNVBAMTB1Jvb3QgWUUwHhcN
MjUwOTAzMDAwMDAwWhcNNDUwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzENMAsG
A1UEChMESVNSRzEQMA4GA1UEAxMHUm9vdCBZRTB2MBAGByqGSM49AgEGBSuBBAAi
A2IABDwS/6vhrcVqcbBo+wgdI3fwn9x7DNJJOY/lTOti0vkwuRN87RhEhTH17E7X
yFjWsPYhIPt/wzOqxTd2b+4ZJNy9ID04YywF9U5zasDVyGSNErVNtz8uSGh5izW8
7j77GaNCMEAwDgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0O
BBYEFKPIJlqOoUzQNWP8myPIOq5W809WMAoGCCqGSM49BAMDA2cAMGQCMHhMr8N9
LdL1VQKs9BdV81r76eXRB6mtjuNjzk6/lBsPNToWLTDzGYgtQKO1jl63uAIwGV7m
onyF377c+MM1oqVNs17sgu7F9YKZwgLmVbeOMDbKAXHtKMDLbiGllCcs8f47
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw
CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg
R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00
MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT
ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw
EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW
+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9
ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T
AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI
zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW
tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1
/q4AaOeMSQ+2b1tbFfLn
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";

static bool rootCaConfigured() {
    return CLOUD_ROOT_CA_PEM != nullptr && strlen(CLOUD_ROOT_CA_PEM) > 0;
}

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
    if (!rootCaConfigured()) {
        errorOut = "cloud TLS root CA not configured";
        return false;
    }

    WiFiClientSecure client;
    client.setCACert(CLOUD_ROOT_CA_PEM);
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

    if (!rootCaConfigured()) {
        gAuthLastError  = "cloud TLS root CA not configured";
        gAuthPollStatus = AuthPollStatus::ERROR;
        return;
    }

    WiFiClientSecure pollClient;
    pollClient.setCACert(CLOUD_ROOT_CA_PEM);
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

    if (!rootCaConfigured()) {
        result.errorMessage = "cloud TLS root CA not configured";
        return result;
    }

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
        client.setCACert(CLOUD_ROOT_CA_PEM);
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
        client.setCACert(CLOUD_ROOT_CA_PEM);
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
    if (!rootCaConfigured()) return false;

    String token = loadToken();
    if (token.length() == 0) return false;

    WiFiClientSecure client;
    client.setCACert(CLOUD_ROOT_CA_PEM);
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

    if (!rootCaConfigured()) {
        errorOut = "cloud TLS root CA not configured";
        return false;
    }
    String token = loadToken();
    if (token.length() == 0) {
        errorOut = "device not authorized -- run cloud setup first";
        return false;
    }

    WiFiClientSecure client;
    client.setCACert(CLOUD_ROOT_CA_PEM);
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

bool downloadUpload(const String& uploadId, const char* outputPath, String& errorOut) {
    if (!rootCaConfigured()) {
        errorOut = "cloud TLS root CA not configured";
        return false;
    }
    String token = loadToken();
    if (token.length() == 0) {
        errorOut = "device not authorized -- run cloud setup first";
        return false;
    }

    WiFiClientSecure client;
    client.setCACert(CLOUD_ROOT_CA_PEM);
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
    if (!rootCaConfigured()) {
        errorOut = "cloud TLS root CA not configured";
        return false;
    }
    String token = loadToken();
    if (token.length() == 0) {
        errorOut = "device not authorized -- run cloud setup first";
        return false;
    }

    WiFiClientSecure client;
    client.setCACert(CLOUD_ROOT_CA_PEM);
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
    if (!rootCaConfigured()) {
        errorOut = "cloud TLS root CA not configured";
        return false;
    }
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
    client.setCACert(CLOUD_ROOT_CA_PEM);
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
    if (!rootCaConfigured()) {
        errorOut = "cloud TLS root CA not configured";
        return false;
    }
    String token = loadToken();
    if (token.length() == 0) {
        errorOut = "device not authorized -- run cloud setup first";
        return false;
    }

    WiFiClientSecure client;
    client.setCACert(CLOUD_ROOT_CA_PEM);
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
