#pragma once

#include <stdint.h>

#include "../config.h"

// The two softkeys on every calibration screen (docs/cal-screens-unified-plan.md).
//
//   BTN2 press             keep going: FINISH, MARK, ACCEPT, OK
//   BTN1 held LONG_PRESS_MS throw the work away: QUIT, REJECT. The left key
//                          fills while it is held.
//   BTN1 short press       nothing, except a hint ("Hold BTN1 to quit")
//
// Any press during which both buttons were down at once does nothing here. A
// two-button hold is the display reinit (handleModalButtons() checks it first),
// and a brief two-button tap must not finish a cal on the way: the BTN2 half of
// it would otherwise read as FINISH or MARK. For the same reason the hold only
// counts while BTN1 is down alone, and BTN2 joining cancels it for the rest of
// that press.
//
// Pure C++ with no Arduino dependency and no clock of its own, so
// tools/cal_keys_test/ runs the real thing on the host. The caller marks the
// buttons fired; this only reads them.
namespace cal_keys {

// The display's debounced ButtonState, reduced to what the gesture reads.
struct Button {
    bool     pressed;       // held now
    uint32_t pressStartMs;  // when the current or last press began; 0 = never pressed
    bool     fired;         // that press has already been acted on
};

struct Result {
    uint8_t holdPct;     // 0-100 through BTN1's hold, for the key's fill
    bool    discard;     // the hold reached LONG_PRESS_MS on this pass
    bool    shortPress;  // BTN1 released before LONG_PRESS_MS: show the hint
    bool    advance;     // BTN2 released: the right key's action
};

class Tracker {
public:
    void reset() {
        start1_ = start2_ = 0;
        chord1_ = chord2_ = false;
    }

    // Call once per button pass. When `discard` or `shortPress` is set the
    // caller marks BTN1 fired; when `advance` is set, BTN2.
    Result update(const Button& b1, const Button& b2, uint32_t nowMs) {
        // A new pressStartMs is a new press, which starts with no chord.
        if (b1.pressStartMs != start1_) { start1_ = b1.pressStartMs; chord1_ = false; }
        if (b2.pressStartMs != start2_) { start2_ = b2.pressStartMs; chord2_ = false; }
        if (b1.pressed && b2.pressed) chord1_ = chord2_ = true;

        Result r{};
        if (b1.pressStartMs != 0 && !b1.fired && !chord1_) {
            if (b1.pressed) {
                const uint32_t held = nowMs - b1.pressStartMs;
                if (held >= LONG_PRESS_MS) {
                    r.discard = true;
                    r.holdPct = 100;
                } else {
                    r.holdPct = (uint8_t)(held * 100 / LONG_PRESS_MS);
                }
            } else {
                r.shortPress = true;
            }
        }
        if (b2.pressStartMs != 0 && !b2.pressed && !b2.fired && !chord2_) {
            r.advance = true;
        }
        return r;
    }

private:
    uint32_t start1_ = 0, start2_ = 0;
    bool     chord1_ = false, chord2_ = false;
};

}  // namespace cal_keys
