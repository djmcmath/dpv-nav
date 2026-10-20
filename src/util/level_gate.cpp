#include "level_gate.h"

// See the header. No Arduino dependencies, for the host test.

namespace level_gate {

void Gate::reset() {
    win_.reset();
    state_       = LevelGateState::NOT_LEVEL;
    holdSinceMs_ = 0;
    holding_     = false;
    holdPct_     = 0;
}

void Gate::tick(uint32_t nowMs, float ax, float ay, float az, float gyroDps) {
    win_.tick(nowMs, ax, ay, az, gyroDps);

    // isLevel() is written so a NaN attitude fails it.
    if (!isLevel(win_.pitchDeg(), win_.rollDeg())) {
        holding_ = false;
        state_   = LevelGateState::NOT_LEVEL;
    } else if (!win_.still()) {
        holding_ = false;
        state_   = LevelGateState::MOVING;
    } else {
        if (!holding_) {
            holding_     = true;
            holdSinceMs_ = nowMs;
        }
        state_ = (nowMs - holdSinceMs_ >= HDG_CAL_LEVEL_SETTLE_MS) ? LevelGateState::READY
                                                                   : LevelGateState::SETTLING;
    }

    if (state_ == LevelGateState::READY) {
        holdPct_ = 100;
    } else if (state_ == LevelGateState::SETTLING) {
        holdPct_ = (uint8_t)(100u * (nowMs - holdSinceMs_) / HDG_CAL_LEVEL_SETTLE_MS);
    } else {
        holdPct_ = 0;
    }
}

}  // namespace level_gate
