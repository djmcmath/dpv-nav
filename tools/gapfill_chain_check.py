#!/usr/bin/env python3
"""End-to-end check that a cell the SERVER flags is the cell the DEVICE targets.

This is the bench test from the Phase 2 plan ("hold the unit at a known
orientation, confirm the highlighted cell matches the website") reduced to the
part that can be checked without hardware -- which is most of it. The chain is:

    coverage.py (elev_band, hdg_sector)     <- server decides a cell is empty
      -> row-major flatten to index r*12+c  <- backend targets_grid_from_coverage
      -> CalProgressPacket.targets[60]      <- 2-bits-per-cell wire packing
      -> JSON over Serial1 -> decode
      -> mag_orient::binIndex(pitch, hdg)   <- device bins a live sample
      -> targets[bin]                       <- the colour the diver sees

Every link is a place the ordering could be transposed, off-by-one, or flipped,
and the failure is silent: the grid still looks plausible, it just sends the
diver to the wrong orientations. So drive a real orientation through the whole
thing and check the status that comes out the far end is the status the server
put in for THAT cell.

The one link simulated rather than executed is the Rust flatten, because it is
a separate binary; `cargo test targets_grid_flattens_row_major` asserts exactly
the `r * 12 + c` used here, with a distinct value per cell so a transpose
cannot pass.

Run with an interpreter that has the calibration-processor's requirements:

    python3 dpv-nav/tools/gapfill_chain_check.py
"""
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "dive-map", "calibration-processor"))

from callib import coverage  # noqa: E402

HARNESS_DIR = os.path.join(HERE, "orient_equiv")
BIN = os.path.join(HARNESS_DIR, "chain")

STATUS_OK, STATUS_THIN, STATUS_EMPTY, STATUS_OVER = 0, 1, 2, 3


def build():
    json_inc = os.path.join(REPO, "dpv-nav", ".pio", "libdeps", "nav", "ArduinoJson", "src")
    if not os.path.isdir(json_inc):
        json_inc = os.path.join(REPO, "dpv-nav", ".pio", "libdeps", "display", "ArduinoJson", "src")
    subprocess.run(
        ["g++", "-std=c++17", "-O1", "-I", json_inc, "-o", BIN,
         os.path.join(HARNESS_DIR, "chain.cpp"),
         os.path.join(REPO, "dpv-nav", "lib", "dpvlink", "dpvlink.cpp"),
         os.path.join(REPO, "dpv-nav", "src", "util", "mag_cal_orient.cpp")],
        check=True)


def oriented_sample(pitch_deg, yaw_deg, roll_deg=0.0):
    """A physically consistent (mag, accel) pair at a given orientation."""
    field = (18000.0, 0.0, 42000.0)
    p, y, r = map(math.radians, (pitch_deg, yaw_deg, roll_deg))
    cy, sy, cp, sp, cr, sr = (math.cos(y), math.sin(y), math.cos(p),
                              math.sin(p), math.cos(r), math.sin(r))

    def rot(v):
        x, yv, z = v
        x, yv = cy * x + sy * yv, -sy * x + cy * yv
        x, z = cp * x - sp * z, sp * x + cp * z
        yv, z = cr * yv + sr * z, -sr * yv + cr * z
        return (x, yv, z)

    return rot(field) + rot((0.0, 0.0, -1.0))


def main():
    build()

    # A deliberately lopsided status map: no symmetry for a transpose or a
    # reversal to hide behind. Row 0 and row 4 differ, column 0 and column 11
    # differ, and the four status values are unevenly distributed.
    #
    # Every flagged cell below is one the sweep provably reaches, and an
    # unvisited flag is a hard failure -- a "passing" run that never entered a
    # flagged cell would prove nothing. The sweep does not reach all 60 cells:
    # at steep pitch this synthetic generator's yaw sweep barely moves the
    # tilt-compensated heading, so bands 1 and 3 only span a few sectors. That
    # is a property of the generator, not of the code under test, and it does
    # not weaken the check -- what is being proven here is index plumbing, and
    # all five elevation bands and a wide spread of sectors are exercised.
    status = [[STATUS_OK] * 12 for _ in range(5)]
    flagged = {}
    pattern = [(0, 1, STATUS_EMPTY), (0, 7, STATUS_THIN), (1, 0, STATUS_THIN),
               (1, 11, STATUS_EMPTY), (2, 3, STATUS_EMPTY), (2, 4, STATUS_OVER),
               (3, 6, STATUS_THIN), (4, 2, STATUS_EMPTY), (4, 9, STATUS_OVER),
               (3, 5, STATUS_EMPTY)]
    for r, c, st in pattern:
        status[r][c] = st
        flagged[(r, c)] = st

    # The backend's flatten (asserted independently by cargo test).
    flat = [status[r][c] for r in range(5) for c in range(12)]

    # Sweep real orientations across the whole sphere, including the poles and
    # sitting right on band boundaries.
    samples = []
    for pitch in (-85, -75, -61, -59, -45, -31, -29, 0, 29, 31, 45, 59, 61, 75, 85):
        for yaw in range(1, 360, 3):
            for roll in (0, 40):
                samples.append((oriented_sample(pitch, yaw, roll), pitch, yaw))

    stdin = " ".join(str(v) for v in flat) + "\n"
    stdin += "".join("%r,%r,%r,%r,%r,%r\n" % s[0] for s in samples)
    out = subprocess.run([BIN], input=stdin, capture_output=True, text=True,
                         check=True).stdout.splitlines()
    assert len(out) == len(samples), f"{len(out)} results for {len(samples)} samples"

    mismatches = []
    seen_flagged = set()
    for (row, pitch, yaw), line in zip(samples, out):
        mx, my, mz, ax, ay, az = row
        py_pitch, py_hdg = coverage.reconstruct_orientation(mx, my, mz, ax, ay, az)
        r = coverage._elev_band(py_pitch)
        c = int(py_hdg // 30.0) % coverage.HDG_SECTORS

        dev_bin, dev_status = (int(v) for v in line.split(","))
        want_bin = r * 12 + c
        want_status = status[r][c]

        if dev_bin != want_bin or dev_status != want_status:
            if len(mismatches) < 5:
                mismatches.append(
                    f"pitch={pitch} yaw={yaw}: server cell ({r},{c}) status "
                    f"{want_status} -> device bin {dev_bin} status {dev_status} "
                    f"(expected bin {want_bin})")
        if (r, c) in flagged:
            seen_flagged.add((r, c))

    bands = sorted({r for r, _ in seen_flagged})
    print(f"{len(samples)} orientations driven through the full chain.")
    print(f"{len(seen_flagged)}/{len(flagged)} flagged cells visited, "
          f"across elevation bands {bands}.")

    if mismatches:
        print("\nCHAIN MISMATCHES:")
        for m in mismatches:
            print("  -", m)
        return 1

    unvisited = sorted(set(flagged) - seen_flagged)
    if unvisited:
        # Not a pass. A run that never entered a flagged cell only proves that
        # unflagged cells stay unflagged, which no amount of index-scrambling
        # would break.
        print(f"\nFAIL: flagged cells never visited: {unvisited}")
        return 1

    print("Server cell status and device target status agree for every orientation.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
