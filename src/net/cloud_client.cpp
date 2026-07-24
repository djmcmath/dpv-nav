#include "cloud_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>

#include <memory>

#include "../config.h"

namespace cloud {

// ---------------------------------------------------------------------------
// TLS root CA
// ---------------------------------------------------------------------------
//
// ISRG Root X1 (Let's Encrypt) -- divemap.diverdaniel.com's cert chain
// terminates here. Fetched directly from
// https://letsencrypt.org/certs/isrgrootx1.pem, not from the live chain
// (which usually omits the self-signed root itself). rootCaConfigured()
// below still fails closed (returns an error rather than falling back to
// WiFiClientSecure::setInsecure()) if this is ever emptied out again.
static const char* CLOUD_ROOT_CA_PEM = R"EOF(
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

// Extracts {"error": "..."} from a JSON error body, falling back to the raw
// body (or a generic message) if it isn't in that shape -- matches the
// backend's structured-error convention (see errors.rs / calibration-processor).
static String extractError(const String& body, int httpCode) {
    if (body.length() > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, body) == DeserializationError::Ok && doc["error"].is<const char*>()) {
            return String((const char*)doc["error"]);
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

static bool readFileToBuffer(const char* path, std::unique_ptr<uint8_t[]>& buf, size_t& len) {
    File f = LittleFS.open(path, FILE_READ);
    if (!f) return false;
    len = f.size();
    buf.reset(new uint8_t[len]);
    size_t read = f.read(buf.get(), len);
    f.close();
    return read == len;
}

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

    std::unique_ptr<uint8_t[]> csvBuf;
    size_t csvLen = 0;
    if (!readFileToBuffer(csvPath, csvBuf, csvLen) || csvLen == 0) {
        result.errorMessage = "could not read samples file";
        return result;
    }

    // Step 1: upload the raw CSV. A 409 (duplicate, same content already
    // uploaded) is not a failure -- the backend hands back the existing
    // upload's id, which works exactly as well for the calibrate call.
    String uploadId;
    {
        WiFiClientSecure client;
        client.setCACert(CLOUD_ROOT_CA_PEM);
        HTTPClient https;
        https.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
        if (!https.begin(client, apiUrl("/api/device/uploads"))) {
            result.errorMessage = "could not start connection";
            return result;
        }
        https.addHeader("Authorization", "Bearer " + token);
        https.addHeader("X-Upload-Kind", "calibration_raw");
        https.addHeader("X-Upload-Filename", basenameOf(csvPath));
        https.addHeader("Content-Type", "application/octet-stream");

        int code = https.POST(csvBuf.get(), csvLen);
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

}  // namespace cloud
