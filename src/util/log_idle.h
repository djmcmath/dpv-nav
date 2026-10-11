#pragma once

#include <cmath>
#include <cstdint>

// Idle detection for the gated log levels (LOW and MID). Pure C++ so
// tools/logidle_test/ can run it on the host.
//
// A dive's log is typically bracketed by half an hour of nothing at each end:
// the scooter sits on the beach, powered and logging, while the diver gears up,
// and again afterwards while they carry kit to the car. Those rows cost as much
// flash as the dive. While the unit is idle, logging.cpp drops from one row per
// second to one per LOG_IDLE_HEARTBEAT_MS.
//
// Idle means every one of these has held for `enterMs`:
//   - shallower than maxDepthM
//   - a live GPS position
//   - speed at or below maxSpeedMs
//   - position within maxDriftM of where the quiet spell began
// and it ends the instant any one fails. Every doubt resolves toward logging:
// no GPS (indoors, dive mode, GPS switched off) means never idle.
//
// The enterMs delay is also what keeps the surfacing fix. The first GPS rows
// after a dive are what the track correction anchors on, and they are all
// written at full rate before idle can begin.
namespace log_idle {

struct Params {
    float    maxDepthM;
    float    maxSpeedMs;
    float    maxDriftM;
    uint32_t enterMs;
};

struct Sample {
    uint32_t nowMs;
    float    depthM;
    float    speedMs;
    bool     hasGps;
    float    xM, yM;
};

class Gate {
public:
    // Feed every sample, logged or not. Returns true while idle.
    bool update(const Sample& s, const Params& p) {
        // NaN fails every comparison below, so it reads as not quiet.
        bool quiet = s.hasGps && s.depthM < p.maxDepthM && s.speedMs <= p.maxSpeedMs;
        if (quiet && quiet_) {
            float dx = s.xM - anchorX_, dy = s.yM - anchorY_;
            quiet = sqrtf(dx * dx + dy * dy) <= p.maxDriftM;
        }
        if (!quiet) {
            // A drift failure restarts the spell from here on the next sample
            // rather than staying broken forever at the new spot.
            quiet_ = false;
            idle_  = false;
            return false;
        }
        if (!quiet_) {
            quiet_   = true;
            sinceMs_ = s.nowMs;
            anchorX_ = s.xM;
            anchorY_ = s.yM;
        }
        idle_ = (s.nowMs - sinceMs_) >= p.enterMs;
        return idle_;
    }

    void reset() { quiet_ = false; idle_ = false; }
    bool idle() const { return idle_; }

private:
    bool     quiet_   = false;
    bool     idle_    = false;
    uint32_t sinceMs_ = 0;
    float    anchorX_ = 0.0f, anchorY_ = 0.0f;
};

// Decides which samples become rows: one per `activeMs` normally, one per
// `heartbeatMs` while idle.
//
// It also closes idle properly. The track processor integrates each row's
// speed over the time since the row before it, so the first row after an idle
// spell must not be up to a heartbeat late: a diver who pulls the trigger 50 s
// after the last heartbeat would be credited with 50 s of travel. The caller
// therefore keeps the newest sample it did not write (`hold`), and when idle
// ends writes that one first (`flushHeld`) -- a still-quiet row a tick before
// the unit moved -- so the row that follows spans one tick, not a minute.
class Pacer {
public:
    struct Plan {
        bool flushHeld;   // write the held sample before this one
        bool write;       // write this sample
        bool hold;        // not written: keep it as the held sample
    };

    Plan step(uint32_t nowMs, bool idle, uint32_t activeMs, uint32_t heartbeatMs) {
        Plan p{false, false, false};
        p.flushHeld = !idle && wasIdle_ && held_;
        wasIdle_ = idle;
        // A flushed still row is always followed at once by the row that ended idle.
        bool due = p.flushHeld || first_ || (nowMs - lastMs_) >= (idle ? heartbeatMs : activeMs);
        if (due) {
            p.write = true;
            first_  = false;
            held_   = false;
            lastMs_ = nowMs;
        } else if (idle) {
            p.hold = true;
            held_  = true;
        }
        return p;
    }

    void reset() { first_ = true; wasIdle_ = false; held_ = false; }

private:
    bool     first_   = true;
    bool     wasIdle_ = false;
    bool     held_    = false;
    uint32_t lastMs_  = 0;
};

}  // namespace log_idle
