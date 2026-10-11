#pragma once

#include <math.h>
#include <stdint.h>

#include <dpvlink.h>

#include "../config.h"

// Dwell capture for mounted magnetometer calibration.
//
// See divemap/docs/architecture/mounted-cal-dwell-plan.md, Part A. In short:
// the mounted fit takes "down" from the accelerometer, so a sample taken while
// the scooter is being handled carries a false tilt and the fit cannot use it.
// Until 2026-10-11 the collector took the first 18 samples that landed in a
// cell, about 0.2 s straight after the unit swung across a 30 deg boundary.
// Across eleven real collections the server could then use almost none of
// them. This replaces that rule:
//
//   A cell (row x 30 deg heading sector) is filled by ONE dwell. The unit must
//   be inside the row's attitude window and still for MAG_CAL_DWELL_SETTLE_MS;
//   it then records MAG_CAL_BIN_GREEN_THRESHOLD consecutive samples. Leaving
//   the window, moving, or drifting into another sector before the last one
//   discards the partial dwell. Passing through a cell records nothing.
//
// Rows are attitude windows, not bands that tile the pitch axis:
//
//   row 0  nose up     pitch +20..+40, roll within 15
//   row 1  level       pitch within 10, roll within 10
//   row 2  nose down   pitch -40..-20, roll within 15
//
// Pitch and roll are accel-only (mag_orient::pitchRollDeg), the same maths the
// server's fit uses, taken from the mean of the last 250 ms of accel. Heading
// sector comes from the caller (the AHRS heading); it only decides which cell
// a dwell fills, and the server judges coverage for itself from the samples.
//
// Pure C++ with no Arduino dependency and no clock of its own, so
// tools/mounted_dwell_test/ runs the real thing on the host.
namespace mounted_dwell {

constexpr int kSectors = MAG_CAL_MOUNTED_HDG_SECTORS;   // 12
constexpr int kRows    = MAG_CAL_MOUNTED_ELEV_BANDS;    // 3
constexpr int kCells   = MAG_CAL_MOUNTED_BINS;          // 36
constexpr int kRowUp = 0, kRowLevel = 1, kRowDown = 2;

// Cells the diver must fill: all of the level row, MAG_CAL_SECTORS_MID in each
// tilted row. The denominator of the on-screen count.
constexpr int kNeedTotal = MAG_CAL_SECTORS_LEVEL + 2 * MAG_CAL_SECTORS_MID;

// The three below are inline, like the completion helpers further down,
// because the display board draws from them and its build does not compile
// mounted_dwell.cpp.

// Row whose window (pitch, roll) is inside, or -1 between rows.
inline int rowFor(float pitchDeg, float rollDeg) {
    const float absRoll = fabsf(rollDeg);
    if (fabsf(pitchDeg) <= MAG_CAL_DWELL_LEVEL_TOL_DEG) {
        return absRoll <= MAG_CAL_DWELL_LEVEL_TOL_DEG ? kRowLevel : -1;
    }
    if (absRoll > MAG_CAL_DWELL_TILT_ROLL_TOL_DEG) return -1;
    if (fabsf(pitchDeg - MAG_CAL_DWELL_TILT_CENTER_DEG) <= MAG_CAL_DWELL_TILT_PITCH_TOL_DEG) return kRowUp;
    if (fabsf(pitchDeg + MAG_CAL_DWELL_TILT_CENTER_DEG) <= MAG_CAL_DWELL_TILT_PITCH_TOL_DEG) return kRowDown;
    return -1;
}

// Row the diver is evidently going for: the highlight follows this, and the
// attitude indicator draws this row's target, even while outside its window.
inline int nearestRow(float pitchDeg) {
    const float half = MAG_CAL_DWELL_TILT_CENTER_DEG * 0.5f;
    if (pitchDeg > half)  return kRowUp;
    if (pitchDeg < -half) return kRowDown;
    return kRowLevel;
}

// Pitch at the centre of a row's window: +30, 0, -30.
inline float rowTargetPitchDeg(int row) {
    if (row == kRowUp)   return MAG_CAL_DWELL_TILT_CENTER_DEG;
    if (row == kRowDown) return -MAG_CAL_DWELL_TILT_CENTER_DEG;
    return 0.0f;
}

// ---- Completion, over a per-cell array (nonzero = filled) ------------------
// Free functions over the counts array rather than Tracker members because the
// display has only CalProgressPacket::bin_counts and must reach the same
// verdict the nav board does.

inline int rowFilled(const uint8_t* cells, int row) {
    int n = 0;
    for (int c = 0; c < kSectors; c++) if (cells[row * kSectors + c]) n++;
    return n;
}

// Widest run of headings between two filled cells in a row, in sectors
// (filled neighbours = 1, every other one = 2). kSectors if fewer than two
// cells are filled.
inline int rowMaxGapSectors(const uint8_t* cells, int row) {
    int first = -1, prev = -1, widest = 0;
    for (int c = 0; c < kSectors; c++) {
        if (!cells[row * kSectors + c]) continue;
        if (first < 0) first = c;
        if (prev >= 0 && c - prev > widest) widest = c - prev;
        prev = c;
    }
    if (first < 0 || first == prev) return kSectors;
    const int wrap = first + kSectors - prev;
    return wrap > widest ? wrap : widest;
}

inline bool rowComplete(const uint8_t* cells, int row) {
    if (row == kRowLevel) return rowFilled(cells, row) >= MAG_CAL_SECTORS_LEVEL;
    return rowFilled(cells, row) >= MAG_CAL_SECTORS_MID &&
           rowMaxGapSectors(cells, row) <= MAG_CAL_DWELL_TILT_MAX_GAP_SECTORS;
}

inline bool allComplete(const uint8_t* cells) {
    return rowComplete(cells, kRowUp) && rowComplete(cells, kRowLevel) &&
           rowComplete(cells, kRowDown);
}

// Cells counted toward kNeedTotal. A tilted row with enough cells bunched on
// one side counts one short, so the number cannot read "24/24" while the
// collection is still open.
inline int progressCount(const uint8_t* cells) {
    int n = 0;
    for (int r = 0; r < kRows; r++) {
        const int need = (r == kRowLevel) ? MAG_CAL_SECTORS_LEVEL : MAG_CAL_SECTORS_MID;
        int have = rowFilled(cells, r);
        if (have > need) have = need;
        if (have == need && !rowComplete(cells, r)) have = need - 1;
        n += have;
    }
    return n;
}

// What the caller does with this tick's magnetometer sample.
enum class Action : uint8_t {
    NONE,    // not recording; drop any provisional samples
    TAKE,    // store it provisionally, at index captured() - 1 of the dwell
    COMMIT,  // store it, and the dwell's samples are now permanent
};

class Tracker {
public:
    void reset();

    // Call once per loop tick. Accel is calibrated, in g; gyroDps is the gyro
    // magnitude in deg/s; headingDeg is any angle (wrapped here); magIsNew is
    // false when the magnetometer has not produced a new reading since the
    // last tick (its ~80 Hz ODR is slower than the loop), so the same reading
    // is never recorded twice.
    Action tick(uint32_t nowMs, float ax, float ay, float az, float gyroDps,
                float headingDeg, bool magIsNew);

    CalDwellState state() const { return state_; }
    float   pitchDeg() const { return pitchDeg_; }
    float   rollDeg()  const { return rollDeg_; }
    // Cell to highlight: nearestRow() x the heading sector. Always valid.
    int     highlightCell() const { return highlightCell_; }
    // Samples taken so far in the dwell in progress; 0 when there is none.
    int     captured() const { return captured_; }
    // 1-based number of the dwell in progress or, on COMMIT, just finished.
    int     dwellNumber() const { return dwellsDone_ + (captured_ > 0 ? 1 : 0); }
    uint8_t motion() const { return motionHeld_; }
    // Which stillness test motion() is reporting.
    CalMotionTerm motionTerm() const { return motionTerm_; }
    uint8_t dwellPct() const { return dwellPct_; }

    const uint8_t* cells() const { return cells_; }
    bool complete() const { return allComplete(cells_); }

private:
    void abortDwell();

    float   ring_[MAG_CAL_DWELL_WINDOW_TICKS][3];
    int     ringHead_;
    int     ringFill_;

    uint8_t cells_[kCells];
    int     dwellsDone_;

    int      dwellCell_;      // cell the dwell in progress is filling, -1 if none
    uint32_t stillSinceMs_;
    int      captured_;

    CalDwellState state_;
    float    pitchDeg_, rollDeg_;
    int      highlightCell_;
    uint8_t  motionHeld_;
    CalMotionTerm motionTerm_;
    uint32_t motionHeldMs_;
    uint8_t  dwellPct_;
};

}  // namespace mounted_dwell
