#include "mag_cal_orient.h"

#include <cmath>

// Verbatim port -- see the header for the convention warning and for why the
// Rodrigues formulation (not aircraft-Euler roll) is mandatory here.
// Deliberately free of Arduino/ESP32 dependencies so tools/orient_equivalence.py
// can host-compile it and diff it against coverage.py sample for sample.

namespace mag_orient {

namespace {
constexpr float kRad2Deg = 57.295779513082320876798154814105f;

// Shared accel-only pitch/roll split -- see reconstructOrientation()'s header
// comment and reconstructRoll() below, which is now a thin wrapper over this.
// Roll is genuinely undefined nose-vertical (pitch -> +-90 deg); returns 0.0
// there rather than amplifying noise through a near-zero-denominator atan2f.
void pitchRollRad(float ax, float ay, float az, float& pitchOut, float& rollOut) {
    // Gravity-down in the body frame. The accelerometer reports SPECIFIC
    // FORCE in a Z-down frame, so the reading is the negative of gravity in
    // ALL THREE components -- level reading az ~ -1g is the visible corner of
    // that, not a Z-only quirk. Negating only az (as this did until
    // 2026-08-26) inverts both pitch and roll everywhere off level.
    const float gx = -ax, gy = -ay, gz = -az;
    pitchOut = atan2f(-gx, hypotf(gy, gz));
    rollOut  = (hypotf(gy, gz) < 1e-6f) ? 0.0f : atan2f(gy, gz);
}
}  // namespace

void reconstructOrientation(float mx, float my, float mz,
                            float ax, float ay, float az,
                            float& pitchDegOut, float& headingDegOut) {
    float pitch, roll;
    pitchRollRad(ax, ay, az, pitch, roll);

    // Un-mirror the mag into the accel's right-handed NED frame. Callers pass
    // mag as readMagRaw() delivers it, which magMap{+1,-2,+3} leaves
    // left-handed in Y (nav_main.cpp documents this; its Mahony path does the
    // same negation as `magNED`). A rotation built from the accel is only
    // meaningful applied to a vector of the same handedness. This negation and
    // the atan2(-yh, xh) below CANCEL EXACTLY AT LEVEL -- do not "simplify"
    // either away after testing at level, which is the one attitude that
    // cannot tell them apart.
    my = -my;

    float xh, yh;
    const float u_norm = sqrtf(ax * ax + ay * ay + az * az);
    if (u_norm < 1e-9f) {
        // Degenerate (near-zero) accel reading: nothing to tilt-compensate
        // with. Fall back to raw mag rather than divide by ~0.
        xh = mx;
        yh = my;
    } else {
        // Undo roll (about the tracked/X axis), then undo pitch (about the
        // once-de-rolled Y axis) -- see coverage.py's reconstruct_orientation
        // docstring for the full derivation and why this replaced the
        // minimal-rotation formulation on 2026-08-26.
        const float cr = cosf(roll), sr = sinf(roll);
        const float my1 = my * cr - mz * sr;
        const float mz1 = my * sr + mz * cr;
        const float cp = cosf(pitch), sp = sinf(pitch);
        xh = mx * cp + mz1 * sp;
        yh = my1;
    }

    float heading = atan2f(-yh, xh) * kRad2Deg;
    heading = fmodf(heading, 360.0f);
    if (heading < 0.0f) heading += 360.0f;  // Python's % is floor-mod; C's fmodf is not

    pitchDegOut   = pitch * kRad2Deg;
    headingDegOut = heading;
}

int elevBand(float pitchDeg) {
    if (pitchDeg >= MAG_CAL_ELEV_H2) return 0;
    if (pitchDeg >= MAG_CAL_ELEV_H1) return 1;
    if (pitchDeg >= MAG_CAL_ELEV_L1) return 2;
    if (pitchDeg >= MAG_CAL_ELEV_L2) return 3;
    return 4;
}

int hdgSector(float headingDeg) {
    int s = (int)floorf(headingDeg / 30.0f);
    // Mirrors Python's `int(heading // 30) % 12`. reconstructOrientation
    // already returns [0, 360), but hdgSector() is public and callers may not
    // -- wrap rather than index out of bounds.
    s %= MAG_CAL_BASELINE_HDG_SECTORS;
    if (s < 0) s += MAG_CAL_BASELINE_HDG_SECTORS;
    return s;
}

int binIndex(float pitchDeg, float headingDeg) {
    return elevBand(pitchDeg) * MAG_CAL_BASELINE_HDG_SECTORS + hdgSector(headingDeg);
}

int binIndexFor(float mx, float my, float mz, float ax, float ay, float az) {
    float pitchDeg = 0.0f, headingDeg = 0.0f;
    reconstructOrientation(mx, my, mz, ax, ay, az, pitchDeg, headingDeg);
    return binIndex(pitchDeg, headingDeg);
}

float reconstructRoll(float ax, float ay, float az) {
    float pitch, rollRad;
    pitchRollRad(ax, ay, az, pitch, rollRad);
    float roll = rollRad * kRad2Deg;
    roll = fmodf(roll, 360.0f);
    if (roll < 0.0f) roll += 360.0f;  // Python's % is floor-mod; C's fmodf is not
    return roll;
}

int rollSector(float rollDeg) {
    int s = (int)floorf((rollDeg + 45.0f) / 90.0f);
    s %= MAG_CAL_ROLL_SECTORS;
    if (s < 0) s += MAG_CAL_ROLL_SECTORS;
    return s;
}

}  // namespace mag_orient
