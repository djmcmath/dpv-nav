#include "cloud_client.h"

#include <ArduinoJson.h>
#include <string.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <time.h>

#include "../config.h"
#include "../version.h"
#include "wifi_manager.h"

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

// ---------------------------------------------------------------------------
// Streamed file upload (POST /api/device/uploads)
// ---------------------------------------------------------------------------
//
// Every streamed upload goes through here so the two callers (calibration raw
// CSVs and cal-backup blobs) share one diagnostic + retry policy.
//
// Why this exists: HTTPClient collapses four *unrelated* body-send failures
// into the single code -3 (HTTPC_ERROR_SEND_PAYLOAD_FAILED) -- a short
// TCP write, a write error, the server closing the connection early, and a
// bytesWritten != Content-Length mismatch. It distinguishes them itself via
// log_d()/log_e(), but those are compiled out at CORE_DEBUG_LEVEL=0 (our
// default build), so the device reports a bare "http -3" with no context at
// all. To recover a *specific* -3 path, build the nav env once with
// -DCORE_DEBUG_LEVEL=4; the numbers logged below are what's available
// without that.
//
// For the record, because it cost a week: the -3 this unit kept hitting was
// the *first* of those -- a short write, from send_ssl_data() giving up after
// socket_timeout while the TCP send window stayed shut. Not an early close, not
// a length mismatch, not the server. The fix was to stop leaving socket_timeout
// at HTTPClient's 5 s default; see CLOUD_UPLOAD_CONNECT_TIMEOUT_MS in config.h
// for the full path from that constant to mbedtls. `consumed` below is still
// worth keeping: it is what distinguishes a mid-body stall (consumed << size)
// from a tail-end failure (consumed ~= size), and those have different causes.
//
// Sentinel codes below -99 are ours, not HTTPClient's (which uses -1..-11).
constexpr int kUploadOpenFailed  = -100;
constexpr int kUploadBeginFailed = -101;
constexpr int kUploadNoHeap      = -102;

// A File that counts what HTTPClient actually consumed. This is the number the
// library will not give us: on a -3 it knows how far it got (bytesWritten) but
// only whispers it through a compiled-out log_d(). Counting reads on our side
// brackets the same thing -- it can read at most one buffer (HTTP_TCP_BUFFER_SIZE)
// ahead of what it wrote -- and tells us *where* in the body it died:
//   consumed ~= size  -> body was fully streamed; failure is at the end (final
//                        Content-Length check, or the peer closed at the tail)
//   consumed << size   -> the write stalled mid-body, at this offset
// Sealed-unit rule: this has to be readable over HTTP, not just Serial -- see
// uploadLogJson() below. Debugging by USB means dismantling the housing.
class CountingFile : public Stream {
public:
    explicit CountingFile(File& f) : _f(f) {}
    size_t consumed() const { return _consumed; }

    int available() override { return _f.available(); }
    int read() override {
        int c = _f.read();
        if (c >= 0) ++_consumed;
        return c;
    }
    int peek() override { return _f.peek(); }
    size_t readBytes(char* buffer, size_t length) {
        size_t n = _f.readBytes(buffer, length);
        _consumed += n;
        return n;
    }
    size_t write(uint8_t) override { return 0; }        // read-only source
    size_t write(const uint8_t*, size_t) override { return 0; }

private:
    File&  _f;
    size_t _consumed = 0;
};

// Last few upload attempts, newest last. Small and fixed so it costs nothing
// on a heap this path is already sensitive about.
struct UploadAttemptLog {
    uint32_t atMs      = 0;
    char     file[32]  = {0};
    int      attempt   = 0;
    int      code      = 0;
    size_t   bytes     = 0;
    size_t   consumed  = 0;
    uint32_t elapsedMs = 0;
    uint32_t heapFree  = 0;
    uint32_t heapLargest = 0;     // after the attempt
    uint32_t heapLargestPre = 0;  // ...and before it opened a TLS session
    int      rssi      = 0;
    int      staStatus = 0;
    bool     apSuspended = false;  // was the softAP off for this attempt?
    uint32_t dnsMs     = 0;        // how long resolving the API host took
    bool     dnsOk     = false;    // ...and whether it worked at all
};
constexpr size_t kUploadLogSlots = 8;
static UploadAttemptLog gUploadLog[kUploadLogSlots];
static size_t           gUploadLogCount = 0;  // total ever recorded

// Set by ApSuspendGuard (below) and recorded into each attempt, so the upload
// log says which radio configuration actually ran -- otherwise an A/B of the
// AP-suspend experiment is unreadable after the fact.
static bool gApSuspendedForUpload = false;

// Largest contiguous free block sampled immediately before an attempt stands up
// its TLS session, recorded alongside the post-attempt figure. mbedtls needs a
// large contiguous allocation per session and fails the whole connect with
// (-32512) SSL - Memory allocation failed when it cannot get one -- which is
// what every -1 in this investigation turned out to be. The post-attempt number
// alone cannot show that, because by then the session has been torn down and the
// memory handed back. Same static-set-then-record pattern as the flag above.
static uint32_t gHeapLargestPre = 0;

static void recordAttempt(const char* path, int attempt, int code, size_t bytes,
                          size_t consumed, uint32_t elapsedMs,
                          uint32_t dnsMs = 0, bool dnsOk = false) {
    UploadAttemptLog& e = gUploadLog[gUploadLogCount % kUploadLogSlots];
    e.atMs = millis();
    strlcpy(e.file, basenameOf(path), sizeof(e.file));
    e.attempt     = attempt;
    e.code        = code;
    e.bytes       = bytes;
    e.consumed    = consumed;
    e.elapsedMs   = elapsedMs;
    e.heapFree    = ESP.getFreeHeap();
    // TLS needs *contiguous* space, and this path already crashed once on
    // fragmentation (see runCalibrationUpload step 1), so total free is not enough.
    e.heapLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    e.heapLargestPre = gHeapLargestPre;
    e.rssi        = (int)WiFi.RSSI();
    e.staStatus   = (int)WiFi.status();
    e.apSuspended = gApSuspendedForUpload;
    e.dnsMs       = dnsMs;
    e.dnsOk       = dnsOk;
    ++gUploadLogCount;
}

static void logUploadFailure(const UploadAttemptLog& e) {
    Serial.printf("[CLOUD] upload attempt=%d code=%d file=%s bytes=%u consumed=%u "
                  "elapsed=%lums heap_free=%u heap_largest=%u/%u rssi=%d sta=%d ap_off=%d "
                  "dns=%s/%lums\n",
                  e.attempt, e.code, e.file, (unsigned)e.bytes, (unsigned)e.consumed,
                  (unsigned long)e.elapsedMs, (unsigned)e.heapFree,
                  (unsigned)e.heapLargestPre, (unsigned)e.heapLargest,
                  e.rssi, e.staStatus, e.apSuspended ? 1 : 0,
                  e.dnsOk ? "ok" : "FAIL", (unsigned long)e.dnsMs);
}

// Stops the softAP for the lifetime of the guard, if configured and if the AP
// was actually running. Restores it in the destructor so every early return
// below -- and there are several -- puts the radio back the way it found it.
struct ApSuspendGuard {
    bool suspended = false;
    explicit ApSuspendGuard(bool armed) {
        if (armed) arm();
    }
    // Suspend the AP from here on. Separate from construction so a caller can
    // decide *after* seeing how the first attempt went.
    void arm() {
        if (suspended || !CLOUD_UPLOAD_SUSPEND_AP) return;
        suspended = wifi::suspendAp();
        gApSuspendedForUpload = suspended;
    }
    ~ApSuspendGuard() {
        release();
    }
    // Hand the radio back early. Used when an attempt fails at *connect* time
    // with the AP suspended: suspending changes WiFi mode immediately before we
    // dial out, so it is itself a candidate for a connect failure, and an
    // experiment must never be the reason an upload cannot happen.
    void release() {
        if (suspended) wifi::resumeAp();
        suspended = false;
        gApSuspendedForUpload = false;
    }
    ApSuspendGuard(const ApSuspendGuard&)            = delete;
    ApSuspendGuard& operator=(const ApSuspendGuard&) = delete;
};

// One attempt. Returns the HTTP status, or a negative transport/sentinel code.
static int postUploadOnce(const String& token, const char* kind, const char* path,
                          String& body, int attempt) {
    File f = LittleFS.open(path, FILE_READ);
    if (!f) {
        recordAttempt(path, attempt, kUploadOpenFailed, 0, 0, 0);
        return kUploadOpenFailed;
    }
    size_t len = f.size();

    // Resolve the host ourselves first, timed. This is the cheapest test of
    // whether the device can still *receive*: a DNS reply is one inbound packet.
    // The 2026-09-18 capture showed the server ACKing everything while the
    // device saw none of it, so "can it hear anything right now" is the question
    // behind both the -3 body stalls and the -1 connect failures that follow
    // them. WiFiClientSecure::connect(host,...) fails silently (returns 0, no
    // log) when hostByName fails, which is exactly the case we could not see.
    IPAddress resolved;
    uint32_t dnsStart = millis();
    bool dnsOk = WiFi.hostByName(CLOUD_API_HOST, resolved);
    uint32_t dnsMs = millis() - dnsStart;
    Serial.printf("[CLOUD] dns %s -> %s in %lums (rssi %d ch %d)\n",
                  CLOUD_API_HOST,
                  dnsOk ? resolved.toString().c_str() : "FAILED",
                  (unsigned long)dnsMs, (int)WiFi.RSSI(), (int)WiFi.channel());

    gHeapLargestPre = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (gHeapLargestPre < CLOUD_TLS_MIN_LARGEST_BLOCK) {
        // Refuse rather than let mbedtls fail the connect and have HTTPClient
        // render it as -1 "connection refused" -- a network verdict on what is
        // actually a heap condition. See CLOUD_TLS_MIN_LARGEST_BLOCK.
        Serial.printf("[CLOUD] skipping attempt %d: largest free block %u < %u\n",
                      attempt, (unsigned)gHeapLargestPre,
                      (unsigned)CLOUD_TLS_MIN_LARGEST_BLOCK);
        f.close();
        recordAttempt(path, attempt, kUploadNoHeap, len, 0, 0);
        return kUploadNoHeap;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    // Do NOT set a timeout on `client` here. WiFiClientSecure::setTimeout()
    // writes _timeout and the socket's SO_SNDTIMEO/SO_RCVTIMEO, but the value
    // that actually governs a stalled body send -- sslclient->socket_timeout --
    // is latched once, inside start_ssl_client(), from whatever _timeout held at
    // connect time. HTTPClient connects for us from inside sendRequest(), so the
    // only value that reaches it is HTTPClient's _connectTimeout. Hence the call
    // below, which is the real fix for the -3; see the long trace at
    // CLOUD_UPLOAD_CONNECT_TIMEOUT_MS in config.h.
    https.setConnectTimeout((int32_t)CLOUD_UPLOAD_CONNECT_TIMEOUT_MS);
    // Still worth setting: _tcpTimeout bounds the *response* read loop, which
    // is a separate stall (server accepted the body, then went quiet).
    https.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
    if (!https.begin(client, apiUrl("/api/device/uploads"))) {
        f.close();
        // Recorded like any other failure: a log that silently omits the
        // can't-even-start cases is worse than no log, because its absence
        // reads as "the upload was never attempted".
        recordAttempt(path, attempt, kUploadBeginFailed, len, 0, 0);
        return kUploadBeginFailed;
    }
    https.addHeader("Authorization", "Bearer " + token);
    https.addHeader("X-Upload-Kind", kind);
    https.addHeader("X-Upload-Filename", basenameOf(path));
    https.addHeader("Content-Type", "application/octet-stream");

    CountingFile counted(f);
    uint32_t t0 = millis();
    int code = https.sendRequest("POST", &counted, len);
    uint32_t elapsedMs = millis() - t0;
    size_t consumed = counted.consumed();

    // Read the TLS/socket error *before* end(), which stops the client and
    // clears it. This is the detail behind a bare "start_ssl_client: -1":
    // WiFiClientSecure keeps the mbedtls error code and its message string.
    //
    // Only negative values mean anything. WiFiClientSecure::_lastError is
    // assigned start_ssl_client()'s return unconditionally, and that function
    // returns the socket fd on success -- so a "lastError" of 50 is file
    // descriptor 50, not an error, and mbedtls_strerror() dutifully renders it
    // as "UNKNOWN ERROR CODE (0032)". Printing that on a -3 sent this
    // investigation chasing a nonexistent errno.
    if (code < 0) {
        char sslErr[128] = {0};
        int sslCode = client.lastError(sslErr, sizeof(sslErr));
        if (sslCode < 0) {
            Serial.printf("[CLOUD] tls error %d: %s\n", sslCode, sslErr);
        }
    }

    f.close();
    body = https.getString();
    https.end();

    // Recorded for every attempt, not just failures: a success next to a
    // failure is what makes the failure's numbers mean anything.
    recordAttempt(path, attempt, code, len, consumed, elapsedMs, dnsMs, dnsOk);
    logUploadFailure(gUploadLog[(gUploadLogCount - 1) % kUploadLogSlots]);
    return code;
}

// Retries a *transport* failure only. Safe to repeat: the backend dedupes on
// content hash and answers a re-POST of identical bytes with 409 + the
// existing upload_id, which both callers already treat as success. A real HTTP
// status (including 4xx/5xx) is returned as-is -- resending can't fix those.
static int postUploadStreamed(const String& token, const char* kind, const char* path,
                              String& body) {
    // The first attempt always runs in the unit's normal radio configuration.
    // Suspending the softAP turned out to break the connection outright (-1,
    // never reached the server) on both a hotspot and the home AP, where the
    // same unit had been failing at -3 -- i.e. the mitigation was worse than
    // the fault. An experiment does not get to touch the path until that path
    // has already failed on its own terms.
    ApSuspendGuard apGuard(/*armed=*/false);

    int code = postUploadOnce(token, kind, path, body, 1);

    // Attempt 1 failed at the transport level. *Now* try it with the AP down --
    // the retries were going to happen anyway, so this costs nothing, and
    // `ap_suspended` in the upload log gives a clean A/B inside one run.
    if (code < 0 && code > kUploadOpenFailed) apGuard.arm();

    for (int attempt = 2;
         code < 0 && code > kUploadOpenFailed && attempt <= CLOUD_UPLOAD_ATTEMPTS;
         ++attempt) {
        // Fresh WiFiClientSecure + file handle per attempt (postUploadOnce owns
        // both), so a half-dead TLS session or a mid-file read position can't
        // carry over into the retry.
        delay(CLOUD_UPLOAD_RETRY_DELAY_MS);
        Serial.printf("[CLOUD] retrying upload of %s (attempt %d/%d)\n",
                      path, attempt, CLOUD_UPLOAD_ATTEMPTS);
        int retryCode = postUploadOnce(token, kind, path, body, attempt);

        // A retry that never opened a socket must not overwrite the verdict of
        // the attempt that did. The caller shows the *last* code, so letting a
        // heap refusal land there would report the upload as a connection
        // failure when what actually happened was attempt 1's body stall --
        // exactly the misreading that made every -1 in this investigation look
        // like a network problem. Keep attempt 1's code and stop.
        if (retryCode == kUploadNoHeap) {
            Serial.printf("[CLOUD] no retry possible (heap); reporting attempt 1 code %d\n",
                          code);
            break;
        }
        code = retryCode;
    }

    // Every attempt failed at the transport level. Hand the radio back and get
    // the diagnostics off the device while we still can -- this report is small
    // enough to succeed on a link that cannot carry the upload itself, and on a
    // hotspot it is the only copy anyone will ever be able to read.
    if (code < 0 && code > kUploadOpenFailed) {
        apGuard.release();
        // Give the heap a moment before asking mbedtls for another session.
        // This report is the third TLS connect of a failed run and it is the one
        // that reliably dies on (-32512) SSL - Memory allocation failed.
        delay(CLOUD_TLS_SETTLE_MS);
        String reportErr;
        if (!reportUploadDiagnostics(reportErr)) {
            Serial.printf("[CLOUD] could not report diagnostics: %s\n", reportErr.c_str());
        }
    }
    return code;
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
        String body;
        int code = postUploadStreamed(token, "calibration_raw", csvPath, body);
        if (code == kUploadOpenFailed) {
            result.errorMessage = "could not read samples file";
            return result;
        }
        if (code == kUploadBeginFailed) {
            result.errorMessage = "could not start connection";
            return result;
        }
        if (code == kUploadNoHeap) {
            result.errorMessage = "out of memory for upload -- reboot, then retry from the menu";
            return result;
        }

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

// Last few streamed-upload attempts as JSON, newest first. Exposed on
// tern.local (GET /api/cloud/upload-log) because the unit that needs this is
// sealed: a -3 has to be diagnosable without opening the housing for USB.
String uploadLogJson() {
    String json = "{\"now_ms\":";
    json += millis();
    json += ",\"total\":";
    json += (uint32_t)gUploadLogCount;
    json += ",\"attempts\":[";
    size_t have = gUploadLogCount < kUploadLogSlots ? gUploadLogCount : kUploadLogSlots;
    for (size_t i = 0; i < have; ++i) {
        // Walk backwards from the newest.
        const UploadAttemptLog& e = gUploadLog[(gUploadLogCount - 1 - i) % kUploadLogSlots];
        if (i) json += ',';
        json += "{\"at_ms\":";        json += e.atMs;
        json += ",\"file\":\"";       json += e.file;
        json += "\",\"attempt\":";    json += e.attempt;
        json += ",\"code\":";         json += e.code;
        json += ",\"bytes\":";        json += (uint32_t)e.bytes;
        json += ",\"consumed\":";     json += (uint32_t)e.consumed;
        json += ",\"elapsed_ms\":";   json += e.elapsedMs;
        json += ",\"heap_free\":";    json += e.heapFree;
        json += ",\"heap_largest\":"; json += e.heapLargest;
        json += ",\"heap_largest_pre\":"; json += e.heapLargestPre;
        json += ",\"rssi\":";         json += e.rssi;
        json += ",\"sta_status\":";   json += e.staStatus;
        json += ",\"ap_suspended\":"; json += e.apSuspended ? "true" : "false";
        json += ",\"dns_ms\":";        json += e.dnsMs;
        json += ",\"dns_ok\":";        json += e.dnsOk ? "true" : "false";
        json += '}';
    }
    json += "]}";
    return json;
}

bool reportUploadDiagnostics(String& errorOut) {
    String token = loadToken();
    if (token.length() == 0) {
        errorOut = "device not authorized";
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        errorOut = "no STA link";
        return false;
    }

    // recorded_at is the table's dedupe key together with device_id, so a unit
    // whose clock never got set (NTP runs on connect; a hotspot may not have
    // let it through) will collide with itself on repeat reports. Send anyway:
    // a deduped row is a far better failure than no diagnostics at all.
    char stamp[32];
    time_t now = time(nullptr);
    struct tm tmUtc;
    gmtime_r(&now, &tmUtc);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tmUtc);

    String payload = "{\"samples\":[{\"recorded_at\":\"";
    payload += stamp;
    payload += "\",\"firmware_version\":\"";
    payload += FW_VERSION;
    payload += "\",\"uptime_s\":";
    payload += (uint32_t)(millis() / 1000);
    payload += ",\"free_heap_bytes\":";
    payload += (uint32_t)ESP.getFreeHeap();
    payload += ",\"wifi_rssi_dbm\":";
    payload += (int)WiFi.RSSI();
    payload += ",\"subsystems\":{\"upload_log\":";
    payload += uploadLogJson();
    payload += "}}]}";

    uint32_t largestPre = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    Serial.printf("[CLOUD] diagnostics: heap_free=%u heap_largest=%u payload=%u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)largestPre,
                  (unsigned)payload.length());
    if (largestPre < CLOUD_TLS_MIN_LARGEST_BLOCK) {
        // This report is the last TLS session of a failed run, so it is the one
        // most likely to hit the wall. Saying so beats another bogus -1.
        errorOut = "not enough contiguous heap for TLS (largest block " +
                   String((uint32_t)largestPre) + ")";
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    // See the note in postUploadOnce: _connectTimeout is what reaches
    // sslclient->socket_timeout, so this is the knob, not client.setTimeout().
    https.setConnectTimeout((int32_t)CLOUD_DIAG_TIMEOUT_S * 1000);
    https.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
    if (!https.begin(client, apiUrl("/api/device/health"))) {
        errorOut = "could not start connection";
        return false;
    }
    https.addHeader("Authorization", "Bearer " + token);
    https.addHeader("Content-Type", "application/json");
    int code = https.POST(payload);
    String body = https.getString();

    // Read before end(): a failed connect leaves the mbedtls code here, and it
    // is the difference between "the network refused us" and "we ran out of
    // contiguous heap", which look identical from the outside (both surface as
    // -1). -32512 is MBEDTLS_ERR_SSL_ALLOC_FAILED.
    if (code < 0) {
        char sslErr[128] = {0};
        int sslCode = client.lastError(sslErr, sizeof(sslErr));
        if (sslCode < 0) {
            Serial.printf("[CLOUD] diagnostics tls error %d: %s (largest block was %u)\n",
                          sslCode, sslErr, (unsigned)largestPre);
        }
    }
    https.end();

    if (code < 200 || code >= 300) {
        errorOut = extractError(body, code);
        Serial.printf("[CLOUD] diagnostics report failed: %s\n", errorOut.c_str());
        return false;
    }
    Serial.printf("[CLOUD] diagnostics reported (%u bytes)\n", (unsigned)payload.length());
    return true;
}

bool uploadBackup(const char* kind, const char* filePath, String& errorOut) {
    String token = loadToken();
    if (token.length() == 0) {
        errorOut = "device not authorized -- run cloud setup first";
        return false;
    }

    String body;
    int code = postUploadStreamed(token, kind, filePath, body);
    if (code == kUploadOpenFailed) {
        errorOut = "could not read " + String(filePath);
        return false;
    }
    if (code == kUploadBeginFailed) {
        errorOut = "could not start connection";
        return false;
    }
    if (code == kUploadNoHeap) {
        errorOut = "out of memory for upload -- reboot, then retry";
        return false;
    }

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
