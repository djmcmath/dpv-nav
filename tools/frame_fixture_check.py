#!/usr/bin/env python3
"""Check the orientation math against REAL HARDWARE captured at KNOWN attitudes.

This is the only instrument in the project whose ground truth does not come
from the code being tested.

  * tools/orient_equivalence.py proves the firmware port and callib/coverage.py
    agree with EACH OTHER. It is fully synthetic, and it builds its samples
    from the same convention the code assumes -- so when both sides shared an
    identical axis-convention bug (2026-08-26: mag delivered Y-mirrored,
    consumed alongside right-handed accel), it passed on all 12,247 samples.
  * tests/test_coverage.py's roll-invariance and recovery tests have the same
    blind spot for the same reason.
  * The archived nav logs never exceed +-5 deg of pitch or roll, so no amount
    of replaying them exercises the tilt conventions at all.

The fixture this reads is produced by the `axis_test` serial command, which
walks an operator through a fixed set of physically known attitudes and records
what the sensors actually reported at each. Ground truth is the operator and a
compass, which is exactly the point.

Usage:
    python3 frame_fixture_check.py [fixture.csv] [fixture_cal.json]

Defaults to tools/fixtures/frame_fixture.csv and frame_fixture_cal.json.
Exit status is 0 only if every pose is recovered within tolerance.
"""
import csv
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "dive-map", "calibration-processor"))

from callib import coverage  # noqa: E402

FIXTURE_DIR = os.path.join(HERE, "fixtures")
DEFAULT_CSV = os.path.join(FIXTURE_DIR, "frame_fixture.csv")
DEFAULT_CAL = os.path.join(FIXTURE_DIR, "frame_fixture_cal.json")

# Tolerances. These are deliberately loose: the ground truth is a human holding
# a device next to a phone compass, and the numbers only ever get consumed
# through 30-degree bins. They are sized to catch a convention error (which is
# worth 90 or 180 degrees), not to certify precision.
#
# Gravity-snapped poses rest against a surface, so their pitch and roll are
# exact to within sensor noise and get a tight bound. Everything about heading,
# and everything about a hand-held pose, inherits the operator's aim.
TOL_TILT_SNAPPED_DEG = 8.0
TOL_TILT_HANDHELD_DEG = 20.0
TOL_HDG_DEG = 25.0

# Dip is a property of the location: it must not change with attitude. This is
# the single most diagnostic number here, and it needs no ground-truth heading.
TOL_DIP_SPREAD_DEG = 8.0

# Field magnitude must be constant across attitudes for ANY frame convention,
# so this measures the magnetometer calibration alone. Above this, heading and
# dip cannot be judged -- they inherit the calibration's error.
MAX_MAG_NORM_SPREAD_PCT = 12.0

SNAPPED = {"level_015", "level_105", "level_195", "level_285", "inverted_015",
           "right_side_015", "left_side_015", "nose_up_90", "nose_down_90"}

# Roll and heading are both genuinely undefined with the tracked axis vertical,
# the same way compass heading is undefined at the geographic poles. Those poses
# are still captured -- they pin the accel X sign, which nothing else does as
# cleanly -- but only their pitch is checked.
POLE_POSES = {"nose_up_90", "nose_down_90"}


def circ_err(a, b):
    return abs((a - b + 180.0) % 360.0 - 180.0)


def _hdg_sector(heading_deg):
    """Kept identical to the expression inlined in coverage.assign_bins --
    there is no named helper there to import."""
    return int(heading_deg // 30.0) % coverage.HDG_SECTORS


def load_fixture(csv_path, cal_path):
    with open(cal_path) as f:
        cal = json.load(f)
    bias = cal["bias"]
    soft = cal["soft_iron"]

    rows = []
    with open(csv_path, newline="") as f:
        for rec in csv.DictReader(f):
            rows.append({
                "pose": rec["pose"],
                "hdg": float(rec["true_hdg_deg"]),
                "pitch": float(rec["true_pitch_deg"]),
                "roll": float(rec["true_roll_deg"]),
                "m": (float(rec["mx"]), float(rec["my"]), float(rec["mz"])),
                "a": (float(rec["ax"]), float(rec["ay"]), float(rec["az"])),
                "verified": rec.get("verified", "1") == "1",
                "still": rec.get("still", "1") == "1",
            })
    return rows, bias, soft


def calibrate(m, bias, soft):
    c = [m[i] - bias[i] for i in range(3)]
    return tuple(sum(soft[r][k] * c[k] for k in range(3)) for r in range(3))


def dip_of(mc, a):
    """Angle of the field below horizontal, from this sample alone.

    Uses the SAME frame reconciliation reconstruct_orientation does -- mag Y
    un-mirrored into the accel's right-handed NED frame -- so this measures
    the frame the math actually computes in, not the one the sensors ship.
    """
    m = (mc[0], -mc[1], mc[2])
    an = math.sqrt(sum(v * v for v in a))
    mn = math.sqrt(sum(v * v for v in m))
    if an < 1e-6 or mn < 1e-6:
        return float("nan")
    d = sum(m[i] * -a[i] for i in range(3)) / (an * mn)
    return math.degrees(math.asin(max(-1.0, min(1.0, d))))


def main():
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    cal_path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_CAL

    if not os.path.exists(csv_path) or not os.path.exists(cal_path):
        print(f"No fixture found at:\n  {csv_path}\n  {cal_path}\n")
        print("Capture one by running the `axis_test` serial command on the nav")
        print("device and saving the two blocks it prints at the end.")
        return 2

    rows, bias, soft = load_fixture(csv_path, cal_path)
    failures = []
    notes = []

    # ------------------------------------------------- calibration quality gate
    #
    # Split deliberately: pitch and roll come from the accelerometer ALONE, so
    # they are assessable no matter how bad the magnetometer calibration is.
    # Heading and dip are not. Reporting a heading failure caused by a bad cal
    # as though it were a frame error is how you end up chasing the wrong bug --
    # so the mag-dependent checks get gated on this and reported as
    # INCONCLUSIVE rather than FAIL when the calibration cannot support them.
    print("=" * 78)
    print("CALIBRATION QUALITY -- is the mag cal good enough to judge heading?")
    print("=" * 78)
    norms = []
    for r in rows:
        mc = calibrate(r["m"], bias, soft)
        norms.append(math.sqrt(sum(v * v for v in mc)))
    mean_n = sum(norms) / len(norms)
    n_spread = 100.0 * (max(norms) - min(norms)) / mean_n
    print("  Field magnitude is a property of your location: |m| must be the")
    print("  same at every attitude, whatever the frame conventions are.")
    print(f"\n  |m| mean {mean_n:.0f}   min {min(norms):.0f}   max {max(norms):.0f}"
          f"   spread {n_spread:.1f}%")
    cal_ok = n_spread <= MAX_MAG_NORM_SPREAD_PCT
    if cal_ok:
        print(f"  PASS -- within {MAX_MAG_NORM_SPREAD_PCT:.0f}%.")
    else:
        print(f"  BAD CALIBRATION -- {n_spread:.1f}% exceeds {MAX_MAG_NORM_SPREAD_PCT:.0f}%.")
        print("  Heading and dip below are INCONCLUSIVE: they cannot be better than")
        print("  the calibration they are computed from. Pitch and roll are still")
        print("  authoritative -- they never touch the magnetometer.")
        print("  Re-run a baseline calibration, then re-capture the fixture.")

    # ---------------------------------------------------------------- dip check
    print()
    print("=" * 78)
    print("DIP CONSISTENCY -- is the field the same angle from gravity at every pose?")
    print("=" * 78)
    dips = []
    for r in rows:
        mc = calibrate(r["m"], bias, soft)
        d = dip_of(mc, r["a"])
        dips.append((r["pose"], d))
        print(f"  {r['pose']:<18} {d:+7.1f}")
    vals = [d for _, d in dips if not math.isnan(d)]
    spread = max(vals) - min(vals)
    lo = min(dips, key=lambda x: x[1])
    hi = max(dips, key=lambda x: x[1])
    print(f"\n  spread {spread:.1f} deg   (low {lo[0]} {lo[1]:+.1f}, high {hi[0]} {hi[1]:+.1f})")
    if spread > TOL_DIP_SPREAD_DEG:
        msg = (f"dip spread {spread:.1f} deg exceeds {TOL_DIP_SPREAD_DEG} -- "
               "mag and accel are not in the same frame")
        if cal_ok:
            failures.append(msg)
            print("  FAIL -- dip cannot depend on how the unit is held.")
        else:
            notes.append(msg + " (inconclusive: calibration is bad)")
            print("  INCONCLUSIVE -- too large, but a bad calibration alone can")
            print("  produce this. Fix the calibration before reading anything")
            print("  into it. A frame error shows up as near-symmetric sign FLIPS")
            print("  (e.g. +70 at level, -70 on-side); a bad cal as ragged scatter.")
    else:
        print("  PASS")

    # -------------------------------------------------------- per-pose recovery
    print()
    print("=" * 78)
    print("POSE RECOVERY -- does reconstruct_orientation() return what was held?")
    print("=" * 78)
    print(f"{'pose':<18}{'pitch true/got':>22}{'hdg true/got':>22}{'roll true/got':>22}")
    for r in rows:
        mc = calibrate(r["m"], bias, soft)
        pitch, hdg = coverage.reconstruct_orientation(*mc, *r["a"])
        roll = coverage.reconstruct_roll(*r["a"])

        snapped = r["pose"] in SNAPPED
        tilt_tol = TOL_TILT_SNAPPED_DEG if snapped else TOL_TILT_HANDHELD_DEG
        pole = r["pose"] in POLE_POSES

        pe = abs(pitch - r["pitch"])
        marks = []
        if pe > tilt_tol:
            marks.append("PITCH")
            failures.append(f"{r['pose']}: pitch off by {pe:.1f} deg "
                            f"(held {r['pitch']:+.0f}, got {pitch:+.1f})")

        he_s = "     n/a"
        if not pole and not math.isnan(r["hdg"]):
            he = circ_err(hdg, r["hdg"])
            he_s = f"{r['hdg']:6.0f}/{hdg:6.1f}"
            if he > TOL_HDG_DEG:
                marks.append("HDG")
                msg = (f"{r['pose']}: heading off by {he:.1f} deg "
                       f"(held {r['hdg']:.0f}, got {hdg:.1f})")
                (failures if cal_ok else notes).append(msg)

        re_s = "     n/a"
        if not pole and not math.isnan(r["roll"]):
            re = circ_err(roll, r["roll"])
            re_s = f"{r['roll']:6.0f}/{roll:6.1f}"
            if re > tilt_tol:
                marks.append("ROLL")
                failures.append(f"{r['pose']}: roll off by {re:.1f} deg "
                                f"(held {r['roll']:+.0f}, got {roll:.1f})")

        flag = ("  <-- " + ",".join(marks)) if marks else ""
        print(f"{r['pose']:<18}{r['pitch']:9.0f}/{pitch:7.1f}   "
              f"{he_s:>18}   {re_s:>18}{flag}")

        if not r["verified"]:
            notes.append(f"{r['pose']}: captured without the pose being verified")
        if not r["still"]:
            notes.append(f"{r['pose']}: unit was moving during capture")

    # ------------------------------------------------------------- bin agreement
    print()
    print("=" * 78)
    print("BIN ASSIGNMENT -- which cell each pose actually lands in")
    print("=" * 78)
    for r in rows:
        mc = calibrate(r["m"], bias, soft)
        pitch, hdg = coverage.reconstruct_orientation(*mc, *r["a"])
        roll = coverage.reconstruct_roll(*r["a"])
        got = (coverage._elev_band(pitch), _hdg_sector(hdg),
               coverage._roll_sector(roll))
        want = (coverage._elev_band(r["pitch"]),
                _hdg_sector(r["hdg"]) if not math.isnan(r["hdg"]) else None,
                coverage._roll_sector(r["roll"]) if not math.isnan(r["roll"]) else None)
        ok = (got[0] == want[0]
              and (want[1] is None or got[1] == want[1])
              and (want[2] is None or got[2] == want[2]))
        print(f"  {r['pose']:<18} want elev/hdg/roll {str(want):<16} got {str(got):<16}"
              f" {'ok' if ok else '<-- MISMATCH'}")

    # ------------------------------------------------------------------- summary
    print()
    print("=" * 78)
    if notes:
        print("Inconclusive / quality warnings (not counted as failures):")
        for n in notes:
            print(f"  ! {n}")
        print()
    if failures:
        print(f"FAIL -- {len(failures)} problem(s):")
        for f in failures:
            print(f"  - {f}")
        print("=" * 78)
        return 1
    if not cal_ok:
        print("PARTIAL PASS -- pitch and roll are correct on real hardware.")
        print("Heading and dip could not be judged: the magnetometer calibration")
        print("in this fixture is too poor to support them. Re-run a baseline")
        print("calibration, re-capture the fixture, and run this again.")
        print("=" * 78)
        return 0
    print("PASS -- every pose recovered within tolerance, on real hardware.")
    print("=" * 78)
    return 0


if __name__ == "__main__":
    sys.exit(main())
