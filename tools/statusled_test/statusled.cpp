// Host-side test for the nav board's status LED. Compiles the REAL
// src/util/status_led.h and the REAL GPS thresholds from src/config.h.
//   see tools/statusled_test/run.sh
#include "../../src/config.h"
#include "../../src/util/status_led.h"

#include <cstdio>

using namespace status_led;

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { printf("  FAIL: %s\n", what); failures++; }
}

// Surfaced, GPS on, module talking, fix accepted and logged: the best case.
// Each test knocks out one thing from here.
static Inputs good(uint32_t now = 60000) {
    Inputs in{};
    in.nowMs             = now;
    in.gpsUsageOn        = true;
    in.healthy           = true;
    in.gpsInitOk         = true;
    in.gpsEnabledSinceMs = 1000;
    in.gpsLastSentenceMs = now - 200;
    in.gpsFresh          = true;
    in.bars              = 4;
    in.minBars           = GPS_POS_MIN_ACCEPT_BARS;
    in.requiredBars      = 4;
    in.accepted          = true;
    in.logged            = true;
    return in;
}

// Fraction of a window during which the LED is lit, sampled every ms.
static int litMs(Inputs in, uint32_t from, uint32_t span) {
    int lit = 0;
    for (uint32_t t = from; t < from + span; t++) {
        in.nowMs = t;
        in.gpsLastSentenceMs = t - 200;
        if (compute(in) != OFF) lit++;
    }
    return lit;
}

int main() {
    printf("classification, one condition at a time\n");
    {
        check(classify(good()) == State::Logged, "accepted + logged is solid green");

        Inputs in = good(); in.logged = false;
        check(classify(in) == State::Accepted, "accepted but not logged blinks green");

        in = good(); in.accepted = false; in.requiredBars = 4; in.bars = 3;
        check(classify(in) == State::Rejected, "above the floor but under the gate is amber");

        in = good(); in.accepted = false; in.bars = GPS_POS_MIN_ACCEPT_BARS - 1;
        check(classify(in) == State::Poor, "under the accept floor is fast red");

        in = good(); in.accepted = false; in.bars = 0;
        check(classify(in) == State::Searching, "zero bars is searching");

        in = good(); in.accepted = false; in.gpsFresh = false;
        check(classify(in) == State::Searching, "no fresh fix is searching");
    }

    printf("logged never shows without an accepted fix\n");
    {
        Inputs in = good(); in.accepted = false; in.bars = 3;
        check(classify(in) == State::Rejected, "a logged row from earlier doesn't make a rejected fix green");
    }

    printf("GPS fault\n");
    {
        Inputs in = good(); in.gpsInitOk = false;
        check(classify(in) == State::Fault, "init failure is a fault");

        in = good(); in.gpsLastSentenceMs = 0; in.gpsEnabledSinceMs = in.nowMs - 1000;
        in.accepted = false; in.gpsFresh = false;
        check(classify(in) == State::Searching, "cold module gets a grace period");

        in.gpsEnabledSinceMs = in.nowMs - NMEA_BOOT_TIMEOUT_MS;
        check(classify(in) == State::Fault, "silent past the boot timeout is a fault");

        in = good(); in.gpsLastSentenceMs = in.nowMs - (NMEA_STALE_TIMEOUT_MS - 1);
        check(classify(in) == State::Logged, "just under the stale timeout is still fine");
        in.gpsLastSentenceMs = in.nowMs - NMEA_STALE_TIMEOUT_MS;
        check(classify(in) == State::Fault, "a module that went quiet is a fault, even mid-fix");

        in = good(0x00000100); in.gpsLastSentenceMs = 0xFFFFFF00;  // millis() wrapped
        check(classify(in) == State::Logged, "stale test survives millis() wrap");
    }

    printf("priority\n");
    {
        Inputs in = good(); in.halted = true; in.diveMode = true;
        check(classify(in) == State::Halted, "halted beats dive mode");

        in = good(); in.diveMode = true; in.gpsInitOk = false;
        check(classify(in) == State::Heartbeat, "dive mode beats a GPS fault (GPS is off down there)");

        in = good(); in.gpsUsageOn = false;
        check(classify(in) == State::Heartbeat, "GPS usage off shows the heartbeat");
    }

    printf("heartbeat\n");
    {
        Inputs in = good(); in.diveMode = true;
        int lit = litMs(in, 0, HEARTBEAT_PERIOD_MS * 3);
        check(lit == (int)HEARTBEAT_ON_MS * 3, "lit exactly HEARTBEAT_ON_MS per period");

        in.nowMs = 0;
        check(compute(in) == BEAT, "heartbeat colour is BEAT");
        check(BEAT.g == 51 && BEAT.r == 0 && BEAT.b == 0, "BEAT is 20% green");

        in.healthy = false;
        check(litMs(in, 0, HEARTBEAT_PERIOD_MS * 3) == 0, "unhealthy heading suppresses the heartbeat");

        in = good(); in.halted = true; in.healthy = false;
        check(litMs(in, 0, 1000) > 0, "halted still flashes regardless of heading");
    }

    printf("rendering\n");
    {
        Inputs in = good();
        check(litMs(in, 0, 2000) == 2000, "logged is solid");

        in.logged = false;
        int lit = litMs(in, 0, 2000);
        check(lit == 1000, "accepted blinks at 50% duty");

        in = good(); in.gpsInitOk = false;
        check(litMs(in, 0, 2000) == 2000, "fault never goes dark (red/amber swap)");
    }

    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("all passed\n");
    return 0;
}
