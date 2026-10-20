#pragma once

#include <stdint.h>

#include <dpvlink.h>

#include "../config.h"
#include "stillness.h"

// Level gate for the 12-point heading cal: may BTN2 mark a point right now?
//
// READY once the unit has been level within HDG_CAL_LEVEL_TOL_DEG (pitch and
// roll, accel-only) and still (stillness::Window) for HDG_CAL_LEVEL_SETTLE_MS.
// Tilting or moving restarts the hold. Precedence: NOT_LEVEL over MOVING,
// because leveling is the thing to fix first and the attitude read while
// moving is noisy anyway.
//
// Runs on the nav board, which owns the sensors; the state travels to the
// display in NavPacket, and the display decides whether BTN2 marks.
// See config.h for the limits and HDG_CAL_LEVEL_GATE.
//
// Pure C++, no clock of its own: host-tested by tools/level_gate_test/.
namespace level_gate {

class Gate {
public:
    void reset();
    void tick(uint32_t nowMs, float ax, float ay, float az, float gyroDps);

    LevelGateState state() const { return state_; }
    // 0-100 through the settle hold; 100 at READY.
    uint8_t holdPct()  const { return holdPct_; }
    float   pitchDeg() const { return win_.pitchDeg(); }
    float   rollDeg()  const { return win_.rollDeg(); }
    uint8_t motion()   const { return win_.motion(); }

private:
    stillness::Window win_;
    LevelGateState state_ = LevelGateState::NOT_LEVEL;
    uint32_t holdSinceMs_ = 0;
    bool     holding_ = false;
    uint8_t  holdPct_ = 0;
};

// Pure predicate, shared with the display's horizon so the bracket colour and
// the gate agree.
inline bool isLevel(float pitchDeg, float rollDeg) {
    return pitchDeg >= -HDG_CAL_LEVEL_TOL_DEG && pitchDeg <= HDG_CAL_LEVEL_TOL_DEG &&
           rollDeg  >= -HDG_CAL_LEVEL_TOL_DEG && rollDeg  <= HDG_CAL_LEVEL_TOL_DEG;
}

}  // namespace level_gate
