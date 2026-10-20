#pragma once

#include <stdint.h>

#include <dpvlink.h>

#include "../config.h"

// Attitude and "is the unit being held still", from the last quarter second of
// calibrated accel plus the gyro magnitude. One definition shared by every cal
// that waits for a still unit -- mounted dwell capture (mounted_dwell.h) and
// the 12-point heading cal's level gate (level_gate.h) -- so "still" cannot
// mean two different things on one unit.
//
// Pitch and roll are accel-only (mag_orient::pitchRollDeg) over the mean of the
// window, the same maths the server's mounted fit uses. Still means all three
// MAG_CAL_DWELL_* terms are under their limits: gyro magnitude, |a| away from
// 1 g, and peak-to-peak accel over the window, and the window has filled.
//
// Pure C++, no clock of its own: host-tested via tools/mounted_dwell_test/ and
// tools/level_gate_test/.
namespace stillness {

class Window {
public:
    void reset();

    // Once per loop tick. Accel is calibrated, in g; gyroDps is the gyro
    // magnitude in deg/s.
    void tick(uint32_t nowMs, float ax, float ay, float az, float gyroDps);

    float pitchDeg() const { return pitchDeg_; }
    float rollDeg()  const { return rollDeg_; }
    bool  still()    const { return still_; }
    // Worst stillness term as a fraction of its limit, scaled so
    // CAL_MOTION_AT_LIMIT is exactly on it. Holds its peak for
    // MAG_CAL_DWELL_MOTION_HOLD_MS so an on-screen bar does not flicker.
    uint8_t       motion()     const { return motionHeld_; }
    CalMotionTerm motionTerm() const { return motionTerm_; }

private:
    float    ring_[MAG_CAL_DWELL_WINDOW_TICKS][3];
    int      ringHead_ = 0;
    int      ringFill_ = 0;
    float    pitchDeg_ = 0.0f, rollDeg_ = 0.0f;
    bool     still_ = false;
    uint8_t  motionHeld_ = 0;
    CalMotionTerm motionTerm_ = CalMotionTerm::GYRO;
    uint32_t motionHeldMs_ = 0;
};

}  // namespace stillness
