// Host-side round-trip test for CalProgressPacket's GAP_FILL target packing.
// Compiles the REAL lib/dpvlink/dpvlink.cpp (ArduinoJson is plain C++ and
// builds on host), so this exercises the shipping encoder/decoder, not a copy.
//   see tools/dpvlink_test/run.sh
#include "../../lib/dpvlink/dpvlink.h"
#include "../../lib/dpvlink/ota_link.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <initializer_list>

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { printf("  FAIL: %s\n", what); failures++; }
}

static CalProgressPacket basePkt(uint8_t phase) {
    CalProgressPacket p{};
    p.cal_type = (uint8_t)CalType::BASELINE;
    p.phase = phase;
    p.bins_total = 60;
    p.bins_green = 7;
    p.current_bin = 31;
    p.cur_pitch_deg = -12.5f;
    p.cur_hdg_deg = 217.25f;
    p.sample_count = 431;
    for (int i = 0; i < 60; i++) p.bin_counts[i] = (uint8_t)(i % 19);
    return p;
}

int main() {
    char buf[512];

    // 1. Every 2-bit pattern survives the round trip, in the right cell.
    {
        CalProgressPacket tx = basePkt((uint8_t)CalPhase::GAP_FILL);
        tx.has_targets = true;
        for (int i = 0; i < 60; i++) tx.targets[i] = (uint8_t)(i % 4);
        size_t n = calProgressPacketToBytes(tx, buf, sizeof(buf));
        check(n > 0, "encode produced bytes");
        printf("  packet length with targets: %zu bytes (buffer %zu)\n", n, sizeof(buf));

        CalProgressPacket rx{};
        check(bytesToCalProgressPacket(buf, n, rx), "decode succeeded");
        check(rx.has_targets, "has_targets survived");
        for (int i = 0; i < 60; i++) {
            if (rx.targets[i] != tx.targets[i]) {
                printf("  FAIL: cell %d: %u != %u\n", i, rx.targets[i], tx.targets[i]);
                failures++;
                break;
            }
        }
        check(rx.current_bin == tx.current_bin, "current_bin unaffected");
        check(rx.bins_green == tx.bins_green, "bins_green unaffected");
        for (int i = 0; i < 60; i++) {
            if (rx.bin_counts[i] != tx.bin_counts[i]) { check(false, "bin_counts unaffected"); break; }
        }
    }

    // 2. Cell ordering is not symmetric under any transpose/reverse: exactly
    //    one cell set, and it must come back in exactly that slot.
    for (int probe : {0, 1, 3, 4, 11, 12, 47, 58, 59}) {
        CalProgressPacket tx = basePkt((uint8_t)CalPhase::GAP_FILL);
        tx.has_targets = true;
        tx.targets[probe] = 2;
        size_t n = calProgressPacketToBytes(tx, buf, sizeof(buf));
        CalProgressPacket rx{};
        bytesToCalProgressPacket(buf, n, rx);
        for (int i = 0; i < 60; i++) {
            uint8_t want = (i == probe) ? 2 : 0;
            if (rx.targets[i] != want) {
                printf("  FAIL: probe %d landed in cell %d\n", probe, i);
                failures++;
                break;
            }
        }
    }

    // 3. Non-GAP_FILL phases don't pay for the field at all.
    {
        CalProgressPacket tx = basePkt((uint8_t)CalPhase::ROUGH_SCAN);
        tx.has_targets = true;
        for (int i = 0; i < 60; i++) tx.targets[i] = 3;
        size_t n = calProgressPacketToBytes(tx, buf, sizeof(buf));
        check(strstr(buf, "\"tg\"") == nullptr, "no tg field outside GAP_FILL");
        CalProgressPacket rx{};
        bytesToCalProgressPacket(buf, n, rx);
        check(!rx.has_targets, "has_targets false when tg absent");
        for (int i = 0; i < 60; i++) if (rx.targets[i] != 0) { check(false, "targets zeroed when absent"); break; }
    }

    // 4. A GAP_FILL packet that never got targets doesn't emit a bogus field.
    {
        CalProgressPacket tx = basePkt((uint8_t)CalPhase::GAP_FILL);
        tx.has_targets = false;
        size_t n = calProgressPacketToBytes(tx, buf, sizeof(buf));
        check(strstr(buf, "\"tg\"") == nullptr, "no tg field when has_targets is false");
        CalProgressPacket rx{};
        bytesToCalProgressPacket(buf, n, rx);
        check(!rx.has_targets, "decoded has_targets stays false");
    }

    // 5. Corrupt / truncated tg is rejected wholesale. A half-decoded map
    //    would send the diver to cells nobody flagged, which is worse than
    //    showing no map at all.
    {
        const char* bad[] = {
            "{\"t\":\"C\",\"ct\":0,\"ph\":2,\"bt\":60,\"tg\":\"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\"}",
            "{\"t\":\"C\",\"ct\":0,\"ph\":2,\"bt\":60,\"tg\":\"abc\"}",
            "{\"t\":\"C\",\"ct\":0,\"ph\":2,\"bt\":60,\"tg\":\"\"}",
            "{\"t\":\"C\",\"ct\":0,\"ph\":2,\"bt\":60,\"tg\":\"00000000000000000000000000000g\"}",
        };
        for (const char* b : bad) {
            CalProgressPacket rx{};
            for (int i = 0; i < 60; i++) rx.targets[i] = 9;  // poison
            bool ok = bytesToCalProgressPacket(b, strlen(b), rx);
            check(ok, "malformed tg still parses the rest of the packet");
            check(!rx.has_targets, "malformed tg leaves has_targets false");
            for (int i = 0; i < 60; i++) if (rx.targets[i] != 0) { check(false, "malformed tg leaves targets zeroed"); break; }
        }
    }

    // 6. Uppercase hex decodes too (nothing emits it, but a hand-typed test
    //    packet over serial shouldn't silently produce a blank grid).
    {
        CalProgressPacket tx = basePkt((uint8_t)CalPhase::GAP_FILL);
        tx.has_targets = true;
        for (int i = 0; i < 60; i++) tx.targets[i] = (uint8_t)((i * 7) % 4);
        size_t n = calProgressPacketToBytes(tx, buf, sizeof(buf));
        for (size_t i = 0; i < n; i++) if (buf[i] >= 'a' && buf[i] <= 'f') buf[i] = (char)(buf[i] - 'a' + 'A');
        CalProgressPacket rx{};
        bytesToCalProgressPacket(buf, n, rx);
        // The uppercasing also hits the "t":"C" tag and field names, so only
        // assert on what we can: if it parsed as a cal packet, targets match.
        if (rx.has_targets) {
            for (int i = 0; i < 60; i++) if (rx.targets[i] != tx.targets[i]) { check(false, "uppercase hex decodes"); break; }
        }
    }

    // 7. Roll breakdown for the currently-highlighted cell round-trips
    //    alongside targets, guarded the same way (only when has_targets).
    {
        CalProgressPacket tx = basePkt((uint8_t)CalPhase::GAP_FILL);
        tx.has_targets = true;
        for (int i = 0; i < 60; i++) tx.targets[i] = (uint8_t)(i % 4);
        tx.current_bin_roll_counts[0] = 12;
        tx.current_bin_roll_counts[1] = 3;
        tx.current_bin_roll_counts[2] = 0;
        tx.current_bin_roll_counts[3] = 5;
        tx.current_bin_roll_targeted[0] = 0;  // ok
        tx.current_bin_roll_targeted[1] = 1;  // thin
        tx.current_bin_roll_targeted[2] = 2;  // empty
        tx.current_bin_roll_targeted[3] = 0;  // ok
        tx.current_roll_sector = 2;
        size_t n = calProgressPacketToBytes(tx, buf, sizeof(buf));
        check(n > 0, "encode with roll breakdown produced bytes");

        CalProgressPacket rx{};
        check(bytesToCalProgressPacket(buf, n, rx), "decode with roll breakdown succeeded");
        for (int k = 0; k < 4; k++) {
            if (rx.current_bin_roll_counts[k] != tx.current_bin_roll_counts[k]) {
                check(false, "current_bin_roll_counts round-trips");
                break;
            }
            if (rx.current_bin_roll_targeted[k] != tx.current_bin_roll_targeted[k]) {
                check(false, "current_bin_roll_targeted round-trips");
                break;
            }
        }
        check(rx.current_roll_sector == tx.current_roll_sector, "current_roll_sector round-trips");
    }

    // 8. Roll fields are absent (and default sensibly) outside GAP_FILL /
    //    when has_targets is false -- same bandwidth discipline as "tg".
    {
        CalProgressPacket tx = basePkt((uint8_t)CalPhase::ROUGH_SCAN);
        tx.has_targets = true;
        tx.current_bin_roll_counts[0] = 9;
        tx.current_roll_sector = 1;
        size_t n = calProgressPacketToBytes(tx, buf, sizeof(buf));
        check(strstr(buf, "\"rc\"") == nullptr, "no rc field outside GAP_FILL");
        check(strstr(buf, "\"rs\"") == nullptr, "no rs field outside GAP_FILL");

        CalProgressPacket rx{};
        for (int k = 0; k < 4; k++) rx.current_bin_roll_counts[k] = 9;  // poison
        rx.current_roll_sector = 9;
        bytesToCalProgressPacket(buf, n, rx);
        for (int k = 0; k < 4; k++) {
            if (rx.current_bin_roll_counts[k] != 0) { check(false, "roll counts default to zero when absent"); break; }
        }
        check(rx.current_roll_sector == -1, "roll sector defaults to -1 when absent");
    }

    // 9. LINK_HELLO carries the display FW_VERSION; still decodes as LINK_HELLO,
    //    and a pre-OTA bare LINK_HELLO yields an empty version.
    {
        char small[96];  // the display's real txBuf size
        size_t n = displayLinkHelloToBytes("12.345.6789", small, sizeof(small));
        check(n > 0, "LINK_HELLO with version fits 96-byte txBuf");
        DisplayCmd cmd = DisplayCmd::NONE;
        check(bytesToDisplayCmd(small, n, cmd) && cmd == DisplayCmd::LINK_HELLO,
              "versioned LINK_HELLO still decodes as LINK_HELLO");
        char ver[16];
        parseLinkVersion(small, n, ver, sizeof(ver));
        check(strcmp(ver, "12.345.6789") == 0, "LINK_HELLO version round-trips");

        n = displayCmdToBytes(DisplayCmd::LINK_HELLO, small, sizeof(small));
        strcpy(ver, "poison");
        parseLinkVersion(small, n, ver, sizeof(ver));
        check(ver[0] == '\0', "bare (pre-OTA) LINK_HELLO yields empty version");
    }

    // 10. BOOT_PING carries nav's FW_VERSION the same way, still identifies as
    //     a BOOT_PING, and fits nav's 32-byte pingBuf.
    {
        char ping[32];  // nav_main.cpp's real pingBuf size
        size_t n = bootPingToBytes("12.345.6789", ping, sizeof(ping));
        check(n > 0, "versioned BOOT_PING fits 32-byte pingBuf");
        check(identifyPacket(ping, n) == PacketType::BOOT_PING,
              "versioned BOOT_PING still identifies as BOOT_PING");
        char ver[16];
        parseLinkVersion(ping, n, ver, sizeof(ver));
        check(strcmp(ver, "12.345.6789") == 0, "BOOT_PING version round-trips");
    }

    // 11. Display firmware transfer: CRC, frames, ACKs, and the U/V/reply lines.
    {
        // Standard CRC-32 check value, and the running form agrees with one pass.
        const uint8_t digits[] = {'1','2','3','4','5','6','7','8','9'};
        check(otalink::crc32(digits, 9) == 0xCBF43926u, "crc32 matches the IEEE check value");
        check(otalink::crc32(digits + 4, 5, otalink::crc32(digits, 4)) == 0xCBF43926u,
              "crc32 can be continued across calls");

        uint8_t payload[otalink::MAX_PAYLOAD];
        for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 7 + 3);
        uint8_t frame[otalink::MAX_FRAME_LEN];
        size_t fn = otalink::encodeFrame(513, payload, otalink::MAX_PAYLOAD, frame, sizeof(frame));
        check(fn == otalink::MAX_FRAME_LEN, "full frame encodes to MAX_FRAME_LEN");
        check(otalink::encodeFrame(0, payload, otalink::MAX_PAYLOAD + 1, frame, sizeof(frame)) == 0,
              "oversize payload refused");

        // Leading garbage (a stray JSON line, a lone magic byte) is skipped.
        otalink::FrameParser fp;
        const char junk[] = "{\"t\":\"N\"}\n\xA5";
        for (size_t i = 0; i + 1 < sizeof(junk); i++)
            check(fp.feed((uint8_t)junk[i]) == otalink::FrameParser::Result::NONE, "junk yields nothing");
        otalink::FrameParser::Result r = otalink::FrameParser::Result::NONE;
        size_t frames = 0;
        for (size_t i = 0; i < fn; i++) {
            r = fp.feed(frame[i]);
            if (r != otalink::FrameParser::Result::NONE) frames++;
        }
        check(frames == 1 && r == otalink::FrameParser::Result::FRAME, "frame decodes after junk");
        check(fp.seq() == 513 && fp.len() == otalink::MAX_PAYLOAD &&
              memcmp(fp.payload(), payload, otalink::MAX_PAYLOAD) == 0, "frame fields round-trip");

        // One flipped payload bit -> BAD_CRC, and the parser recovers for the next frame.
        frame[100] ^= 0x10;
        for (size_t i = 0; i < fn; i++) r = fp.feed(frame[i]);
        check(r == otalink::FrameParser::Result::BAD_CRC, "corrupted payload reports BAD_CRC");
        frame[100] ^= 0x10;
        for (size_t i = 0; i < fn; i++) r = fp.feed(frame[i]);
        check(r == otalink::FrameParser::Result::FRAME, "parser recovers after a bad frame");

        // End-of-image and abort frames carry no payload.
        uint8_t endf[otalink::HEADER_LEN + otalink::CRC_LEN];
        size_t en = otalink::encodeFrame(otalink::SEQ_ABORT, nullptr, 0, endf, sizeof(endf));
        for (size_t i = 0; i < en; i++) r = fp.feed(endf[i]);
        check(r == otalink::FrameParser::Result::FRAME && fp.seq() == otalink::SEQ_ABORT && fp.len() == 0,
              "zero-length abort frame decodes");

        // A header claiming more than MAX_PAYLOAD is not a frame: resync, then decode the real one.
        const uint8_t fake[] = {otalink::MAGIC0, otalink::MAGIC1, 0, 0, 0xFF, 0x7F};
        for (uint8_t b : fake) fp.feed(b);
        for (size_t i = 0; i < en; i++) r = fp.feed(endf[i]);
        check(r == otalink::FrameParser::Result::FRAME, "oversize length header resyncs");

        // ACKs: round-trip, and resync past garbage including a false 'A' start.
        uint8_t ack[otalink::ACK_LEN];
        otalink::AckParser ap;
        const uint8_t noise[] = {'x', 'A', 0x00};
        for (uint8_t b : noise) check(!ap.feed(b), "ack noise yields nothing");
        otalink::encodeAck(false, 0x1234, ack, sizeof(ack));
        bool got = false;
        for (uint8_t b : ack) got = ap.feed(b) || got;
        check(got && !ap.ok() && ap.seq() == 0x1234, "NAK decodes after noise");
        otalink::encodeAck(true, 7, ack, sizeof(ack));
        got = false;
        for (uint8_t b : ack) got = ap.feed(b);
        check(got && ap.ok() && ap.seq() == 7, "ACK decodes");

        // JSON lines.
        OtaBeginPacket ub{};
        ub.pv = otalink::PROTOCOL_VERSION;
        ub.size = 1310719;
        memset(ub.sha256, 'a', 64);
        strcpy(ub.version, "123.456.789");
        char line[160];
        size_t ln = otaBeginPacketToBytes(ub, line, sizeof(line));
        check(ln > 0 && identifyPacket(line, ln) == PacketType::OTA_BEGIN, "U packet identifies");
        OtaBeginPacket ub2{};
        check(bytesToOtaBeginPacket(line, ln, ub2) && ub2.pv == 1 && ub2.size == 1310719 &&
              strcmp(ub2.sha256, ub.sha256) == 0 && strcmp(ub2.version, ub.version) == 0,
              "U packet round-trips");

        ln = updateHintToBytes("0.7.2", line, sizeof(line));
        char ver[16];
        parseLinkVersion(line, ln, ver, sizeof(ver));
        check(identifyPacket(line, ln) == PacketType::UPDATE_HINT && strcmp(ver, "0.7.2") == 0,
              "V packet identifies and carries the version");

        char small[96];  // the display's real txBuf size
        ln = otaReplyToBytes(DisplayCmd::OTA_DONE, false, "Checksum mismatch", small, sizeof(small));
        DisplayCmd dc = DisplayCmd::NONE;
        bool ok = true;
        char err[64];
        check(ln > 0 && bytesToDisplayCmd(small, ln, dc) && dc == DisplayCmd::OTA_DONE, "OTA_DONE decodes");
        check(parseOtaReply(small, ln, ok, err, sizeof(err)) && !ok && strcmp(err, "Checksum mismatch") == 0,
              "OTA reply carries ok + err");
        ln = otaReplyToBytes(DisplayCmd::OTA_READY, true, nullptr, small, sizeof(small));
        check(parseOtaReply(small, ln, ok, err, sizeof(err)) && ok && err[0] == '\0', "OTA_READY ok");
    }

    // 12. NavPacket's log-sync progress fields ride the same conditional-field
    //     convention as the cal fields: on the wire only while FLAG2_UPLOADING
    //     is set, and absent means zero rather than stale.
    {
        NavPacket tx{};
        tx.heading_deg = 191.5f;
        tx.flags2 = FLAG2_UPLOADING | FLAG2_WIFI_CLIENT;
        tx.log_sync_done  = 3;
        tx.log_sync_total = 7;
        size_t n = navPacketToBytes(tx, buf, sizeof(buf));
        check(n > 0, "nav encode produced bytes");

        NavPacket rx{};
        check(bytesToNavPacket(buf, n, rx), "nav decode succeeded");
        check((rx.flags2 & FLAG2_UPLOADING) != 0, "FLAG2_UPLOADING survived");
        check(rx.log_sync_done == 3, "log_sync_done survived");
        check(rx.log_sync_total == 7, "log_sync_total survived");

        // Not uploading: the fields stay off the wire, and a decoder holding a
        // previous packet's values must be reset to zero, not left showing
        // "3 of 7" after the pass ended.
        NavPacket idle{};
        idle.heading_deg = 191.5f;
        idle.flags2 = FLAG2_WIFI_CLIENT;
        idle.log_sync_done  = 3;
        idle.log_sync_total = 7;
        n = navPacketToBytes(idle, buf, sizeof(buf));
        check(strstr(buf, "\"ls\"") == nullptr, "no ls field when not uploading");
        check(strstr(buf, "\"lt\"") == nullptr, "no lt field when not uploading");

        NavPacket stale{};
        stale.log_sync_done  = 9;  // poison
        stale.log_sync_total = 9;
        bytesToNavPacket(buf, n, stale);
        check(stale.log_sync_done == 0, "log_sync_done defaults to zero when absent");
        check(stale.log_sync_total == 0, "log_sync_total defaults to zero when absent");
    }

    if (failures == 0) printf("  all dpvlink round-trip checks passed\n");
    return failures ? 1 : 0;
}
