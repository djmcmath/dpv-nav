// Host tests for mounted dwell capture (src/util/mounted_dwell.h). Compiles
// the real source, not a copy -- see run.sh.
//
// What these pin down is the promise the screen makes to the diver: a cell
// fills only from a still unit inside its row's attitude window, and nothing
// else puts a sample in the file.
#include <cmath>
#include <cstdio>

#include "../../src/util/mounted_dwell.h"

using mounted_dwell::Action;
using mounted_dwell::Tracker;

static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) { printf("  FAIL: %s\n", what); failures++; }
}

static constexpr float kDeg = 0.017453292519943295f;
static constexpr int   N    = MAG_CAL_BIN_GREEN_THRESHOLD;

// One simulated unit: holds an attitude, feeds the tracker at the 100 Hz loop
// rate, and counts what the tracker told the caller to keep.
struct Rig {
    Tracker  t;
    uint32_t now = 1000;
    float pitch = 0, roll = 0, hdg = 15, gyroDps = 0.5f, accelScale = 1.0f;
    float wobbleG = 0;        // +- on the z axis, alternating every tick
    bool  magNew = true;
    int   committed = 0;      // samples made permanent
    int   provisional = 0;    // samples in the dwell in progress
    int   lastDwell = 0;
    int   tickNo = 0;

    Rig() { t.reset(); }

    // Calibrated accel for this attitude, in mag_orient's convention: the
    // sensor reports specific force, i.e. minus gravity, in all three axes.
    void accel(float& ax, float& ay, float& az) const {
        const float p = pitch * kDeg, r = roll * kDeg;
        ax = sinf(p) * accelScale;
        ay = -cosf(p) * sinf(r) * accelScale;
        az = -cosf(p) * cosf(r) * accelScale;
    }

    Action step() {
        float ax, ay, az;
        accel(ax, ay, az);
        az += (tickNo++ % 2 ? wobbleG : -wobbleG);
        now += 10;
        const Action a = t.tick(now, ax, ay, az, gyroDps, hdg, magNew);
        if (a == Action::NONE) {
            provisional = 0;
        } else {
            provisional++;
            lastDwell = t.dwellNumber();
            if (a == Action::COMMIT) { committed += provisional; provisional = 0; }
        }
        return a;
    }
    void run(int ms) { for (int i = 0; i < ms / 10; i++) step(); }

    // Swing to an attitude with the gyro reading a handling rate, then let go.
    void moveTo(float p, float r, float h) {
        gyroDps = 25.0f;
        pitch = p; roll = r; hdg = h;
        run(300);
        gyroDps = 0.5f;
    }
    bool filled(int row, int sector) const { return t.cells()[row * 12 + sector] != 0; }
};

int main() {
    printf("mounted dwell capture\n");

    // --- The attitude windows ----------------------------------------------
    using namespace mounted_dwell;
    check(rowFor(0, 0) == kRowLevel,      "level is the level row");
    check(rowFor(9.9f, -9.9f) == kRowLevel, "level window reaches 10 deg on both axes");
    check(rowFor(10.5f, 0) == -1,         "pitch 10.5 is outside the level window");
    check(rowFor(0, 10.5f) == -1,         "roll 10.5 is outside the level window");
    check(rowFor(0, -10.5f) == -1,        "roll -10.5 is outside the level window");
    check(rowFor(15, 0) == -1,            "between rows is no row");
    check(rowFor(30, 14) == kRowUp,       "nose up 30, roll 14 is the nose-up row");
    check(rowFor(30, -16) == -1,          "nose-up row stops at 15 deg of roll");
    check(rowFor(19, 0) == -1 && rowFor(41, 0) == -1, "nose-up row is 20..40");
    check(rowFor(-30, 0) == kRowDown,     "nose down 30 is the nose-down row");
    check(nearestRow(14) == kRowLevel && nearestRow(16) == kRowUp && nearestRow(-16) == kRowDown,
          "nearest row switches halfway between targets");

    // --- The orientation maths this rests on -------------------------------
    // Both bank directions must read as a small signed roll. The server's
    // level window took abs() of a [0, 360) roll and dropped one of them.
    {
        Rig r;
        r.roll = -5;  r.run(400);
        check(fabsf(r.t.rollDeg() - (-5.0f)) < 0.05f, "left bank reads as negative roll");
        r.roll = 5;   r.run(400);
        check(fabsf(r.t.rollDeg() - 5.0f) < 0.05f, "right bank reads as positive roll");
        r.roll = 0; r.pitch = 30; r.run(400);
        check(fabsf(r.t.pitchDeg() - 30.0f) < 0.05f, "nose up reads as positive pitch");
    }

    // --- One dwell fills one cell with exactly N samples --------------------
    {
        Rig r;
        r.run(250);   // window fills
        check(r.committed == 0 && r.provisional == 0, "nothing recorded before the settle time");
        check(r.t.state() == CalDwellState::SETTLING, "still and level reads as settling");
        r.run(350);
        check(r.committed == 0, "nothing is permanent part-way through a dwell");
        r.run(300);
        check(r.committed == N, "a held dwell commits exactly one cell's samples");
        check(r.filled(kRowLevel, 0), "the cell under the unit is filled");
        check(r.lastDwell == 1, "first dwell is numbered 1");
        check(r.t.state() == CalDwellState::CELL_DONE, "a filled cell reads as done");
        check(r.t.dwellPct() == 100, "hold bar is full on a filled cell");
        r.run(2000);
        check(r.committed == N, "sitting on a filled cell records nothing more");

        // A second cell, and the dwell number moves on.
        r.moveTo(0, 0, 45);
        r.run(1000);
        check(r.committed == 2 * N && r.filled(kRowLevel, 1), "second heading fills the next cell");
        check(r.lastDwell == 2, "second dwell is numbered 2");
    }

    // --- Passing through records nothing ------------------------------------
    {
        Rig r;
        r.gyroDps = 25.0f;
        for (int h = 0; h < 360; h += 3) { r.hdg = (float)h; r.step(); }
        check(r.committed == 0, "sweeping through every heading in motion records nothing");
        check(r.t.state() == CalDwellState::MOVING, "level but turning reads as moving");
        check(r.t.motion() > CAL_MOTION_AT_LIMIT, "motion bar shows over the limit while turning");
        check(r.t.motionTerm() == CalMotionTerm::GYRO, "and names the gyro as the reason");
    }

    // --- Each stillness term, on its own, blocks capture --------------------
    {
        Rig r;  r.gyroDps = 5.5f;  r.run(1500);
        check(r.committed == 0, "gyro just over the limit blocks capture");
        r.gyroDps = 4.5f;  r.run(1500);
        check(r.committed == N, "gyro just under the limit allows it");
    }
    {
        Rig r;  r.accelScale = 1.03f;  r.run(1500);
        check(r.committed == 0, "accel 0.03 g off 1 g blocks capture");
        check(r.t.motionTerm() == CalMotionTerm::ACCEL_MAG, "and names accel magnitude as the reason");
    }
    {
        Rig r;  r.wobbleG = 0.015f;  r.run(1500);   // 0.03 g peak to peak
        check(r.committed == 0, "accel jitter over the limit blocks capture");
        check(r.t.state() == CalDwellState::MOVING, "jitter reads as moving");
        check(r.t.motionTerm() == CalMotionTerm::ACCEL_JITTER, "and names accel jitter as the reason");
    }
    {
        Rig r;  r.accelScale = NAN;  r.run(1500);
        check(r.committed == 0, "a non-finite accel reading never counts as still");
    }

    // --- A dwell that is interrupted leaves nothing behind ------------------
    {
        Rig r;
        r.run(250 + 400 + 80);        // settled, part-way through capture
        check(r.provisional > 0 && r.provisional < N, "capture is under way");
        r.gyroDps = 12.0f;  r.step();
        check(r.provisional == 0 && r.committed == 0, "moving mid-capture discards the dwell");
        check(!r.filled(kRowLevel, 0), "the cell stays open");
        r.gyroDps = 0.5f;
        r.run(300);
        check(r.committed == 0, "capture does not resume without a fresh settle");
        r.run(600);
        check(r.committed == N, "and completes once held again");
    }
    {
        Rig r;
        r.run(250 + 400 + 80);
        r.pitch = 12;                  // tips out of the level window
        r.run(400);
        check(r.committed == 0, "leaving the window mid-capture discards the dwell");
        check(r.t.state() == CalDwellState::OUT_OF_WINDOW, "and reads as out of window");
    }

    // --- The magnetometer is slower than the loop ---------------------------
    {
        Rig r;
        r.magNew = false;
        r.run(3000);
        check(r.committed == 0, "a repeated mag reading is never recorded");
        check(r.t.state() == CalDwellState::CAPTURING, "it waits in capture for a new reading");
    }

    // --- Heading-sector edges ----------------------------------------------
    {
        Rig r;  r.hdg = 29.0f;  r.run(1500);
        check(r.committed == 0, "a dwell will not start at a sector edge");
        check(r.t.state() == CalDwellState::EDGE, "and says so");
    }
    {
        Rig r;                         // starts mid-sector, heading 15
        r.run(250 + 200);
        r.hdg = 28.8f;                 // drifts into the guard band, same sector
        r.run(600);
        check(r.committed == N && r.filled(kRowLevel, 0), "a dwell under way keeps its whole sector");
    }
    {
        Rig r;
        r.run(250 + 400 + 80);
        r.hdg = 31.0f;                 // crosses into the next sector
        r.step();
        check(r.provisional == 0 && r.committed == 0, "crossing into the next sector discards the dwell");
    }
    {
        Rig r;  r.hdg = -345.0f;  r.run(1000);   // same heading as +15
        check(r.filled(kRowLevel, 0), "negative headings wrap");
    }

    // --- Tilted rows --------------------------------------------------------
    {
        Rig r;
        r.moveTo(30, 0, 105);  r.run(1000);
        check(r.filled(kRowUp, 3), "nose up 30 fills the nose-up row");
        r.moveTo(-30, 8, 195); r.run(1000);
        check(r.filled(kRowDown, 6), "nose down 30 fills the nose-down row");
        r.moveTo(15, 0, 285);  r.run(1000);
        check(r.committed == 2 * N, "between rows records nothing");
    }

    // --- Gap measure ---------------------------------------------------------
    // Fixed cell patterns, independent of MAG_CAL_SECTORS_MID: the gap rule is
    // what keeps a tilted row from completing bunched on one side whenever the
    // cell count is under 12.
    {
        uint8_t c[36] = {};
        for (int s = 0; s < 6; s++) c[kRowUp * 12 + s] = 18;          // bunched: sectors 0..5
        check(rowMaxGapSectors(c, kRowUp) == 7, "six bunched cells leave a 210 deg gap");
        c[kRowUp * 12 + 8] = 18;                                      // 5 -> 8 -> 0: gaps of 3 and 4
        check(rowMaxGapSectors(c, kRowUp) == 4, "5 -> 8 -> 0 leaves a 120 deg gap");
        c[kRowUp * 12 + 10] = 18;                                     // 8 -> 10 -> 0: gap of 2
        check(rowMaxGapSectors(c, kRowUp) == 3, "8 -> 10 -> 0 closes it to 90 deg");

        for (int s = 0; s < 12; s += 2) c[kRowDown * 12 + s] = 18;    // every other sector
        check(rowMaxGapSectors(c, kRowDown) == 2, "every other sector is a 60 deg gap");
        for (int s = 1; s < 12; s += 2) c[kRowDown * 12 + s] = 18;
        check(rowMaxGapSectors(c, kRowDown) == 1, "a full row has no gap");

        uint8_t one[36] = {};
        one[kRowUp * 12 + 4] = 18;
        check(rowMaxGapSectors(one, kRowUp) == 12, "a single cell has no gap to measure");
        uint8_t none[36] = {};
        check(rowMaxGapSectors(none, kRowUp) == 12, "an empty row has no gap to measure");
    }

    // --- Completion ---------------------------------------------------------
    // In terms of MAG_CAL_SECTORS_MID, so changing it (the backlog's tilted-row
    // analysis) needs no test rewrite. kSpread holds MID sectors spaced evenly
    // round the row, which is a complete tilted row at any MID from 4 to 12.
    constexpr int kMid = MAG_CAL_SECTORS_MID;
    static_assert(kMid >= 4 && kMid <= 12, "the spread rows below assume 4..12 cells");
    int kSpread[12];
    for (int i = 0; i < kMid; i++) kSpread[i] = i * 12 / kMid;
    {
        check(kNeedTotal == MAG_CAL_SECTORS_LEVEL + 2 * kMid, "kNeedTotal is the level row plus both tilted rows");

        uint8_t c[36] = {};
        for (int s = 0; s < 12; s++) c[kRowLevel * 12 + s] = 18;
        check(rowComplete(c, kRowLevel), "twelve level cells complete the level row");
        check(!allComplete(c), "the level row alone is not a complete collection");
        check(progressCount(c) == MAG_CAL_SECTORS_LEVEL, "and counts the level row only");

        for (int i = 0; i < kMid - 1; i++) c[kRowDown * 12 + kSpread[i]] = 18;
        check(!rowComplete(c, kRowDown), "one cell short of MID does not complete a tilted row");
        check(progressCount(c) == MAG_CAL_SECTORS_LEVEL + kMid - 1, "and counts what is there");
        c[kRowDown * 12 + kSpread[kMid - 1]] = 18;
        check(rowComplete(c, kRowDown), "MID spread cells complete a tilted row");
        check(!allComplete(c), "still open while the other tilted row is empty");

        // MID cells bunched at one end of the row. When that leaves a gap over
        // the limit (any MID under 10), the row stays open and counts one short;
        // at 10 or more the bunch IS spread enough, and the row completes.
        for (int s = 0; s < kMid; s++) c[kRowUp * 12 + s] = 18;
        if (12 - kMid + 1 > MAG_CAL_DWELL_TILT_MAX_GAP_SECTORS) {
            check(!rowComplete(c, kRowUp), "MID bunched cells do not complete a tilted row");
            check(progressCount(c) == MAG_CAL_SECTORS_LEVEL + 2 * kMid - 1,
                  "a bunched row counts one short of MID");
            for (int s = kMid; s < 12; s++) c[kRowUp * 12 + s] = 18;
        }
        check(rowComplete(c, kRowUp), "the nose-up row completes");
        check(allComplete(c) && progressCount(c) == kNeedTotal, "all three rows complete reads kNeedTotal");

        uint8_t full[36];
        for (int i = 0; i < 36; i++) full[i] = 18;
        check(progressCount(full) == kNeedTotal, "a full grid never counts past kNeedTotal");
    }

    // --- A whole collection, end to end -------------------------------------
    {
        Rig r;
        for (int s = 0; s < 12; s++) { r.moveTo(0, 2, 15.0f + 30.0f * s); r.run(900); }
        check(!r.t.complete(), "level ring alone does not complete");
        for (int i = 0; i < kMid; i++) { r.moveTo(30, -4, 15.0f + 30.0f * kSpread[i]); r.run(900); }
        check(!r.t.complete(), "level ring plus one tilted row does not complete");
        for (int i = 0; i < kMid - 1; i++) { r.moveTo(-30, 4, 15.0f + 30.0f * kSpread[i]); r.run(900); }
        check(!r.t.complete(), "one cell short does not complete");
        r.moveTo(-30, 4, 15.0f + 30.0f * kSpread[kMid - 1]); r.run(900);
        check(r.t.complete(), "the level ring and MID spread dwells per tilted row complete the collection");
        check(r.committed == kNeedTotal * N, "with exactly kNeedTotal cells' worth of samples");
        check(r.lastDwell == kNeedTotal, "numbered 1 to kNeedTotal");
    }

    // --- Motion bar ---------------------------------------------------------
    {
        Rig r;
        r.run(1000);
        check(r.t.motion() <= CAL_MOTION_AT_LIMIT, "a still unit reads at or under the limit");
        r.gyroDps = 15.0f;  r.step();
        const uint8_t peak = r.t.motion();
        check(peak == 3 * CAL_MOTION_AT_LIMIT, "three times the gyro limit reads three times the mark");
        r.gyroDps = 0.5f;   r.run(200);
        check(r.t.motion() == peak, "the peak is held so the bar does not flicker");
        r.run(200);
        check(r.t.motion() < peak, "and released after the hold time");
        r.gyroDps = 500.0f; r.step();
        check(r.t.motion() == 255, "motion saturates instead of wrapping");
    }

    if (failures == 0) printf("  all mounted dwell checks passed\n");
    return failures ? 1 : 0;
}
