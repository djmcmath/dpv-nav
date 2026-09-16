#pragma once

// Display side of the firmware transfer from nav (protocol: lib/dpvlink/ota_link.h).
//
// Header-only because platformio.ini (and its per-env build_src_filter) is
// untracked; include from display_main.cpp only.
//
// While active() this owns Serial1 and the screen: display_main.cpp's loop()
// hands every pass to update() and skips its own link parsing, buttons and
// rendering. The image goes into the inactive app slot and the boot partition
// only switches after the size and sha256 both match, so any failure leaves
// the running firmware untouched. A new image still has to prove itself on its
// first NavPacket (util/ota_confirm.h), or the bootloader rolls it back.

#include <Arduino.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include <dpvlink.h>
#include <ota_link.h>

#include "drivers/display.h"

namespace display_ota {

enum class Phase : uint8_t { OFF, RECEIVING, FAILED_HOLD };

static constexpr uint32_t FAIL_HOLD_MS   = 5000;  // leave the reason on screen this long
static constexpr uint32_t REDRAW_MS      = 400;
static constexpr uint32_t RESTART_DELAY_MS = 300; // let the OTA_DONE line leave the UART

static Phase                  gPhase = Phase::OFF;
static OtaBeginPacket         gBegin{};
static otalink::FrameParser   gParser;
static mbedtls_sha256_context gSha;
static bool                   gShaActive   = false;
static uint32_t               gWritten     = 0;
static uint16_t               gExpectSeq   = 0;
static uint32_t               gLastFrameMs = 0;
static uint32_t               gLastByteMs  = 0;
static uint32_t               gFailedAtMs  = 0;
static uint32_t               gLastDrawMs  = 0;
static int                    gLastPct     = -1;
static char                   gLine[160];  // JSON lines seen between frames (a retried "U")
static size_t                 gLinePos     = 0;

inline bool active() { return gPhase != Phase::OFF; }

static void sendReply(DisplayCmd cmd, bool ok, const char* err) {
    char buf[128];
    size_t n = otaReplyToBytes(cmd, ok, err, buf, sizeof(buf));
    if (n > 0) Serial1.write(buf, n);
}

static void sendAck(bool ok, uint16_t seq) {
    uint8_t buf[otalink::ACK_LEN];
    otalink::encodeAck(ok, seq, buf, sizeof(buf));
    Serial1.write(buf, sizeof(buf));
}

static void releaseSha() {
    if (gShaActive) {
        mbedtls_sha256_free(&gSha);
        gShaActive = false;
    }
}

static void fail(const char* why) {
    Update.abort();
    releaseSha();
    Serial.printf("[OTA] display update failed: %s\n", why);
    display::showFirmwareUpdate(gBegin.version, -1, why, /*error=*/true, /*full=*/true);
    gPhase      = Phase::FAILED_HOLD;
    gFailedAtMs = millis();
}

// Validates the begin packet and opens the update. Replies OTA_READY either way.
static void startTransfer(const OtaBeginPacket& pkt) {
    const char* refuse = nullptr;
    if (pkt.pv != otalink::PROTOCOL_VERSION) refuse = "Unsupported transfer protocol version";
    else if (pkt.size == 0) refuse = "Empty image";
    else if (strlen(pkt.sha256) != 64) refuse = "Bad sha256 in update request";
    if (refuse) {
        Serial.printf("[OTA] refusing update: %s\n", refuse);
        sendReply(DisplayCmd::OTA_READY, false, refuse);
        return;
    }
    if (!Update.begin(pkt.size, U_FLASH)) {
        const char* err = Update.errorString();
        Serial.printf("[OTA] Update.begin(%u) failed: %s\n", pkt.size, err);
        sendReply(DisplayCmd::OTA_READY, false, err);
        return;
    }

    gBegin = pkt;
    for (char* p = gBegin.sha256; *p; p++) *p = (char)tolower((unsigned char)*p);
    mbedtls_sha256_init(&gSha);
    mbedtls_sha256_starts_ret(&gSha, /*is224=*/0);
    gShaActive   = true;
    gWritten     = 0;
    gExpectSeq   = 0;
    gLinePos     = 0;
    gLastFrameMs = millis();
    gLastByteMs  = gLastFrameMs;
    gLastDrawMs  = gLastFrameMs;
    gLastPct     = 0;
    gParser.reset();
    gPhase = Phase::RECEIVING;

    Serial.printf("[OTA] receiving display %s (%u bytes)\n", gBegin.version, gBegin.size);
    display::showFirmwareUpdate(gBegin.version, 0, "Receiving firmware from nav...", false, true);
    sendReply(DisplayCmd::OTA_READY, true, nullptr);
}

// Called by display_main.cpp for an OTA_BEGIN line in normal mode.
inline void begin(const char* line, size_t len) {
    OtaBeginPacket pkt{};
    if (!bytesToOtaBeginPacket(line, len, pkt)) return;
    startTransfer(pkt);
}

static void finishTransfer() {
    if (gWritten != gBegin.size) {
        char why[64];
        snprintf(why, sizeof(why), "Image incomplete (%u of %u bytes)", gWritten, gBegin.size);
        sendReply(DisplayCmd::OTA_DONE, false, why);
        fail(why);
        return;
    }
    unsigned char digest[32];
    mbedtls_sha256_finish_ret(&gSha, digest);
    releaseSha();
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    if (strcmp(hex, gBegin.sha256) != 0) {
        Serial.printf("[OTA] sha256 mismatch: got %s want %s\n", hex, gBegin.sha256);
        sendReply(DisplayCmd::OTA_DONE, false, "Checksum mismatch");
        fail("Checksum mismatch -- nothing was installed");
        return;
    }
    // end() validates the image header and switches the boot partition.
    if (!Update.end()) {
        const char* err = Update.errorString();
        Serial.printf("[OTA] Update.end failed: %s\n", err);
        sendReply(DisplayCmd::OTA_DONE, false, err);
        fail(err);
        return;
    }
    Serial.printf("[OTA] display %s verified and installed -- restarting\n", gBegin.version);
    display::showFirmwareUpdate(gBegin.version, 100, "Installed. Restarting...", false, true);
    sendReply(DisplayCmd::OTA_DONE, true, nullptr);
    Serial1.flush();
    delay(RESTART_DELAY_MS);
    ESP.restart();
}

static void handleFrame() {
    uint16_t seq = gParser.seq();
    uint16_t len = gParser.len();

    if (seq == otalink::SEQ_ABORT && len == 0) {
        fail("Cancelled by nav (download problem?)");
        return;
    }
    if (len == 0) {
        if (seq == gExpectSeq) finishTransfer();
        return;  // a stale end frame: nav resends it if it's still waiting
    }
    if (seq == gExpectSeq) {
        if (gWritten + len > gBegin.size) {
            fail("Received more data than announced");
            return;
        }
        if (Update.write((uint8_t*)gParser.payload(), len) != len) {
            fail(Update.errorString());
            return;
        }
        mbedtls_sha256_update_ret(&gSha, gParser.payload(), len);
        gWritten += len;
        gExpectSeq++;
        gLastFrameMs = millis();
        sendAck(true, seq);
    } else if ((uint16_t)(seq + 1) == gExpectSeq) {
        // Nav missed our ACK and resent -- it's already written.
        gLastFrameMs = millis();
        sendAck(true, seq);
    } else {
        sendAck(false, seq);
    }
}

// Bytes outside a frame: collect lines, so a "U" nav retried because our
// OTA_READY got lost is answered again instead of timing nav out.
static void handleLineByte(uint8_t b) {
    if (b != '\n') {
        if (gLinePos < sizeof(gLine) - 1) gLine[gLinePos++] = (char)b;
        return;
    }
    gLine[gLinePos] = '\0';
    size_t len = gLinePos;
    gLinePos = 0;
    if (len == 0 || identifyPacket(gLine, len) != PacketType::OTA_BEGIN) return;

    OtaBeginPacket pkt{};
    if (!bytesToOtaBeginPacket(gLine, len, pkt)) return;
    if (gExpectSeq == 0 && pkt.size == gBegin.size && strcasecmp(pkt.sha256, gBegin.sha256) == 0) {
        sendReply(DisplayCmd::OTA_READY, true, nullptr);
        return;
    }
    // A different image: nav gave up on the old transfer and started over.
    // If this one is refused we just drop back to normal mode.
    Update.abort();
    releaseSha();
    gPhase = Phase::OFF;
    startTransfer(pkt);
}

inline void update() {
    if (gPhase == Phase::FAILED_HOLD) {
        // Nav resumes its normal stream after a failure; nothing here is for us.
        while (Serial1.available()) Serial1.read();
        if (millis() - gFailedAtMs >= FAIL_HOLD_MS) gPhase = Phase::OFF;
        return;
    }
    if (gPhase != Phase::RECEIVING) return;

    while (Serial1.available() && gPhase == Phase::RECEIVING) {
        uint8_t b = (uint8_t)Serial1.read();
        gLastByteMs = millis();
        bool wasMid = gParser.midFrame();
        otalink::FrameParser::Result r = gParser.feed(b);
        if (r == otalink::FrameParser::Result::FRAME) {
            handleFrame();
        } else if (r == otalink::FrameParser::Result::BAD_CRC) {
            sendAck(false, gParser.seq());
        } else if (!wasMid && !gParser.midFrame()) {
            handleLineByte(b);
        }
    }
    if (gPhase != Phase::RECEIVING) return;

    // Read the clock only now: handleFrame() above stamps gLastFrameMs with
    // millis(), and an earlier reading minus that later stamp wraps to ~49 days
    // -- which aborted every transfer right after its first frame.
    uint32_t now = millis();

    // A frame cut off mid-way (nav resends it after its ACK timeout).
    if (gParser.midFrame() && now - gLastByteMs > otalink::FRAME_GAP_RESET_MS) gParser.reset();

    if (now - gLastFrameMs > otalink::RECEIVER_IDLE_MS) {
        fail("Nav stopped sending (timed out)");
        return;
    }

    int pct = (int)((uint64_t)gWritten * 100 / gBegin.size);
    if (pct != gLastPct && now - gLastDrawMs >= REDRAW_MS) {
        gLastPct    = pct;
        gLastDrawMs = now;
        display::showFirmwareUpdate(gBegin.version, pct, nullptr, false, false);
    }
}

}  // namespace display_ota
