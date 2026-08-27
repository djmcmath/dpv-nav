#!/usr/bin/env python3
"""Cross-check the firmware orientation port against the server's Python.

Gap-fill's entire premise is that the cell the *device* highlights is the same
cell the *website* reported as thin. That holds only while
`dpv-nav/src/util/mag_cal_orient.cpp` and
`divemap/calibration-processor/callib/coverage.py::reconstruct_orientation`
agree sample for sample. This script proves they do, without hardware:
it host-compiles the port (no Arduino dependencies, by design) and diffs both
implementations over a dense orientation sweep plus every degenerate branch.

Run it after touching either file, with an interpreter that has the
calibration-processor's requirements installed (coverage.py imports numpy):

    python3 dpv-nav/tools/orient_equivalence.py

Exit status is 0 only if every sample lands in the same bin and pitch/heading
agree to within float32 round-off. There is no real 9-axis CSV in this repo
(those live on the server's ./data volume), and for an *equivalence* check
synthetic data is the better instrument anyway -- it can be aimed directly at
the singular branches that real tumble data almost never visits.
"""
import math
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "dive-map", "calibration-processor"))

from callib import coverage  # noqa: E402

HARNESS_DIR = os.path.join(HERE, "orient_equiv")
SRC = [os.path.join(HARNESS_DIR, "harness.cpp"),
       os.path.join(REPO, "dpv-nav", "src", "util", "mag_cal_orient.cpp")]
BIN = os.path.join(HARNESS_DIR, "harness")

# float32 round-off: the firmware computes in float, the server in float64.
# Pitch/heading are only ever consumed through a 30-degree-wide bin, so these
# tolerances are three orders of magnitude tighter than anything that could
# change a bin assignment -- they exist to catch a formula drift, not to
# certify precision.
PITCH_TOL_DEG = 2e-3
HDG_TOL_DEG = 5e-3
# Roll sectors are 90 deg wide (coarser than the 30-deg heading sectors), so
# this tolerance has even more headroom -- same purpose, catch a formula
# drift, not certify precision.
ROLL_TOL_DEG = 5e-3


def build():
    subprocess.run(["g++", "-std=c++17", "-O2", "-o", BIN] + SRC, check=True)


def samples():
    """(label, rows) groups. Rows are (mx, my, mz, ax, ay, az)."""
    rng = random.Random(20260824)

    # 1. Dense physical sweep: a real magnetic field vector rotated through the
    #    full orientation sphere, with the accel reading implied by that same
    #    orientation. This is the case gap-fill actually runs in.
    sweep = []
    field = (18000.0, 0.0, 42000.0)  # nT-ish counts, dip ~67 deg (mid-latitude)
    for pitch_i in range(-90, 91, 5):
        for yaw_i in range(0, 360, 5):
            p = math.radians(pitch_i)
            y = math.radians(yaw_i)
            for roll_i in (0, 55, 130):
                r = math.radians(roll_i)
                # world->body: yaw, then pitch, then roll
                cy, sy = math.cos(y), math.sin(y)
                cp, sp = math.cos(p), math.sin(p)
                cr, sr = math.cos(r), math.sin(r)
                def rot(v):
                    x, yv, z = v
                    x, yv = cy * x + sy * yv, -sy * x + cy * yv
                    x, z = cp * x - sp * z, sp * x + cp * z
                    yv, z = cr * yv + sr * z, -sr * yv + cr * z
                    return (x, yv, z)
                mx, my, mz = rot(field)
                # gravity reads (0,0,-1g) in this frame when level
                ax, ay, az = rot((0.0, 0.0, -1.0))
                sweep.append((mx, my, mz, ax, ay, az))
    yield "physical sweep (full sphere x 3 rolls)", sweep

    # 2. Exactly level and exactly inverted -- the s2 < 1e-12 branch, where the
    #    minimal rotation is undefined and the two implementations have to make
    #    the *same* arbitrary choice (identity vs. the my-negating flip).
    degenerate = []
    for mx, my, mz in [(1000.0, 0.0, 500.0), (0.0, 1000.0, -500.0),
                       (-700.0, -700.0, 0.0), (0.0, 0.0, 1000.0)]:
        degenerate.append((mx, my, mz, 0.0, 0.0, -1.0))   # level: c > 0
        degenerate.append((mx, my, mz, 0.0, 0.0, 1.0))    # inverted: c < 0
    yield "degenerate: level / inverted (undefined minimal rotation)", degenerate

    # 3. Near-zero accel -- the u_norm < 1e-9 fallback. Straddles the threshold
    #    on both sides so a mismatched constant would show up.
    tiny = []
    for scale in (0.0, 1e-12, 1e-10, 9e-10, 1.1e-9, 1e-8, 1e-6):
        tiny.append((1234.0, -567.0, 890.0, scale, scale * 0.5, -scale))
    yield "degenerate: vanishing accel (tilt-compensation fallback)", tiny

    # 4. Poles: pitch = +/-90 exactly, and either side of it. This is the
    #    gimbal-lock case that the aircraft-Euler formulation gets wrong and
    #    that a targeted recollection exposed server-side.
    poles = []
    for pitch_deg in (-90.0, -89.999, -89.9, -60.0001, -59.9999,
                      59.9999, 60.0001, 89.9, 89.999, 90.0):
        p = math.radians(pitch_deg)
        for yaw_i in range(0, 360, 15):
            y = math.radians(yaw_i)
            cy, sy = math.cos(y), math.sin(y)
            cp, sp = math.cos(p), math.sin(p)
            def rot(v):
                x, yv, z = v
                x, yv = cy * x + sy * yv, -sy * x + cy * yv
                return (cp * x - sp * z, yv, sp * x + cp * z)
            poles.append(rot(field) + rot((0.0, 0.0, -1.0)))
    yield "poles and elevation-band boundaries", poles

    # 5. Adversarial: unconstrained random values, including physically
    #    impossible ones. Nothing should diverge, throw, or produce NaN.
    junk = [(rng.uniform(-40000, 40000), rng.uniform(-40000, 40000),
             rng.uniform(-40000, 40000), rng.uniform(-3, 3),
             rng.uniform(-3, 3), rng.uniform(-3, 3)) for _ in range(4000)]
    yield "adversarial random (incl. physically impossible)", junk


ROLL_INVARIANCE_TOL_DEG = 1e-2  # float32 firmware math; generous vs. the ~1e-6 deg float64 result


def roll_invariant_check():
    """Assert the *property* the equivalence loop above cannot see: heading
    must not move as roll varies at fixed true pitch/heading.

    2026-08-26: the firmware port and coverage.py agreed on every sample for
    over a month while both were silently non-roll-invariant (a "minimal
    rotation" formulation that only happened to be correct at roll = 0, the
    only orientation either implementation was ever validated against). The
    equivalence loop caught nothing because both sides had the identical bug
    -- it only proves the two *formulas* match, never that either one is
    right. This check runs directly against the firmware port (via the same
    host-compiled harness) and would have caught it: it groups samples by
    true (pitch, heading) and sweeps roll within each group, checking the
    *firmware's own* heading stays put -- no comparison to Python at all.
    """
    field = (18000.0, 0.0, 42000.0)
    groups = []  # each: list of (mx,my,mz,ax,ay,az) at fixed pitch/yaw, varying roll
    for pitch_i in range(-85, 86, 10):
        for yaw_i in range(0, 360, 30):
            p, y = math.radians(pitch_i), math.radians(yaw_i)
            rows = []
            for roll_i in range(0, 360, 20):
                r = math.radians(roll_i)
                cy, sy = math.cos(y), math.sin(y)
                cp, sp = math.cos(p), math.sin(p)
                cr, sr = math.cos(r), math.sin(r)
                def rot(v):
                    x, yv, z = v
                    x, yv = cy * x + sy * yv, -sy * x + cy * yv
                    x, z = cp * x - sp * z, sp * x + cp * z
                    yv, z = cr * yv + sr * z, -sr * yv + cr * z
                    return (x, yv, z)
                mx, my, mz = rot(field)
                # Deliver mag MIRRORED IN Y, as the board's magMap{+1,-2,+3}
                # does. This is a physical-property check, so it has to feed
                # the port the frame the hardware actually hands it -- an
                # un-mirrored vector here would make the check agree with a
                # frame-mixing bug instead of catching one (2026-08-26).
                my = -my
                ax, ay, az = rot((0.0, 0.0, -1.0))
                rows.append((mx, my, mz, ax, ay, az))
            groups.append(((pitch_i, yaw_i), rows))

    all_rows = [row for _, rows in groups for row in rows]
    stdin = "".join("%r,%r,%r,%r,%r,%r\n" % r for r in all_rows)
    out = subprocess.run([BIN], input=stdin, capture_output=True,
                         text=True, check=True).stdout.splitlines()
    assert len(out) == len(all_rows)

    worst = 0.0
    worst_group = None
    i = 0
    for label, rows in groups:
        headings = []
        for _ in rows:
            _, hdg, _, _, _ = out[i].split(",")
            headings.append(float(hdg))
            i += 1
        spread = max(
            min(abs(a - b), 360.0 - abs(a - b))
            for idx, a in enumerate(headings) for b in headings[idx + 1:]
        )
        if spread > worst:
            worst, worst_group = spread, label

    status = "FAIL" if worst > ROLL_INVARIANCE_TOL_DEG else "ok"
    print(f"[{status:4}] firmware heading roll-invariance "
          f"(worst spread {worst:.4e} deg at pitch/yaw={worst_group})")
    return worst <= ROLL_INVARIANCE_TOL_DEG, worst, worst_group


def convention_sentinel():
    """Quantify the axis-convention trap, which equivalence cannot catch.

    The diff above proves the two *formulas* agree. It says nothing about
    whether the caller hands them the right vectors -- and the single most
    likely way to break gap-fill on hardware is to pass `magNED` (mag Y
    re-negated for the Mahony filter) instead of the logical-frame mag the
    CSV and coverage.py both use. That mistake type-checks, runs, and
    produces plausible-looking headings.

    So: measure it. If the number below is small, the convention note in
    mag_cal_orient.h is overstated and should be revised. It is not small.
    """
    field = (18000.0, 0.0, 42000.0)
    worst_y = worst_z = 0.0
    for pitch_i in range(-80, 81, 10):
        for yaw_i in range(0, 360, 10):
            p, y = math.radians(pitch_i), math.radians(yaw_i)
            cy, sy, cp, sp = math.cos(y), math.sin(y), math.cos(p), math.sin(p)
            def rot(v):
                x, yv, z = v
                x, yv = cy * x + sy * yv, -sy * x + cy * yv
                return (cp * x - sp * z, yv, sp * x + cp * z)
            mx, my, mz = rot(field)
            ax, ay, az = rot((0.0, 0.0, -1.0))
            _, ref = coverage.reconstruct_orientation(mx, my, mz, ax, ay, az)
            # (a) mag Y negated -- i.e. someone passed magNED
            _, bad_y = coverage.reconstruct_orientation(mx, -my, mz, ax, ay, az)
            # (b) accel Z sign convention dropped
            _, bad_z = coverage.reconstruct_orientation(mx, my, mz, ax, ay, -az)
            for bad, slot in ((bad_y, "y"), (bad_z, "z")):
                d = abs(bad - ref)
                d = min(d, 360.0 - d)
                if slot == "y":
                    worst_y = max(worst_y, d)
                else:
                    worst_z = max(worst_z, d)

    print("\nConvention sentinel (NOT an equivalence check -- see docstring):")
    print(f"  passing magNED (mag Y negated) shifts heading by up to {worst_y:.1f} deg")
    print(f"  dropping the accel Z negation shifts heading by up to {worst_z:.1f} deg")


def main():
    build()
    total = 0
    failures = []
    for label, rows in samples():
        stdin = "".join("%r,%r,%r,%r,%r,%r\n" % r for r in rows)
        out = subprocess.run([BIN], input=stdin, capture_output=True,
                             text=True, check=True).stdout.splitlines()
        if len(out) != len(rows):
            failures.append(f"{label}: harness returned {len(out)} of {len(rows)} rows")
            continue

        worst_p = worst_h = worst_r = 0.0
        bin_mismatch = 0
        roll_sector_mismatch = 0
        for row, line in zip(rows, out):
            mx, my, mz, ax, ay, az = row
            py_pitch, py_hdg = coverage.reconstruct_orientation(mx, my, mz, ax, ay, az)
            py_bin = (coverage._elev_band(py_pitch) * coverage.HDG_SECTORS
                      + int(py_hdg // 30.0) % coverage.HDG_SECTORS)
            py_roll = coverage.reconstruct_roll(ax, ay, az)
            py_roll_sector = coverage._roll_sector(py_roll)
            c_pitch, c_hdg, c_bin, c_roll, c_roll_sector = line.split(",")
            c_pitch, c_hdg, c_bin = float(c_pitch), float(c_hdg), int(c_bin)
            c_roll, c_roll_sector = float(c_roll), int(c_roll_sector)

            dp = abs(c_pitch - py_pitch)
            dh = abs(c_hdg - py_hdg)
            dh = min(dh, 360.0 - dh)  # wrap
            dr = abs(c_roll - py_roll)
            dr = min(dr, 360.0 - dr)  # wrap
            worst_p = max(worst_p, dp)
            worst_h = max(worst_h, dh)
            worst_r = max(worst_r, dr)
            if c_bin != py_bin:
                # A disagreement exactly on a bin boundary is float32 round-off,
                # not a formula difference -- only count it if the angles differ
                # by more than the tolerance too.
                if dp > PITCH_TOL_DEG or dh > HDG_TOL_DEG:
                    bin_mismatch += 1
                    if bin_mismatch <= 3:
                        failures.append(
                            f"{label}: bin {c_bin} != {py_bin} for {row} "
                            f"(pitch {c_pitch:.6f} vs {py_pitch:.6f}, "
                            f"hdg {c_hdg:.6f} vs {py_hdg:.6f})")
            if c_roll_sector != py_roll_sector and dr > ROLL_TOL_DEG:
                roll_sector_mismatch += 1
                if roll_sector_mismatch <= 3:
                    failures.append(
                        f"{label}: roll_sector {c_roll_sector} != {py_roll_sector} for {row} "
                        f"(roll {c_roll:.6f} vs {py_roll:.6f})")

        total += len(rows)
        status = "FAIL" if (bin_mismatch or roll_sector_mismatch
                            or worst_p > PITCH_TOL_DEG or worst_h > HDG_TOL_DEG
                            or worst_r > ROLL_TOL_DEG) else "ok"
        print(f"[{status:4}] {len(rows):6d}  {label}")
        print(f"          max |dpitch| = {worst_p:.3e} deg, "
              f"max |dheading| = {worst_h:.3e} deg, bin mismatches = {bin_mismatch}, "
              f"max |droll| = {worst_r:.3e} deg, roll_sector mismatches = {roll_sector_mismatch}")
        if worst_p > PITCH_TOL_DEG:
            failures.append(f"{label}: pitch drift {worst_p:.3e} deg exceeds tolerance")
        if worst_h > HDG_TOL_DEG:
            failures.append(f"{label}: heading drift {worst_h:.3e} deg exceeds tolerance")
        if worst_r > ROLL_TOL_DEG:
            failures.append(f"{label}: roll drift {worst_r:.3e} deg exceeds tolerance")

    print(f"\n{total} samples compared.")

    roll_ok, roll_worst, roll_group = roll_invariant_check()
    if not roll_ok:
        failures.append(f"firmware heading not roll-invariant: {roll_worst:.4e} deg "
                         f"spread at pitch/yaw={roll_group}")

    if failures:
        print("\nFAILURES:")
        for f in failures:
            print("  -", f)
        return 1
    print("Firmware port and callib/coverage.py agree on every sample.")
    convention_sentinel()
    return 0


if __name__ == "__main__":
    sys.exit(main())
