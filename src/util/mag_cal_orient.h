#pragma once

#include "../config.h"

// Algebraic per-sample orientation for gap-fill calibration.
//
// This is a VERBATIM PORT of reconstruct_orientation() / _elev_band() /
// assign_bins() in
//   divemap/calibration-processor/callib/coverage.py
// and it must stay one. Gap-fill's whole premise is that the cell the device
// highlights is the same cell the website reported as thin -- if these two
// implementations ever disagree, the diver collects into the wrong bin and
// the feature is worse than useless. Any change here needs the matching
// change there, and `tools/orient_equivalence.py` re-run (it cross-checks
// this file against the Python on real 9-axis CSVs).
//
// -------------------------------------------------------------------------
// WHY THIS IS NOT THE SEVENTH FAILED ATTEMPT AT LIVE FEEDBACK
// -------------------------------------------------------------------------
// The six prior spikes (see imu.cpp's bin-cal comment block and
// docs/baseline-cal-two-pass.md) all fought the same chicken-and-egg: a live
// grid needs heading, heading needs a calibration, and no calibration exists
// yet during a *first* baseline cal. Attempt (3) got furthest and still
// failed on hardware -- no correlation between physical orientation and the
// highlighted box -- consistent with Mahony/AHRS lag and hysteresis under a
// fast tumble, not with a mapping bug.
//
// Gap-fill does not have that problem, and does not use that machinery:
//   * It runs ONLY with a good baseline cal already installed, so heading is
//     already trustworthy. That is a hard precondition, enforced by the
//     caller, not an assumption.
//   * No Mahony, no filter, no state, no lag: orientation is computed
//     algebraically from the current mag + accel reading alone.
//   * It is a point-and-hold motion, not a tumble, so rotation rate is low
//     regardless.
//
// -------------------------------------------------------------------------
// THE AXIS CONVENTION -- READ THIS BEFORE CALLING
// -------------------------------------------------------------------------
// coverage.py consumes the 9-axis CSV columns exactly as magBinCalDumpCSV()
// writes them, which means:
//
//   mag   -- logical-frame counts from readMagRaw() (post-axis-map, so mag Y
//            is ALREADY sign-flipped and the frame is left-handed for Y),
//            with the installed MagCalib applied: softIron * (raw - bias).
//   accel -- calibrated, in g, same logical frame.
//
// reconstructOrientation() then reconciles those two frames internally, which
// as of 2026-08-26 means it negates mag Y (un-mirroring it into the accel's
// right-handed NED frame) and negates ALL THREE accel components (the sensor
// reports specific force, so the reading is the negative of gravity, not just
// on Z). Pass the vectors as described above and let it do that; do not
// pre-negate anything at the call site.
//
// Before 2026-08-26 it negated only accel Z and left mag mirrored -- mixing a
// left-handed vector with a right-handed rotation. Both errors cancel exactly
// at level, so a level-only check certifies nothing here. That is not a
// hypothetical: the 2026-07-26 axis_test run sampled only level headings, the
// archived nav logs hold no more than +-5 deg of tilt, and the synthetic
// cross-checks build their samples from this same convention -- so the error
// shipped, worth up to 180 deg of heading and 90 deg of elevation at real
// tilt. Validate changes here against the captured hardware fixture
// (tools/fixtures/, replayed by tools/frame_fixture_check.py), whose ground
// truth comes from an operator and a compass rather than from this code.
//
// ***Never pass magNED here.*** magNED re-negates Y for the Mahony filter's
// cross-product math -- the exact opposite of what this function wants.
// Getting that backwards yields headings wrong by up to 180 deg (measured --
// tools/orient_equivalence.py prints the figure), which on hardware would look
// identical to the old failures. See dpv-nav/CLAUDE.md,
// "Magnetometer Coordinate Frame."
namespace mag_orient {

// One sample's orientation. `mx/my/mz` must already be bias/soft-iron
// corrected; `ax/ay/az` are calibrated accel in g, same logical frame.
// Heading is returned in [0, 360); pitch in [-90, +90].
//
// Tilt compensation explicitly undoes roll then pitch (reusing the same
// atan2(ay, az_n) reconstructRoll() computes standalone below), then reads
// heading off the mag vector's remaining X/Y components.
//
// From 2026-07-27 to 2026-08-26 this instead used a minimal (shortest-arc)
// rotation via Rodrigues' formula, which never forms an explicit roll angle
// -- adopted specifically to dodge the roll-then-pitch decomposition's
// gimbal-lock singularity as pitch -> +-90 deg (ay and az_n both -> 0 there).
// That formulation was only ever validated near roll = 0 (where it agrees
// with roll-then-pitch exactly); off roll = 0 it silently mixed roll into
// heading, by up to ~300 deg of swing for a pure roll change at fixed true
// pitch/heading. Invisible until gap-fill's roll-coverage widget -- the first
// flow to deliberately hold large rolls -- made it visible on real hardware
// as the highlighted target cell jumping to an unrelated heading sector on a
// pure roll motion. The pole singularity this dodged is real, but local, and
// already tolerated identically by reconstructRoll() below; a global heading
// error at every roll off zero is the far worse defect for a tool whose
// entire job is classifying orientation. See coverage.py's
// reconstruct_orientation docstring and dive-map's
// calibration-grid-conventions.md for the full writeup.
void reconstructOrientation(float mx, float my, float mz,
                            float ax, float ay, float az,
                            float& pitchDegOut, float& headingDegOut);

// Elevation band [0, 5): 0 = nose-up beyond +60 deg ... 4 = nose-down beyond
// -60 deg. Boundaries are MAG_CAL_ELEV_* from config.h, which coverage.py
// mirrors as _ELEV_L2/_ELEV_L1/_ELEV_H1/_ELEV_H2.
int elevBand(float pitchDeg);

// Heading sector [0, 12), 30 deg each, sector 0 centred on due north's
// leading edge (0-30 deg).
int hdgSector(float headingDeg);

// Flat bin index [0, 60) == elevBand * 12 + hdgSector -- the same row-major
// ordering as the website grid, CalProgressPacket::bin_counts, and
// showCalGrid()'s cell layout. All four must agree; they do so by using this
// one ordering everywhere.
int binIndex(float pitchDeg, float headingDeg);

// Convenience: calibrated mag + calibrated accel -> flat bin index.
int binIndexFor(float mx, float my, float mz, float ax, float ay, float az);

// -------------------------------------------------------------------------
// Roll coverage -- see config.h's "Roll coverage" block for the why. This is
// a VERBATIM PORT of reconstruct_roll() / _roll_sector() in
// divemap/calibration-processor/callib/coverage.py, same discipline as
// reconstructOrientation() above and re-checked by the same
// tools/orient_equivalence.py.
// -------------------------------------------------------------------------

// Roll (bank) angle in degrees, [0, 360). 0 = upright (canopy up), 90 =
// right side down, 180 = upside down, 270 = left side down.
//
// Roll needs only the accelerometer: pitch already consumes one of the two
// degrees of freedom in the measured "down" direction (its elevation
// relative to gravity); roll is exactly the *other* one -- rotation about
// the tracked/nose axis. This is the textbook `atan2(ay, az_n)` aircraft-roll
// formula, computed once in pitchRollRad() and reused by
// reconstructOrientation() above to undo roll before reading off heading.
// Ill-conditioned near pitch = +-90 deg (ay and az_n both -> 0 there), but
// that isn't a bug to fix: roll is genuinely undefined pointing straight
// up/down, the same way heading is undefined at a magnetic pole. Returns 0.0
// (arbitrary, deterministic -- "upright" by convention) in that degenerate
// case rather than dividing near-zero by near-zero.
float reconstructRoll(float ax, float ay, float az);

// Roll sector [0, MAG_CAL_ROLL_SECTORS): 0 = upright, 1 = right-side,
// 2 = upside-down, 3 = left-side. Sector 0 is centred on 0 deg (boundaries
// at +-45 deg), matching dive-map's _roll_sector.
int rollSector(float rollDeg);

}  // namespace mag_orient
