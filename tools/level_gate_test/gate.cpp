// Host tests for the 12-point heading cal level gate (src/util/level_gate.h).
// Compiles the real source, not a copy -- see run.sh.
//
// The promise to the diver: BTN2 marks only a level, still, settled unit, and
// tilting or moving at any point restarts the hold.
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "../../src/util/level_gate.h"

using level_gate::Gate;

static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) { printf("  FAIL: %s\n", what); failures++; }
}

static constexpr float kDeg = 0.017453292519943295f;

struct Rig {
    Gate     g;
    uint32_t now = 1000;
    float pitch = 0, roll = 0, gyroDps = 0.5f;

    Rig() { g.reset(); }

    // Same convention as mounted_dwell_test: specific force, minus gravity.
    void step() {
        const float p = pitch * kDeg, r = roll * kDeg;
        now += 10;
        g.tick(now, sinf(p), -cosf(p) * sinf(r), -cosf(p) * cosf(r), gyroDps);
    }
    void run(int ms) { for (int i = 0; i < ms / 10; i++) step(); }
    LevelGateState s() const { return g.state(); }
};

static void testReadyAfterSettle() {
    Rig r;
    r.run(300);   // window fills (250 ms), hold starts
    check(r.s() == LevelGateState::SETTLING, "level + still starts the hold");
    r.run(HDG_CAL_LEVEL_SETTLE_MS - 400);
    check(r.s() == LevelGateState::SETTLING, "not READY before the settle time");
    check(r.g.holdPct() > 0 && r.g.holdPct() < 100, "hold bar part-way");
    r.run(400);
    check(r.s() == LevelGateState::READY, "READY after the settle time");
    check(r.g.holdPct() == 100, "hold bar full at READY");
}

static void testToleranceEdges() {
    for (float tilt : {2.5f, -2.5f}) {
        Rig r; r.pitch = tilt; r.run(HDG_CAL_LEVEL_SETTLE_MS + 500);
        check(r.s() == LevelGateState::READY, "pitch inside the window reaches READY");
        Rig q; q.roll = tilt; q.run(HDG_CAL_LEVEL_SETTLE_MS + 500);
        check(q.s() == LevelGateState::READY, "roll inside the window reaches READY");
    }
    for (float tilt : {3.5f, -3.5f}) {
        Rig r; r.pitch = tilt; r.run(HDG_CAL_LEVEL_SETTLE_MS + 500);
        check(r.s() == LevelGateState::NOT_LEVEL, "pitch past the window is NOT_LEVEL");
        Rig q; q.roll = tilt; q.run(HDG_CAL_LEVEL_SETTLE_MS + 500);
        check(q.s() == LevelGateState::NOT_LEVEL, "roll past the window is NOT_LEVEL");
    }
}

static void testTiltRestartsHold() {
    Rig r;
    r.run(HDG_CAL_LEVEL_SETTLE_MS + 500);
    check(r.s() == LevelGateState::READY, "READY before the tilt");
    r.roll = 6.0f; r.run(300);
    check(r.s() == LevelGateState::NOT_LEVEL, "tilting drops READY");
    r.roll = 0.0f; r.run(300);
    check(r.s() == LevelGateState::SETTLING, "back to level restarts the hold, not READY");
}

static void testMotionBlocksAndRestarts() {
    Rig r;
    r.run(HDG_CAL_LEVEL_SETTLE_MS - 200);
    r.gyroDps = 20.0f; r.run(100);   // turning, still level
    check(r.s() == LevelGateState::MOVING, "turning while level is MOVING");
    r.gyroDps = 0.5f; r.run(400);
    check(r.s() == LevelGateState::SETTLING, "the hold restarts after moving");
    r.run(HDG_CAL_LEVEL_SETTLE_MS);
    check(r.s() == LevelGateState::READY, "and completes from there");
}

static void testNotLevelBeatsMoving() {
    Rig r; r.pitch = 10.0f; r.gyroDps = 20.0f; r.run(500);
    check(r.s() == LevelGateState::NOT_LEVEL, "tilted and moving reads NOT_LEVEL");
}

static void testResetClears() {
    Rig r; r.run(HDG_CAL_LEVEL_SETTLE_MS + 500);
    r.g.reset();
    check(r.s() == LevelGateState::NOT_LEVEL && r.g.holdPct() == 0, "reset clears READY");
    r.step();
    check(r.s() != LevelGateState::READY, "one tick after reset is not READY");
}

int main() {
    printf("12-point heading cal level gate\n");
    testReadyAfterSettle();
    testToleranceEdges();
    testTiltRestartsHold();
    testMotionBlocksAndRestarts();
    testNotLevelBeatsMoving();
    testResetClears();
    if (failures) { printf("  %d failure(s)\n", failures); return 1; }
    printf("  all level gate checks passed\n");
    return 0;
}
