#include "mounted_dwell.h"

#include <cmath>
#include <cstring>

#include "mag_cal_orient.h"

// See the header. Deliberately free of Arduino/ESP32 dependencies so
// tools/mounted_dwell_test/ can host-compile it.

namespace mounted_dwell {

void Tracker::reset() {
    memset(ring_, 0, sizeof(ring_));
    ringHead_ = 0;
    ringFill_ = 0;
    memset(cells_, 0, sizeof(cells_));
    dwellsDone_    = 0;
    dwellCell_     = -1;
    stillSinceMs_  = 0;
    captured_      = 0;
    state_         = CalDwellState::OUT_OF_WINDOW;
    pitchDeg_      = 0.0f;
    rollDeg_       = 0.0f;
    highlightCell_ = kRowLevel * kSectors;
    motionHeld_    = 0;
    motionTerm_    = CalMotionTerm::GYRO;
    motionHeldMs_  = 0;
    dwellPct_      = 0;
}

void Tracker::abortDwell() {
    dwellCell_ = -1;
    captured_  = 0;
}

Action Tracker::tick(uint32_t nowMs, float ax, float ay, float az, float gyroDps,
                     float headingDeg, bool magIsNew) {
    // --- Accel window: mean gives the attitude, spread gives the jitter -----
    ring_[ringHead_][0] = ax;
    ring_[ringHead_][1] = ay;
    ring_[ringHead_][2] = az;
    ringHead_ = (ringHead_ + 1) % MAG_CAL_DWELL_WINDOW_TICKS;
    if (ringFill_ < MAG_CAL_DWELL_WINDOW_TICKS) ringFill_++;

    float sum[3] = {0.0f, 0.0f, 0.0f};
    float lo[3], hi[3];
    for (int k = 0; k < 3; k++) lo[k] = hi[k] = ring_[0][k];
    for (int i = 0; i < ringFill_; i++) {
        for (int k = 0; k < 3; k++) {
            const float v = ring_[i][k];
            sum[k] += v;
            if (v < lo[k]) lo[k] = v;
            if (v > hi[k]) hi[k] = v;
        }
    }
    float jitter = 0.0f;
    for (int k = 0; k < 3; k++) {
        if (hi[k] - lo[k] > jitter) jitter = hi[k] - lo[k];
    }
    mag_orient::pitchRollDeg(sum[0] / ringFill_, sum[1] / ringFill_, sum[2] / ringFill_,
                             pitchDeg_, rollDeg_);

    // --- Stillness, as the worst term's fraction of its limit ---------------
    const float accelDev = fabsf(sqrtf(ax * ax + ay * ay + az * az) - 1.0f);
    float ratio = gyroDps / MAG_CAL_DWELL_GYRO_DPS;
    CalMotionTerm term = CalMotionTerm::GYRO;
    if (accelDev / MAG_CAL_DWELL_ACCEL_DEV_G > ratio) {
        ratio = accelDev / MAG_CAL_DWELL_ACCEL_DEV_G;
        term  = CalMotionTerm::ACCEL_MAG;
    }
    if (jitter / MAG_CAL_DWELL_ACCEL_JITTER_G > ratio) {
        ratio = jitter / MAG_CAL_DWELL_ACCEL_JITTER_G;
        term  = CalMotionTerm::ACCEL_JITTER;
    }
    // A non-finite reading compares false against everything; count it as
    // motion rather than letting it through as stillness.
    if (!(ratio >= 0.0f)) ratio = 1e6f;
    // Not still until the window has filled: a short window understates jitter.
    const bool still = ratio <= 1.0f && ringFill_ >= MAG_CAL_DWELL_WINDOW_TICKS;

    float scaled = ratio * (float)CAL_MOTION_AT_LIMIT;
    // Round up, so anything over the limit never reads as exactly on it.
    const uint8_t motionNow = scaled >= 255.0f ? (uint8_t)255 : (uint8_t)ceilf(scaled);
    if (motionNow >= motionHeld_ || nowMs - motionHeldMs_ >= MAG_CAL_DWELL_MOTION_HOLD_MS) {
        motionHeld_   = motionNow;
        motionTerm_   = term;
        motionHeldMs_ = nowMs;
    }

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
