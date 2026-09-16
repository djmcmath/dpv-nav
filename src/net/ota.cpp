#include "ota.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <mbedtls/sha256.h>
#include <dpvlink.h>
#include <ota_link.h>

#include "wifi_manager.h"
#include "../config.h"
#include "../nav_main.h"
#include "../version.h"

namespace ota {

// TLS is setInsecure() for the same reason as cloud_client.cpp (no reflash on
// CA rotation). What stops a corrupted or truncated image from being booted is
// the sha256 check against the manifest (here for nav, on the display for its
// image), not the transport.

static constexpr const char* FIRMWARE_PATH_PREFIX = "/api/firmware/dpv_nav/";
static constexpr uint32_t CHECK_RETRY_MS     = 5UL * 60UL * 1000UL;  // after a failed boot check
static constexpr uint32_t STALL_TIMEOUT_MS   = 20000;                // no bytes for this long = give up
static constexpr uint32_t REBOOT_DELAY_MS    = 3000;                 // lets tern.local see "rebooting"
static constexpr int      MANIFEST_MAX_BYTES = 32768;

// Display transfer timing (the per-frame ACK timeout and retries are in ota_link.h).
static constexpr uint32_t END_REPLY_TIMEOUT_MS  = 3000;   // wait for OTA_DONE after the end frame
static constexpr uint8_t  END_MAX_SENDS         = 3;
static constexpr uint32_t VERIFY_PING_MS        = 2000;   // re-ping the restarting display
static constexpr uint32_t VERIFY_TIMEOUT_MS     = 45000;  // display must be back on the new version by then

// "Update available" hint for the display's screen, after the boot check.
static constexpr uint8_t  HINT_SENDS       = 3;
static constexpr uint32_t HINT_INTERVAL_MS = 2000;

struct Image {
    uint32_t size = 0;
    char     sha256[65] = "";  // lowercase hex
};

enum class XferPhase : uint8_t { HELLO, SEND, WAIT_ACK, END_WAIT };

static State    gState = State::IDLE;
static String   gError;
static String   gMessage;                     // success text for DONE
static bool     gChecked            = false;  // a manifest check has succeeded
static uint32_t gLastCheckAttemptMs = 0;
static bool     gReleaseValid       = false;  // gLatest names a release with both images
static char     gLatest[16]         = "";
static Image    gNavImage;
static Image    gDisplayImage;
static JsonDocument gReleases;                // array of newer releases, for the page

// Download state -- heap-allocated only while a download is running, so the
// ~40 KB TLS context isn't held for the life of the unit.
static WiFiClientSecure*     gClient = nullptr;
static HTTPClient*           gHttp   = nullptr;
static mbedtls_sha256_context gSha;
static uint32_t gWritten     = 0;
static uint32_t gLastByteMs  = 0;
static uint32_t gRebootAtMs  = 0;
static bool     gNavAfterDisplay = false;  // install continues with nav once the display is done

// Display transfer state.
static XferPhase gXfer        = XferPhase::HELLO;
static bool      gDisplayReady = false;       // display accepted the begin packet (binary mode)
static uint8_t   gSends        = 0;           // attempts for the current packet/frame
static uint32_t  gSentAtMs     = 0;
static uint16_t  gSeq          = 0;
static uint8_t   gChunk[otalink::MAX_PAYLOAD];
static uint16_t  gChunkLen     = 0;
static uint32_t  gAcked        = 0;           // bytes the display has confirmed
static uint8_t   gFrame[otalink::MAX_FRAME_LEN];
static size_t    gFrameLen     = 0;
static otalink::AckParser gAck;
static char      gLine[128];
static size_t    gLinePos      = 0;
static uint32_t  gVerifyStartMs = 0;
static uint32_t  gLastPingMs    = 0;

static uint8_t   gHintsLeft    = 0;
static uint32_t  gNextHintMs   = 0;

// ---------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------

// Strictly MAJOR.MINOR.PATCH -- matches the server's valid_version().
static bool parseVersion(const char* s, unsigned v[3]) {
    char tail;
    return s && sscanf(s, "%u.%u.%u%c", &v[0], &v[1], &v[2], &tail) == 3;
}

// <0, 0, >0 like strcmp. Callers only pass strings parseVersion() accepted.
static int compareVersions(const char* a, const char* b) {
    unsigned va[3] = {0, 0, 0}, vb[3] = {0, 0, 0};
    parseVersion(a, va);
    parseVersion(b, vb);
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    }
    return 0;
}

static bool isSha256Hex(const char* s) {
    if (!s || strlen(s) != 64) return false;
    for (const char* p = s; *p; p++) {
        if (!isxdigit((unsigned char)*p)) return false;
    }
    return true;
}

static bool displayVersionKnown() {
    unsigned tmp[3];
    return parseVersion(displayFwVersion(), tmp);
}

static bool navNeedsUpdate() {
    return gReleaseValid && compareVersions(gLatest, FW_VERSION) > 0;
}

// An unknown display version (pre-OTA build) is not "needs update" -- it can't
// receive one; blockedReason() explains that instead.
static bool displayNeedsUpdate() {
    return gReleaseValid && displayVersionKnown() && compareVersions(gLatest, displayFwVersion()) > 0;
}

static bool updateAvailable() { return navNeedsUpdate() || displayNeedsUpdate(); }

static String imageUrl(const char* version, const char* board) {
    String url = "https://";
    url += CLOUD_API_HOST;
    url += FIRMWARE_PATH_PREFIX;
    url += version;
    url += "/";
    url += board;
    url += ".bin";
    return url;
}

static const char* stateName(State s) {
    switch (s) {
        case State::IDLE:             return "idle";
        case State::CHECKING:         return "checking";
        case State::UP_TO_DATE:       return "up_to_date";
        case State::AVAILABLE:        return "available";
        case State::DISPLAY_TRANSFER: return "display_transfer";
        case State::DISPLAY_VERIFY:   return "display_verify";
        case State::NAV_DOWNLOAD:     return "nav_download";
        case State::REBOOTING:        return "rebooting";
        case State::DONE:             return "done";
        case State::FAILED:           return "failed";
    }
    return "unknown";
}

static bool installing() {
    return gState == State::DISPLAY_TRANSFER || gState == State::DISPLAY_VERIFY ||
           gState == State::NAV_DOWNLOAD || gState == State::REBOOTING;
}

static const char* blockedReason() {
    if (!wifi::isStaConnected()) return "Not connected to a WiFi network with internet access";
    if (!displayVersionKnown()) {
        return "The display didn't report a firmware version at boot, so it can't receive "
               "updates -- flash it over USB once";
    }
    return otaBlockedReason();
}

bool ownsDisplayLink() { return gState == State::DISPLAY_TRANSFER; }

// ---------------------------------------------------------------------------
// Manifest check
// ---------------------------------------------------------------------------

static bool readImage(JsonObject images, const char* board, Image& out) {
    JsonObject o = images[board];
    uint32_t size = o["size"] | (uint32_t)0;
    const char* sha = o["sha256"] | "";
    if (size == 0 || !isSha256Hex(sha)) return false;
    out.size = size;
    for (int i = 0; i < 64; i++) out.sha256[i] = (char)tolower((unsigned char)sha[i]);
    out.sha256[64] = '\0';
    return true;
}

// Returns an error string, or "" on success.
static String runCheck() {
    gLastCheckAttemptMs = millis();

    if (!wifi::isStaConnected()) return "Not connected to a WiFi network with internet access";

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.setTimeout(CLOUD_HTTP_TIMEOUT_MS);

    String url = "https://";
    url += CLOUD_API_HOST;
    url += FIRMWARE_PATH_PREFIX;
    url += "manifest.json";
    if (!https.begin(client, url)) return "Could not start connection";

    int code = https.GET();
    if (code == 404) {
        // Nothing published yet -- that's "up to date", not a failure.
        https.end();
        gChecked = true;
        gReleaseValid = false;
        gLatest[0] = '\0';
        gReleases.clear();
        gReleases.to<JsonArray>();
        return "";
    }
    if (code != 200) {
        https.end();
        return "Manifest request failed (HTTP " + String(code) + ")";
    }
    if (https.getSize() > MANIFEST_MAX_BYTES) {
        https.end();
        return "Manifest too large";
    }
    String body = https.getString();
    https.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) return "Manifest is not valid JSON";

    // Changes are listed from the older of the two boards, so a display that
    // lagged behind still shows what it's about to get.
    const char* baseline = FW_VERSION;
    if (displayVersionKnown() && compareVersions(displayFwVersion(), FW_VERSION) < 0) {
        baseline = displayFwVersion();
    }

    const char* newest = nullptr;
    JsonObject  newestObj;
    JsonDocument releases;
    JsonArray    out = releases.to<JsonArray>();

    for (JsonObject r : doc["releases"].as<JsonArray>()) {
        const char* v = r["version"] | "";
        unsigned tmp[3];
        if (!parseVersion(v, tmp)) continue;
        if (!newest || compareVersions(v, newest) > 0) {
            newest = v;
            newestObj = r;
        }
        if (compareVersions(v, baseline) > 0) {
            // Manifest is newest-first (publish script sorts it), so this
            // list comes out in the same order.
            JsonObject o = out.add<JsonObject>();
            o["version"] = v;
            o["date"]    = r["date"] | "";
            o["changes"] = r["changes"];
        }
    }

    gReleases = releases;
    gChecked = true;
    gReleaseValid = false;
    gLatest[0] = '\0';

    if (!newest) return "";
    strncpy(gLatest, newest, sizeof(gLatest) - 1);
    gLatest[sizeof(gLatest) - 1] = '\0';

    JsonObject images = newestObj["images"];
    if (!readImage(images, "nav", gNavImage) || !readImage(images, "display", gDisplayImage)) {
        return String("Release ") + newest + " is missing a valid nav or display image in the manifest";
    }
    gReleaseValid = true;
    return "";
}

static void applyCheckResult(const String& err) {
    if (err.length() > 0) {
        gError = err;
        gState = State::FAILED;
        Serial.printf("[OTA] check failed: %s\n", err.c_str());
        return;
    }
    gError = "";
    gState = updateAvailable() ? State::AVAILABLE : State::UP_TO_DATE;
    Serial.printf("[OTA] check ok: nav %s, display %s, latest %s -> %s\n", FW_VERSION,
                  displayVersionKnown() ? displayFwVersion() : "(unknown)",
                  gLatest[0] ? gLatest : "(none published)", stateName(gState));
}

String checkNow() {
    if (installing()) return "An update is in progress";
    gState = State::CHECKING;
    String err = runCheck();
    applyCheckResult(err);
    if (err.length() > 0) return err;
    return updateAvailable() ? String("Version ") + gLatest + " is available"
                             : String("Up to date (") + FW_VERSION + ")";
}

// ---------------------------------------------------------------------------
// Shared HTTPS image stream
// ---------------------------------------------------------------------------

static void releaseHttp() {
    if (gHttp) {
        gHttp->end();
        delete gHttp;
        gHttp = nullptr;
    }
    if (gClient) {
        delete gClient;
        gClient = nullptr;
    }
}

// Opens board's image and checks the server agrees on its size.
static bool openImage(const char* board, uint32_t expectSize, String& errOut) {
    gClient = new WiFiClientSecure;
    gClient->setInsecure();
    gHttp = new HTTPClient;
    gHttp->setTimeout(CLOUD_HTTP_TIMEOUT_MS);

    if (!gHttp->begin(*gClient, imageUrl(gLatest, board))) {
        releaseHttp();
        errOut = "Could not start connection";
        return false;
    }
    int code = gHttp->GET();
    if (code != 200) {
        releaseHttp();
        errOut = String(board) + " image download failed (HTTP " + String(code) + ")";
        return false;
    }
    // The raw stream is read directly, which only works for a Content-Length
    // body (no chunked framing to strip).
    int len = gHttp->getSize();
    if (len != (int)expectSize) {
        releaseHttp();
        errOut = String("Server ") + board + " image size (" + String(len) +
                 ") doesn't match the manifest (" + String(expectSize) + ")";
        return false;
    }
    gLastByteMs = millis();
    return true;
}

// ---------------------------------------------------------------------------
// Nav image download
// ---------------------------------------------------------------------------

static void failNavDownload(const String& why) {
    Update.abort();
    mbedtls_sha256_free(&gSha);
    releaseHttp();
    gError = why + " -- nav firmware was not changed";
    gState = State::FAILED;
    Serial.printf("[OTA] nav download failed: %s\n", why.c_str());
}

static bool beginNavDownload(String& errOut) {
    if (!openImage("nav", gNavImage.size, errOut)) return false;
    if (!Update.begin(gNavImage.size, U_FLASH)) {
        releaseHttp();
        errOut = String("Can't prepare flash: ") + Update.errorString();
        return false;
    }

    mbedtls_sha256_init(&gSha);
    mbedtls_sha256_starts_ret(&gSha, /*is224=*/0);
    gWritten = 0;
    gError   = "";
    gState   = State::NAV_DOWNLOAD;
    Serial.printf("[OTA] downloading nav %s (%u bytes)\n", gLatest, gNavImage.size);
    return true;
}

static void finishNavDownload() {
    unsigned char digest[32];
    mbedtls_sha256_finish_ret(&gSha, digest);
    mbedtls_sha256_free(&gSha);
    releaseHttp();

    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    if (strcmp(hex, gNavImage.sha256) != 0) {
        Update.abort();
        gError = "Nav checksum mismatch (download corrupted) -- nav firmware was not changed";
        gState = State::FAILED;
        Serial.printf("[OTA] sha256 mismatch: got %s want %s\n", hex, gNavImage.sha256);
        return;
    }
    // end() validates the image header and switches the boot partition.
    if (!Update.end()) {
        gError = String("Nav image rejected: ") + Update.errorString() + " -- nav firmware was not changed";
        gState = State::FAILED;
        Serial.printf("[OTA] Update.end failed: %s\n", Update.errorString());
        return;
    }
    gState      = State::REBOOTING;
    gRebootAtMs = millis() + REBOOT_DELAY_MS;
    Serial.printf("[OTA] nav %s verified and installed -- rebooting\n", gLatest);
}

static void pumpNavDownload() {
    static uint8_t buf[4096];

    WiFiClient* stream = gHttp ? gHttp->getStreamPtr() : nullptr;
    int avail = stream ? stream->available() : 0;
    if (avail > 0) {
        size_t remaining = gNavImage.size - gWritten;
        size_t want = (size_t)avail;
        if (want > sizeof(buf)) want = sizeof(buf);
        if (want > remaining) want = remaining;
        int n = stream->read(buf, want);
        if (n > 0) {
            mbedtls_sha256_update_ret(&gSha, buf, n);
            if (Update.write(buf, n) != (size_t)n) {
                failNavDownload(String("Flash write failed: ") + Update.errorString());
                return;
            }
            gWritten += n;
            gLastByteMs = millis();
        }
    } else if (millis() - gLastByteMs > STALL_TIMEOUT_MS) {
        failNavDownload("Download stalled (connection lost?)");
        return;
    }

    if (gWritten >= gNavImage.size) finishNavDownload();
}

// ---------------------------------------------------------------------------
// Display transfer (protocol: lib/dpvlink/ota_link.h)
// ---------------------------------------------------------------------------

static void sendBeginPacket() {
    OtaBeginPacket pkt{};
    pkt.pv   = otalink::PROTOCOL_VERSION;
    pkt.size = gDisplayImage.size;
    strncpy(pkt.sha256, gDisplayImage.sha256, sizeof(pkt.sha256) - 1);
    strncpy(pkt.version, gLatest, sizeof(pkt.version) - 1);
    char buf[160];
    // Leading newline ends any line the display was partway through, so the
    // begin packet isn't glued onto the tail of a NavPacket.
    buf[0] = '\n';
    size_t n = otaBeginPacketToBytes(pkt, buf + 1, sizeof(buf) - 1);
    if (n > 0) Serial1.write((const uint8_t*)buf, n + 1);
    gSends++;
    gSentAtMs = millis();
}

static void sendCurrentFrame() {
    Serial1.write(gFrame, gFrameLen);
    gSends++;
    gSentAtMs = millis();
    gAck.reset();
}

static void failDisplayTransfer(const String& why) {
    if (gDisplayReady) {
        // Best effort: put the display back to normal now rather than after its idle timeout.
        uint8_t abortFrame[otalink::HEADER_LEN + otalink::CRC_LEN];
        size_t n = otalink::encodeFrame(otalink::SEQ_ABORT, nullptr, 0, abortFrame, sizeof(abortFrame));
        Serial1.write(abortFrame, n);
    }
    releaseHttp();
    gError = why + " -- no firmware was changed";
    gState = State::FAILED;
    Serial.printf("[OTA] display transfer failed: %s\n", why.c_str());
}

static bool beginDisplayTransfer(String& errOut) {
    if (!openImage("display", gDisplayImage.size, errOut)) return false;

    while (Serial1.available()) Serial1.read();  // drop any half-read display command
    gDisplayReady = false;
    gSends        = 0;
    gSeq          = 0;
    gChunkLen     = 0;
    gAcked        = 0;
    gLinePos      = 0;
    gError        = "";
    gXfer         = XferPhase::HELLO;
    gState        = State::DISPLAY_TRANSFER;
    Serial.printf("[OTA] sending display %s (%u bytes)\n", gLatest, gDisplayImage.size);
    sendBeginPacket();
    return true;
}

// Reads one JSON line from the display. Returns true when gLine holds one.
static bool readDisplayLine() {
    while (Serial1.available()) {
        char c = (char)Serial1.read();
        if (c == '\n') {
            gLine[gLinePos] = '\0';
            bool have = gLinePos > 0;
            gLinePos = 0;
            if (have) return true;
        } else if (gLinePos < sizeof(gLine) - 1) {
            gLine[gLinePos++] = c;
        }
    }
    return false;
}

// OTA_READY / OTA_DONE in gLine? Fills ok/err and returns true if it's `want`.
static bool lineIsReply(DisplayCmd want, bool& ok, char* err, size_t errLen) {
    DisplayCmd cmd = DisplayCmd::NONE;
    size_t len = strlen(gLine);
    if (!bytesToDisplayCmd(gLine, len, cmd) || cmd != want) return false;
    return parseOtaReply(gLine, len, ok, err, errLen);
}

static void startDisplayVerify() {
    releaseHttp();
    forgetDisplayFwVersion();
    gVerifyStartMs = millis();
    gLastPingMs    = 0;
    gState         = State::DISPLAY_VERIFY;
    Serial.println("[OTA] display image sent -- waiting for it to restart on the new version");
}

static void pumpDisplayTransfer() {
    uint32_t now = millis();

    switch (gXfer) {
        case XferPhase::HELLO: {
            bool ok = false;
            char err[64];
            while (readDisplayLine()) {
                if (!lineIsReply(DisplayCmd::OTA_READY, ok, err, sizeof(err))) continue;
                if (!ok) {
                    failDisplayTransfer(String("The display refused the update: ") + err);
                    return;
                }
                gDisplayReady = true;
                gSends = 0;
                gXfer  = XferPhase::SEND;
                Serial.println("[OTA] display ready");
                return;
            }
            if (now - gSentAtMs >= otalink::ACK_TIMEOUT_MS) {
                if (gSends >= otalink::MAX_RETRIES) {
                    failDisplayTransfer(String("The display didn't answer the update request. A display "
                                               "running firmware from before display updates existed "
                                               "(it reports ") + displayFwVersion() +
                                        ") has to be flashed over USB once");
                    return;
                }
                sendBeginPacket();
            }
            return;
        }

        case XferPhase::SEND: {
            // Fill the next frame's payload from HTTPS, then send it.
            uint32_t remaining = gDisplayImage.size - gAcked;
            uint16_t want = remaining < otalink::MAX_PAYLOAD ? (uint16_t)remaining : otalink::MAX_PAYLOAD;
            if (want == 0) {
                gFrameLen = otalink::encodeFrame(gSeq, nullptr, 0, gFrame, sizeof(gFrame));
                gSends = 0;
                sendCurrentFrame();
                gLinePos = 0;
                gXfer = XferPhase::END_WAIT;
                return;
            }
            WiFiClient* stream = gHttp ? gHttp->getStreamPtr() : nullptr;
            int avail = stream ? stream->available() : 0;
            if (avail > 0) {
                int n = stream->read(gChunk + gChunkLen, want - gChunkLen);
                if (n > 0) {
                    gChunkLen += n;
                    gLastByteMs = now;
                }
            } else if (now - gLastByteMs > STALL_TIMEOUT_MS) {
                failDisplayTransfer("Download stalled (connection lost?)");
                return;
            }
            if (gChunkLen < want) return;

            gFrameLen = otalink::encodeFrame(gSeq, gChunk, gChunkLen, gFrame, sizeof(gFrame));
            gSends = 0;
            sendCurrentFrame();
            gXfer = XferPhase::WAIT_ACK;
            return;
        }

        case XferPhase::WAIT_ACK: {
            bool resend = false;
            while (Serial1.available()) {
                if (!gAck.feed((uint8_t)Serial1.read())) continue;
                if (gAck.seq() != gSeq) continue;  // stale reply to an earlier send
                if (gAck.ok()) {
                    gAcked += gChunkLen;
                    gChunkLen = 0;
                    gSeq++;
                    gXfer = XferPhase::SEND;
                    return;
                }
                resend = true;  // NAK: the frame arrived damaged
                break;
            }
            if (!resend && now - gSentAtMs < otalink::ACK_TIMEOUT_MS) return;
            if (gSends >= otalink::MAX_RETRIES) {
                failDisplayTransfer(String("The display stopped accepting data at ") +
                                    String((uint64_t)gAcked * 100 / gDisplayImage.size) +
                                    "% (the reason, if any, is on its screen)");
                return;
            }
            Serial.printf("[OTA] resending frame %u (%s)\n", gSeq, resend ? "NAK" : "timeout");
            sendCurrentFrame();
            return;
        }

        case XferPhase::END_WAIT: {
            bool ok = false;
            char err[64];
            while (readDisplayLine()) {
                if (!lineIsReply(DisplayCmd::OTA_DONE, ok, err, sizeof(err))) continue;
                if (!ok) {
                    failDisplayTransfer(String("The display rejected the image: ") + err);
                    return;
                }
                startDisplayVerify();
                return;
            }
            if (now - gSentAtMs < END_REPLY_TIMEOUT_MS) return;
            if (gSends < END_MAX_SENDS) {
                sendCurrentFrame();
                return;
            }
            // No reply, but the display may well have installed it and restarted
            // (the reply is the one line that can go missing). Its version decides.
            startDisplayVerify();
            return;
        }
    }
}

static void pumpDisplayVerify() {
    uint32_t now = millis();
    const char* v = displayFwVersion();

    if (v[0]) {
        if (strcmp(v, gLatest) != 0) {
            gError = String("The display restarted on ") + v + " instead of " + gLatest +
                     " (it rejected the new image and rolled back) -- nav firmware was not changed";
            gState = State::FAILED;
            Serial.printf("[OTA] display came back on %s, wanted %s\n", v, gLatest);
            return;
        }
        Serial.printf("[OTA] display confirmed on %s\n", v);
        if (gNavAfterDisplay) {
            String err;
            if (!beginNavDownload(err)) {
                gError = String("Display updated to ") + gLatest + ", but the nav download failed: " + err;
                gState = State::FAILED;
            }
            return;
        }
        gMessage = String("Display updated to ") + gLatest + ".";
        gState   = State::DONE;
        return;
    }

    if (now - gVerifyStartMs > VERIFY_TIMEOUT_MS) {
        gError = "The display didn't come back after its update -- power-cycle the unit and check "
                 "the versions here; nav firmware was not changed";
        gState = State::FAILED;
        return;
    }
    if (gLastPingMs == 0 || now - gLastPingMs >= VERIFY_PING_MS) {
        gLastPingMs = now;
        char buf[32];
        size_t n = bootPingToBytes(FW_VERSION, buf, sizeof(buf));
        if (n > 0) Serial1.write(buf, n);
    }
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

bool start(String& errOut) {
    if (installing()) {
        errOut = "An update is already in progress";
        return false;
    }
    if (!updateAvailable()) {
        errOut = "No update available";
        return false;
    }
    const char* blocked = blockedReason();
    if (blocked) {
        errOut = blocked;
        return false;
    }

    gMessage = "";
    gNavAfterDisplay = navNeedsUpdate();
    bool ok = displayNeedsUpdate() ? beginDisplayTransfer(errOut) : beginNavDownload(errOut);
    if (!ok) {
        gError = errOut;
        gState = State::FAILED;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Loop hook + status
// ---------------------------------------------------------------------------

static void sendHints() {
    if (gHintsLeft == 0 || installing() || (int32_t)(millis() - gNextHintMs) < 0) return;
    char buf[48];
    size_t n = updateHintToBytes(gLatest, buf, sizeof(buf));
    if (n > 0) Serial1.write(buf, n);
    gHintsLeft--;
    gNextHintMs = millis() + HINT_INTERVAL_MS;
}

void update() {
    switch (gState) {
        case State::DISPLAY_TRANSFER:
            if (!wifi::isStaConnected()) {
                failDisplayTransfer("WiFi disconnected");
                return;
            }
            pumpDisplayTransfer();
            return;
        case State::DISPLAY_VERIFY:
            pumpDisplayVerify();
            return;
        case State::NAV_DOWNLOAD:
            // Dive mode kills WiFi; the stall timeout would catch it, but
            // there's no reason to sit on a dead download for 20 s.
            if (!wifi::isStaConnected()) {
                failNavDownload("WiFi disconnected");
                return;
            }
            pumpNavDownload();
            return;
        case State::REBOOTING:
            if ((int32_t)(millis() - gRebootAtMs) >= 0) ESP.restart();
            return;
        default:
            break;
    }

    sendHints();

    // One-shot check once the unit has internet; retried every few minutes
    // until one succeeds (hotspot switched on late, server briefly down...).
    if (!gChecked && wifi::isStaConnected() &&
        (gLastCheckAttemptMs == 0 || millis() - gLastCheckAttemptMs >= CHECK_RETRY_MS)) {
        gState = State::CHECKING;
        applyCheckResult(runCheck());
        if (gState == State::AVAILABLE) {
            gHintsLeft  = HINT_SENDS;
            gNextHintMs = millis();
        }
    }
}

String statusJson() {
    JsonDocument doc;
    doc["current_nav"]      = FW_VERSION;
    doc["current_display"]  = displayFwVersion();  // "" = not reported (pre-OTA display)
    doc["latest"]           = gLatest;
    doc["update_available"] = updateAvailable();
    JsonArray targets = doc["targets"].to<JsonArray>();
    if (displayNeedsUpdate()) targets.add("display");
    if (navNeedsUpdate()) targets.add("nav");
    doc["state"] = stateName(gState);

    int progress = 0;
    if (gState == State::DISPLAY_TRANSFER && gDisplayImage.size > 0) {
        progress = (int)((uint64_t)gAcked * 100 / gDisplayImage.size);
    } else if (gState == State::NAV_DOWNLOAD && gNavImage.size > 0) {
        progress = (int)((uint64_t)gWritten * 100 / gNavImage.size);
    }
    doc["progress"]  = progress;
    doc["error"]     = gError;
    doc["message"]   = gMessage;
    const char* blocked = blockedReason();
    if (blocked) doc["blocked_reason"] = blocked;
    else         doc["blocked_reason"] = nullptr;
    doc["releases"] = gReleases.is<JsonArray>() ? gReleases.as<JsonArray>() : JsonArray();

    String json;
    serializeJson(doc, json);
    return json;
}

}  // namespace ota
