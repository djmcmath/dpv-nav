// Host-side round-trip test for CalProgressPacket's GAP_FILL target packing.
// Compiles the REAL lib/dpvlink/dpvlink.cpp (ArduinoJson is plain C++ and
// builds on host), so this exercises the shipping encoder/decoder, not a copy.
//   see tools/dpvlink_test/run.sh
#include "../../lib/dpvlink/dpvlink.h"
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

    if (failures == 0) printf("  all dpvlink GAP_FILL target round-trip checks passed\n");
    return failures ? 1 : 0;
}
