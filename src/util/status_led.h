#pragma once

#include <cstdint>

// The nav board's onboard NeoPixel (Feather ESP32 V2). Pure C++ down to the
// `#ifdef ARDUINO` line so tools/statusled_test/ can run it on the host.
//
// It answers one question before descent -- "is a good GPS fix in my log
// yet?" -- and otherwise only says "still alive". Highest priority first:
//
//   Halted     boot self-test failed            fast magenta. Shown at any depth:
//                                               if the display link is what
//                                               failed, this is nav's only output.
//   Heartbeat  dive mode, or GPS usage off      20% green, 50 ms every 4 s, only
//                                               while heading is finite. A blocked
//                                               or NaN-poisoned loop goes dark.
//   Fault      no NMEA from the module          red/yellow swap
//   Searching  no fresh fix                     slow red
//   Poor       fewer than the accept floor      fast red
//   Rejected   the confidence gate refuses it   slow amber
//   Accepted   nav is taking GPS position, but  green blink
//              no 'G' row is in the log yet
//   Logged     an accepted 'G' row is in the    solid green
//              current file since the GPS last
//              (re)started
//
// The colours follow the GPS-housing indicator (gps_indicator_main.cpp) so one
// legend covers both, but the meaning differs at the top: that board's solid
// green is "a good fix held 10 s", this one's is "a good fix written to the
// log". Logging OFF therefore tops out at green blink -- a prompt to turn it on.
//
// "Accepted" uses the same test as the log's 'G' tag (nav_main.cpp), so the
// LED can never call a fix good that the log would record as an estimate.
namespace status_led {

struct Inputs {
    uint32_t nowMs;
    bool     halted;             // SystemState::ERROR
    bool     diveMode;
    bool     gpsUsageOn;         // CONFIG > GPS
    bool     healthy;            // this loop's heading is finite
    bool     gpsInitOk;          // gps::init() heard the module at boot
    uint32_t gpsEnabledSinceMs;  // when the module was last powered up
    uint32_t gpsLastSentenceMs;  // last parsed NMEA since then, 0 = none yet
    bool     gpsFresh;           // a fix younger than GPS_FIX_STALE_MS
    uint8_t  bars;               // gps::computeSignalBars()
    uint8_t  minBars;            // GPS_POS_MIN_ACCEPT_BARS
    uint8_t  requiredBars;       // the confidence gate's current requirement
    bool     accepted;           // the gate accepted a position within GPS_FIX_STALE_MS
    bool     logged;             // logging::gpsRowWrittenSince(GPS session start)
};

enum class State : uint8_t {
    Halted, Heartbeat, Fault, Searching, Poor, Rejected, Accepted, Logged
};

struct Color {
    uint8_t r, g, b;
    bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const Color& o) const { return !(*this == o); }
};

// --- Timing and levels -----------------------------------------------------
// GPS fault thresholds match gps_indicator_main.cpp: a cold module gets a few
// seconds to say anything, but one that was talking and goes quiet is gone.
static constexpr uint32_t NMEA_BOOT_TIMEOUT_MS  = 8000;
static constexpr uint32_t NMEA_STALE_TIMEOUT_MS = 5000;

static constexpr uint32_t ALARM_HALF_MS = 100;   // fault / halted
static constexpr uint32_t SLOW_HALF_MS  = 500;   // searching / rejected
static constexpr uint32_t FAST_HALF_MS  = 125;   // poor
static constexpr uint32_t GOOD_HALF_MS  = 250;   // accepted, not yet logged

static constexpr uint32_t HEARTBEAT_ON_MS     = 50;
static constexpr uint32_t HEARTBEAT_PERIOD_MS = 4000;

// Absolute channel values -- the strip runs at full library brightness so
// these are exactly what the LED is sent. GPS states at 60/255 (the
// GPS-housing indicator's setting); the heartbeat at 20%. Tune through the
// real housing.
static constexpr uint8_t LEVEL     = 60;
static constexpr uint8_t HEARTBEAT = 51;

static constexpr Color OFF     {0, 0, 0};
static constexpr Color RED     {LEVEL, 0, 0};
static constexpr Color AMBER   {LEVEL, (uint8_t)(LEVEL * 180 / 255), 0};
static constexpr Color GREEN   {0, LEVEL, 0};
static constexpr Color MAGENTA {LEVEL, 0, LEVEL};
static constexpr Color BEAT    {0, HEARTBEAT, 0};

inline State classify(const Inputs& in) {
    if (in.halted) return State::Halted;
    if (in.diveMode || !in.gpsUsageOn) return State::Heartbeat;

    // Unsigned subtraction: correct across millis() wrap, and a sentence
    // stamped after nowMs was sampled cannot occur (same loop pass).
    const bool neverHeard = in.gpsLastSentenceMs == 0;
    if (!in.gpsInitOk ||
        (neverHeard && in.nowMs - in.gpsEnabledSinceMs >= NMEA_BOOT_TIMEOUT_MS) ||
        (!neverHeard && in.nowMs - in.gpsLastSentenceMs >= NMEA_STALE_TIMEOUT_MS)) {
        return State::Fault;
    }

    if (in.accepted) return in.logged ? State::Logged : State::Accepted;
    if (!in.gpsFresh || in.bars == 0) return State::Searching;
    if (in.bars < in.minBars) return State::Poor;
    return State::Rejected;
}

inline bool blinkOn(uint32_t nowMs, uint32_t halfMs) {
    return (nowMs % (2 * halfMs)) < halfMs;
}

inline Color render(State s, uint32_t nowMs, bool healthy) {
    switch (s) {
        case State::Halted:    return blinkOn(nowMs, ALARM_HALF_MS) ? MAGENTA : OFF;
        case State::Heartbeat: return (healthy && nowMs % HEARTBEAT_PERIOD_MS < HEARTBEAT_ON_MS)
                                      ? BEAT : OFF;
        case State::Fault:     return blinkOn(nowMs, ALARM_HALF_MS) ? RED : AMBER;
        case State::Searching: return blinkOn(nowMs, SLOW_HALF_MS) ? RED : OFF;
        case State::Poor:      return blinkOn(nowMs, FAST_HALF_MS) ? RED : OFF;
        case State::Rejected:  return blinkOn(nowMs, SLOW_HALF_MS) ? AMBER : OFF;
        case State::Accepted:  return blinkOn(nowMs, GOOD_HALF_MS) ? GREEN : OFF;
        case State::Logged:    return GREEN;
    }
    return OFF;
}

inline Color compute(const Inputs& in) {
    return render(classify(in), in.nowMs, in.healthy);
}

#ifdef ARDUINO
// Power the pixel and release the deep-sleep latch left by sleepForPowerOff().
// Call first thing in setup(), so a halted boot can still show itself.
void init();

// Send `c` to the pixel if it differs from what is already showing. Cheap to
// call every loop pass: the RMT transfer only happens on a change.
void show(Color c);
inline void update(const Inputs& in) { show(compute(in)); }

// Black, then cut the pixel's power rail and hold it cut through deep sleep.
void sleepForPowerOff();
#endif

}  // namespace status_led
