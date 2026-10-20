#include "stillness.h"

#include <cmath>
#include <cstring>

#include "mag_cal_orient.h"

// See the header. Deliberately free of Arduino/ESP32 dependencies so the host
// tests can compile it.

namespace stillness {

void Window::reset() {
    memset(ring_, 0, sizeof(ring_));
    ringHead_     = 0;
    ringFill_     = 0;
    pitchDeg_     = 0.0f;
    rollDeg_      = 0.0f;
    still_        = false;
    motionHeld_   = 0;
    motionTerm_   = CalMotionTerm::GYRO;
    motionHeldMs_ = 0;
}

void Window::tick(uint32_t nowMs, float ax, float ay, float az, float gyroDps) {
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
    still_ = ratio <= 1.0f && ringFill_ >= MAG_CAL_DWELL_WINDOW_TICKS;

    float scaled = ratio * (float)CAL_MOTION_AT_LIMIT;
    // Round up, so anything over the limit never reads as exactly on it.
    const uint8_t motionNow = scaled >= 255.0f ? (uint8_t)255 : (uint8_t)ceilf(scaled);
    if (motionNow >= motionHeld_ || nowMs - motionHeldMs_ >= MAG_CAL_DWELL_MOTION_HOLD_MS) {
        motionHeld_   = motionNow;
        motionTerm_   = term;
        motionHeldMs_ = nowMs;
    }
}

}  // namespace stillness
