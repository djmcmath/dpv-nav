#include "mounted_dwell.h"

#include <cmath>
#include <cstring>


// See the header. Deliberately free of Arduino/ESP32 dependencies so
// tools/mounted_dwell_test/ can host-compile it.

namespace mounted_dwell {

void Tracker::reset() {
    win_.reset();
    memset(cells_, 0, sizeof(cells_));
    dwellsDone_    = 0;
    dwellCell_     = -1;
    stillSinceMs_  = 0;
    captured_      = 0;
    state_         = CalDwellState::OUT_OF_WINDOW;
    highlightCell_ = kRowLevel * kSectors;
    dwellPct_      = 0;
}

void Tracker::abortDwell() {
    dwellCell_ = -1;
    captured_  = 0;
}

Action Tracker::tick(uint32_t nowMs, float ax, float ay, float az, float gyroDps,
                     float headingDeg, bool magIsNew) {
    // --- Attitude and stillness (util/stillness.h) --------------------------
    win_.tick(nowMs, ax, ay, az, gyroDps);
    const float pitchDeg_ = win_.pitchDeg();
    const float rollDeg_  = win_.rollDeg();
    const bool  still     = win_.still();

    // --- Which cell ---------------------------------------------------------
    float hdg = fmodf(headingDeg, 360.0f);
    if (!(hdg >= 0.0f)) hdg = (hdg < 0.0f) ? hdg + 360.0f : 0.0f;
    const float sectorW = 360.0f / (float)kSectors;
    int sector = (int)(hdg / sectorW);
    if (sector >= kSectors) sector = kSectors - 1;
    const float intoSector = hdg - (float)sector * sectorW;
    const bool nearEdge = intoSector < MAG_CAL_DWELL_HDG_GUARD_DEG ||
                          intoSector > sectorW - MAG_CAL_DWELL_HDG_GUARD_DEG;

    highlightCell_ = nearestRow(pitchDeg_) * kSectors + sector;
    const int row  = rowFor(pitchDeg_, rollDeg_);
    const int cell = (row < 0) ? -1 : row * kSectors + sector;

    // --- The dwell ----------------------------------------------------------
    Action action = Action::NONE;
    if (cell < 0) {
        abortDwell();
        state_ = CalDwellState::OUT_OF_WINDOW;
    } else if (cells_[cell]) {
        abortDwell();
        state_ = CalDwellState::CELL_DONE;
    } else if (!still) {
        abortDwell();
        state_ = CalDwellState::MOVING;
    } else if (dwellCell_ != cell) {
        // Still, in a window, over an open cell, and not yet dwelling on it.
        // The edge guard applies only here: once a dwell has started it keeps
        // the whole sector, which is the hysteresis.
        abortDwell();
        if (nearEdge) {
            state_ = CalDwellState::EDGE;
        } else {
            dwellCell_    = cell;
            stillSinceMs_ = nowMs;
            state_        = CalDwellState::SETTLING;
        }
    } else if (nowMs - stillSinceMs_ < MAG_CAL_DWELL_SETTLE_MS) {
        state_ = CalDwellState::SETTLING;
    } else {
        state_ = CalDwellState::CAPTURING;
        if (magIsNew) {
            captured_++;
            action = Action::TAKE;
            if (captured_ >= MAG_CAL_BIN_GREEN_THRESHOLD) {
                cells_[cell] = MAG_CAL_BIN_GREEN_THRESHOLD;
                dwellsDone_++;
                abortDwell();
                state_ = CalDwellState::CELL_DONE;
                action = Action::COMMIT;
            }
        }
    }

    // Settle is the first 60% of the bar, capture the rest.
    switch (state_) {
        case CalDwellState::SETTLING:
            dwellPct_ = (uint8_t)(60u * (nowMs - stillSinceMs_) / MAG_CAL_DWELL_SETTLE_MS);
            break;
        case CalDwellState::CAPTURING:
            dwellPct_ = (uint8_t)(60 + 40 * captured_ / MAG_CAL_BIN_GREEN_THRESHOLD);
            break;
        case CalDwellState::CELL_DONE:
            dwellPct_ = 100;
            break;
        default:
            dwellPct_ = 0;
            break;
    }
    return action;
}

}  // namespace mounted_dwell
