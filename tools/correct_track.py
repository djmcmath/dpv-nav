#!/usr/bin/env python3
"""
Correct a DPV-nav dead-reckoning track to close on known positions.

Supports any number of "anchor" blocks in the log — one at the start, one at
the end, and optionally one or more in the middle (e.g. a waypoint snap on a
wreck, a surface interval mid-dive, etc.).

Anchors — positions that are known rather than inferred:
    G   a GPS fix.
    W   a waypoint snap.  The diver snapped position to a known waypoint and
        the unit jumped to it in real time, so the track demonstrably passed
        through that point.  It constrains the DR leading up to it exactly as
        a GPS fix does, and is treated identically here.

Anchor block structure:
    - An "anchor block" is a contiguous run of rows with pos_src in {G, W}.
    - The *last* row of an anchor block anchors the start of the following DR
      segment (most recent known position before going under).
    - The *first* row of an anchor block anchors the end of the preceding DR
      segment (first known position after surfacing).
    - For a single-row anchor block (a lone waypoint snap mid-dive), both
      anchors are the same row: it closes the DR before it and opens the DR
      after it.


Assembled dives, timebase, and discontinuities
──────────────────────────────────────────────
A single dive is often spread across several log files — the unit gets power
cycled on the boat, or logging is restarted partway out.  `assemble_track.py`
splices those files into one CSV and adds two columns:

    t_s   seconds on a single wall-clock-derived timeline spanning the whole
          dive.  Raw logs only carry `timestamp_ms` (millis since boot), which
          resets on every power cycle and therefore cannot span files.
    seg   run id.  It increments at every discontinuity — a splice between two
          files, or any interval the unit was not logging.

Both columns are optional.  A plain single-file log with neither still works
exactly as before.

DR is **never integrated across a discontinuity**.  Across a gap, time passed
and the diver kept moving, but we have no data for the interval — and the
integrator would happily multiply the speed the diver resumed at by a
gap-sized dt and invent a leg that never happened.  A discontinuity therefore
always ends the current block and the current DR segment.

That in turn means a DR segment may be anchored on only one side:

    both   A … E … A   Constrains (k, θ) and can be closed.  The normal case.
    start  A … E ⋮     Logging stopped while still under.  Integrate forward.
    end    ⋮ E … A     Logging *started* while already under — the common
                       "I restarted the log partway out" case.  The shape is
                       known and one position is known, so the whole segment is
                       rigidly slid into place by its closing anchor.
    none   ⋮ E ⋮       Nothing to tie it to.  Passed through uncorrected.

Only both-anchored segments constrain the solve; one-anchored segments are
still *placed* once (k, θ) are known from the rest of the dive.

For raw files with no `seg` column, any implausibly large or negative time step
is treated as an implicit discontinuity (see --max-dt) rather than being
integrated through.


Correction modes (--mode):

  auto (default)
    Works out which of the modes below the data can actually support, and says
    why.  This is not a convenience — it is the difference between a measurement
    and a fabrication.

    Each both-anchored segment supplies exactly 2 equations.  The unknowns are
    the speed factor k, the heading offset θ, and — if the water was moving — the
    two components of the current:

        2 informative segments (4 eqns) → k, θ, Cx, Cy       [reciprocal]
        1 informative segment  (2 eqns) → k, θ, no current   [joint]
        0 informative segments          → nothing is identifiable

    A segment is only informative if its *net* displacement is a real fraction of
    the distance travelled.  On an out-and-back the diver returns to where they
    started, so net displacement is near zero however far they swam: the system
    goes singular and the fit will drive k towards zero to reconcile a long path
    with a short displacement.  That is a genuine least-squares minimiser and
    physical nonsense.  Auto refuses to report it as a sensor correction, falls
    back to proportional, and says the track is cosmetic.

    The practical consequence for divers: an anchor in the MIDDLE of the dive is
    worth far more than fixes at both ends of a round trip.  Snapping a waypoint
    at the wreck turns one useless loop into two informative legs — and makes the
    current solvable.

  reciprocal
    Calibration mode.  Requires an A-E-A-E-A log structure — outbound leg,
    midpoint fix (GPS or waypoint snap), return leg on a reciprocal course.
    Solves simultaneously for speed factor (k), heading offset (θ), and water
    current vector (Cx, Cy) using a 4×4 linear system derived from the two
    anchor-constrained leg displacements.  Exact solution (no LS); current and
    sensor errors are isolated without approximation.

    Corrected EPs include current drift: position = k·speed in dir (heading+θ)
    plus current velocity, each dt.  Use this mode to characterize sensor errors
    at different DPV speeds and measure ambient current.

  joint
    Solves jointly for a constant speed scale (k) and constant heading
    offset (θ) using closed-form weighted least squares across all
    both-anchored DR segments.  A single (k, θ) pair is applied to every
    segment — appropriate when the systematic errors (compass bias, flow-meter
    k-factor) are constant across the whole dive.

    Corrected EPs are re-integrated as: adj[i+1] = adj[i] + k·speed·dt
    in direction (heading + θ).  This is the physically meaningful
    reconstruction — what the device would have shown with correct
    sensors.  Anchors are used only to solve for (k, θ); the
    corrected track is not warped to force closure (use proportional
    mode for that).  Per-segment closure gaps after correction are
    reported and reflect the quality of the joint fit.

    Normal equations decouple (A^T A = D · I₂, D = Σ|DR_i|²), so no
    numpy is required.  The single-segment case is exact (zero gap).

    Optional --max-theta / --max-k-error flags constrain the solution.

  proportional
    Applies distance-weighted closure correction independently to each
    both-anchored segment.  Makes no assumption about error cause.  Works for
    loop segments where joint degenerates.  Always closes each segment exactly.
    One-anchored segments cannot be closed and are placed by plain DR.

Note that anchor rows (G and W) pass through unchanged in all modes.
Adds columns: adj_pos_x_m, adj_pos_y_m, adj_lat, adj_lon
In order to re-ingest into dive map, copy the adj_ columns back to the lat/lon columns.

Usage:
    python tools/correct_track.py <logfile.csv> [--mode auto|reciprocal|joint|proportional]
                                                 [--max-theta DEG]
                                                 [--max-k-error FRAC]
                                                 [--max-dt SECONDS]

Output: <logfile>_corrected.csv
"""

import argparse
import csv
import math
import os
import sys

# Must match DEFAULT_BASELINE_LAT / DEFAULT_BASELINE_LON in src/config.h
BASELINE_LAT = 47.5889
BASELINE_LON = -122.284

M_PER_DEG_LAT = 111320.0

# A time step longer than this, in a file that does not declare its own `seg`
# column, is assumed to be a gap in logging rather than a real sample interval.
DEFAULT_MAX_DT_S = 30.0


def m_per_deg_lon(lat_deg: float) -> float:
    return M_PER_DEG_LAT * math.cos(math.radians(lat_deg))


def latlon_to_xy(lat: float, lon: float):
    x = (lon - BASELINE_LON) * m_per_deg_lon(BASELINE_LAT)
    y = (lat - BASELINE_LAT) * M_PER_DEG_LAT
    return x, y


def xy_to_latlon(x: float, y: float):
    lat = BASELINE_LAT + y / M_PER_DEG_LAT
    lon = BASELINE_LON + x / m_per_deg_lon(BASELINE_LAT)
    return lat, lon


# ── Timebase ──────────────────────────────────────────────────────────────────

def row_time(row: dict) -> float:
    """
    Row time in seconds.

    Prefers `t_s` (the unified dive timeline written by assemble_track.py) and
    falls back to `timestamp_ms` for raw single-file logs.  The two must never
    be mixed: timestamp_ms restarts at zero on every power cycle, so it is only
    meaningful within one file.
    """
    t = row.get("t_s")
    if t not in (None, ""):
        return float(t)
    return int(row["timestamp_ms"]) / 1000.0


# ── Runs (discontinuity handling) ─────────────────────────────────────────────

def assign_runs(rows: list[dict], max_dt: float) -> list[tuple[int, float]]:
    """
    Stamp every row with an internal run id (row['_run']).  Rows in the same run
    are contiguous in time; a change of run id marks an interval we have no data
    for and must not integrate across.

    Uses the `seg` column when the file declares one.  Otherwise infers the
    boundaries from the timebase, so that a raw log which skipped or restarted
    still can't fabricate a leg.

    Returns the list of *inferred* boundaries as (row_index, dt_seconds), so the
    caller can report them.  Empty when the file declared its own segments.
    """
    if not rows:
        return []

    if "seg" in rows[0] and rows[0]["seg"] not in (None, ""):
        for row in rows:
            row["_run"] = int(row["seg"])
        return []

    inferred: list[tuple[int, float]] = []
    run = 0
    rows[0]["_run"] = 0
    prev_t = row_time(rows[0])
    for i, row in enumerate(rows[1:], 1):
        t = row_time(row)
        dt = t - prev_t
        if dt < 0.0 or dt > max_dt:
            run += 1
            inferred.append((i, dt))
        row["_run"] = run
        prev_t = t
    return inferred


# ── Block parsing ─────────────────────────────────────────────────────────────

# Position sources that are *known* rather than inferred, and so can anchor a DR
# segment:
#
#   G  a GPS fix.
#   W  a waypoint snap — the diver deliberately snapped position to a known
#      waypoint.  The unit jumps to that position in real time, which means the
#      track demonstrably passed through it, so it constrains the DR that led up
#      to it exactly as a GPS fix does.
#
# Both collapse to a single block type 'A' (anchor), so a G-block running
# straight into a W row is one anchor block, and a lone W in the middle of a
# dive is a single-row anchor block that closes the DR before it and opens the
# DR after it.
ANCHOR_SRCS = {"G", "W"}


def block_type(row: dict) -> str:
    """'A' for a known position (GPS or waypoint snap), 'E' for dead reckoning."""
    return "A" if row["pos_src"] in ANCHOR_SRCS else row["pos_src"]


def parse_blocks(rows: list[dict]) -> list[dict]:
    """
    Group consecutive rows into blocks of anchors ('A') and dead reckoning ('E').
    A block never spans a run boundary — a discontinuity always ends the current
    block.
    Returns list of {'type': str, 'rows': list[dict], 'start_idx': int, 'run': int}.
    """
    if not rows:
        return []
    blocks = []
    cur_type  = block_type(rows[0])
    cur_run   = rows[0]["_run"]
    cur_rows  = [rows[0]]
    cur_start = 0
    for i, row in enumerate(rows[1:], 1):
        typ = block_type(row)
        run = row["_run"]
        if typ == cur_type and run == cur_run:
            cur_rows.append(row)
        else:
            blocks.append({"type": cur_type, "rows": list(cur_rows),
                           "start_idx": cur_start, "run": cur_run})
            cur_type  = typ
            cur_run   = run
            cur_rows  = [row]
            cur_start = i
    blocks.append({"type": cur_type, "rows": cur_rows,
                   "start_idx": cur_start, "run": cur_run})
    return blocks


def validate_blocks(blocks: list[dict]) -> None:
    """
    Check there is something correctable here.

    Deliberately permissive about where the anchors fall: an assembled dive may
    legitimately open or close on DR rows, and each such segment is anchored
    from one side only.  build_segments() works that out per segment; requiring
    the whole file to be G-E-G-…-G would reject exactly the logs we most want
    to fix.
    """
    if not blocks:
        print("Error: no data rows found.")
        sys.exit(1)

    unsupported = {b["type"] for b in blocks} - {"A", "E"}
    if unsupported:
        vals = ", ".join(repr(v) for v in sorted(unsupported))
        print(f"Error: unsupported pos_src value(s): {vals}. Expected 'G' (GPS), "
              f"'W' (waypoint snap) or 'E' (estimated).")
        sys.exit(1)

    if not any(b["type"] == "E" for b in blocks):
        print("Error: no estimated rows (pos_src = 'E') found — nothing to correct.")
        sys.exit(1)

    if not any(b["type"] == "A" for b in blocks):
        print("Error: no known positions (pos_src = 'G' or 'W') found — the track has no")
        print("  anchor, so its shape is known but its position is not.  Nothing to")
        print("  correct against.")
        sys.exit(1)


def build_segments(blocks: list[dict]) -> list[dict]:
    """
    One segment per E-block.

    start_row: last row of the preceding anchor block *in the same run* — the
               most recent known position before the DR began.  None when the run
               opens on DR, i.e. logging started after the unit had already left
               its last fix.
    end_row:   first row of the following anchor block *in the same run* — the
               first known position after the DR.  None when the run ends while
               still on DR.

    The "same run" condition is the whole point: an anchor on the far side of a
    discontinuity is not an anchor for this segment, because the diver moved an
    unknown distance in between.
    """
    segments = []
    for i, b in enumerate(blocks):
        if b["type"] != "E":
            continue

        prev_b = blocks[i - 1] if i > 0 else None
        next_b = blocks[i + 1] if i + 1 < len(blocks) else None

        start_row = None
        if prev_b and prev_b["type"] == "A" and prev_b["run"] == b["run"]:
            start_row = prev_b["rows"][-1]

        end_row = None
        if next_b and next_b["type"] == "A" and next_b["run"] == b["run"]:
            end_row = next_b["rows"][0]

        if start_row and end_row:
            anchored = "both"
        elif start_row:
            anchored = "start"
        elif end_row:
            anchored = "end"
        else:
            anchored = "none"

        segments.append({
            "num":       len(segments) + 1,
            "start_row": start_row,
            "end_row":   end_row,
            "est_rows":  b["rows"],
            "start_idx": b["start_idx"],
            "run":       b["run"],
            "anchored":  anchored,
        })
    return segments


# ── Per-segment math ──────────────────────────────────────────────────────────

def _dr_steps(seg: dict):
    """
    Yield (dt, heading_rad, speed_ms) for each estimated row in a segment.

    The first step is measured from the start anchor when there is one.  When
    the segment opens on a discontinuity there is no measurable interval before
    its first row, so that row's dt is zero and it contributes no displacement —
    its position *is* the segment's relative origin.
    """
    est_rows = seg["est_rows"]
    anchor   = seg["start_row"] if seg["start_row"] is not None else est_rows[0]
    prev_t   = row_time(anchor)
    for row in est_rows:
        t  = row_time(row)
        dt = t - prev_t
        prev_t = t
        yield dt, math.radians(float(row["heading_deg"])), float(row["speed_ms"])


def integrate(seg: dict, k: float = 1.0, theta_rad: float = 0.0,
              Cx: float = 0.0, Cy: float = 0.0,
              origin: tuple[float, float] = (0.0, 0.0)) -> list[tuple[float, float]]:
    """
    Re-integrate one segment from `origin`:
        adj[i+1] = adj[i] + (k·spd·(sin(h+θ), cos(h+θ)) + (Cx, Cy))·dt

    Returns the position after every estimated row.  With k=1, θ=0 and no
    current this reproduces the raw DR shape.
    """
    x, y = origin
    positions: list[tuple[float, float]] = []
    for dt, hr, spd in _dr_steps(seg):
        x += (k * spd * math.sin(hr + theta_rad) + Cx) * dt
        y += (k * spd * math.cos(hr + theta_rad) + Cy) * dt
        positions.append((x, y))
    return positions


def compute_dr_vectors(seg: dict) -> tuple[float, float, float, float]:
    """
    Integrate the raw DR for one both-anchored segment.
    Returns (Sx, Sy, dx, dy) where:
        (Sx, Sy) = raw DR displacement sum
        (dx, dy) = GPS required displacement (end − start)
    """
    x0, y0 = latlon_to_xy(float(seg["start_row"]["lat"]), float(seg["start_row"]["lon"]))
    xN, yN = latlon_to_xy(float(seg["end_row"]["lat"]),   float(seg["end_row"]["lon"]))
    dx, dy = xN - x0, yN - y0

    Sx, Sy = integrate(seg)[-1]
    return Sx, Sy, dx, dy


def place_segment(seg: dict, k: float, theta_rad: float,
                  Cx: float = 0.0, Cy: float = 0.0):
    """
    Place a segment's corrected positions in the absolute frame.

    both / start-anchored
        Integrate forward from the opening fix.
    end-anchored
        Integrate the shape from an arbitrary origin, then translate it so the
        last estimated row lands exactly on the closing fix.  The dive's shape
        is known and one true position is known, so the track slides rigidly
        into place — no guess about where the diver started is required.
    unanchored
        Nothing to tie it to.  Pass the raw logged positions through unchanged;
        the caller warns.

    Returns (positions, gap_m, (gap_x, gap_y)).  The gap is the closure residual
    against the end anchor, and is only meaningful for a both-anchored segment —
    an end-anchored one closes exactly by construction.
    """
    if seg["start_row"] is not None:
        origin = latlon_to_xy(float(seg["start_row"]["lat"]), float(seg["start_row"]["lon"]))
        positions = integrate(seg, k, theta_rad, Cx, Cy, origin)

    elif seg["end_row"] is not None:
        rel = integrate(seg, k, theta_rad, Cx, Cy, (0.0, 0.0))
        ex, ey = latlon_to_xy(float(seg["end_row"]["lat"]), float(seg["end_row"]["lon"]))
        ox, oy = ex - rel[-1][0], ey - rel[-1][1]
        positions = [(x + ox, y + oy) for x, y in rel]

    else:
        positions = [latlon_to_xy(float(r["lat"]), float(r["lon"]))
                     for r in seg["est_rows"]]

    if seg["end_row"] is not None:
        ex, ey = latlon_to_xy(float(seg["end_row"]["lat"]), float(seg["end_row"]["lon"]))
        gap_x, gap_y = ex - positions[-1][0], ey - positions[-1][1]
    else:
        gap_x = gap_y = 0.0

    return positions, math.hypot(gap_x, gap_y), (gap_x, gap_y)


def _leg_stats(seg: dict) -> tuple[float, float]:
    """Return (Σspd·dt, elapsed_s) for a segment — average speed = ratio."""
    spd_int = 0.0
    elapsed = 0.0
    for dt, _hr, spd in _dr_steps(seg):
        spd_int += spd * dt
        elapsed += dt
    return spd_int, elapsed


def solve_reciprocal(seg1: dict, seg2: dict,
                     vec1: tuple, vec2: tuple) -> tuple:
    """
    Exact solution for (k, θ, Cx, Cy) from a reciprocal run pair.

    For each leg the corrected ground displacement equals the GPS displacement:
        c·Sx₁ + s·Sy₁ + Cx·t₁ = dx     (1x)
        c·Sy₁ − s·Sx₁ + Cy·t₁ = dy     (1y)
        c·Sx₂ + s·Sy₂ + Cx·t₂ = ex     (2x)
        c·Sy₂ − s·Sx₂ + Cy·t₂ = ey     (2y)
    where c = k·cosθ, s = k·sinθ.

    Substituting Cx, Cy from equations (1) into (2) eliminates the current
    and yields a 2×2 system in (c, s) with closed-form solution:
        α = t₂/t₁
        E = Sx₂ − Sx₁·α,   F = Sy₂ − Sy₁·α
        G = ex  − dx·α,     H = ey  − dy·α
        c = (G·E + H·F) / (E²+F²)
        s = (G·F − H·E) / (E²+F²)

    E²+F² = 0 only if both legs have identical DR direction per unit time —
    impossible for truly reciprocal courses.

    Returns (k, theta_rad, Cx, Cy, t1, t2, avg_spd1, avg_spd2).
    """
    Sx1, Sy1, dx, dy = vec1
    Sx2, Sy2, ex, ey = vec2

    spd_int1, t1 = _leg_stats(seg1)
    spd_int2, t2 = _leg_stats(seg2)

    alpha = t2 / t1
    E = Sx2 - Sx1 * alpha
    F = Sy2 - Sy1 * alpha
    G = ex  - dx  * alpha
    H = ey  - dy  * alpha

    denom = E * E + F * F
    if denom < 1e-6:
        print("Error: degenerate reciprocal geometry — legs too similar in direction.")
        sys.exit(1)

    c = (G * E + H * F) / denom
    s = (G * F - H * E) / denom

    Cx = (dx - c * Sx1 - s * Sy1) / t1
    Cy = (dy - c * Sy1 + s * Sx1) / t1

    avg_spd1 = spd_int1 / t1 if t1 > 0 else 0.0
    avg_spd2 = spd_int2 / t2 if t2 > 0 else 0.0

    return math.hypot(c, s), math.atan2(s, c), Cx, Cy, t1, t2, avg_spd1, avg_spd2


# ── Joint solver ──────────────────────────────────────────────────────────────

def solve_k_theta_multi(vec_list: list[tuple]) -> tuple[float, float]:
    """
    Closed-form weighted LS solution for (k, θ) across N segments.

    For each segment the corrected displacement must satisfy:
        [Sx_i   Sy_i] [c]   [dx_i]
        [Sy_i  -Sx_i] [s] = [dy_i]
    where c = k·cos θ, s = k·sin θ.

    The stacked normal equations decouple (A^T A = (Σ|DR_i|²)·I₂):
        c = Σ(Sx_i·dx_i + Sy_i·dy_i) / Σ(Sx_i² + Sy_i²)
        s = Σ(Sy_i·dx_i − Sx_i·dy_i) / Σ(Sx_i² + Sy_i²)

    Single-segment case is exact (zero residual).  Multi-segment is LS.
    Segments with larger DR magnitude contribute proportionally more weight.

    Sign convention for θ:
        positive → raw DR points clockwise of target; headings were reading
                   low (add θ to all headings corrects)
        negative → raw DR points counterclockwise; headings were reading high

    Returns (k, theta_rad), or None when the constrained segments carry too
    little DR displacement to say anything about the sensors — which happens
    when the only fully-anchored segments are short stationary ones and all the
    real swimming sits in a segment anchored on one side.  That is a normal
    shape for a dive whose logging was interrupted, so it is the caller's job to
    fall back to the identity correction, not grounds for failing the run.
    """
    num_c = num_s = denom = 0.0
    for Sx, Sy, dx, dy in vec_list:
        num_c += Sx * dx + Sy * dy
        num_s += Sy * dx - Sx * dy
        denom += Sx * Sx + Sy * Sy

    if denom < 1e-6:
        return None

    c = num_c / denom
    s = num_s / denom
    return math.hypot(c, s), math.atan2(s, c)


# ── Proportional solver ───────────────────────────────────────────────────────

def proportional_correct_segment(seg: dict):
    """
    Distance-weighted proportional closure for one both-anchored segment.
    Returns (positions, (closure_x, closure_y), total_path_m).
    """
    x, y   = latlon_to_xy(float(seg["start_row"]["lat"]), float(seg["start_row"]["lon"]))
    xN, yN = latlon_to_xy(float(seg["end_row"]["lat"]),   float(seg["end_row"]["lon"]))

    raw_positions: list[tuple[float, float]] = []
    cum_dist: list[float] = []
    total = 0.0

    for dt, hr, spd in _dr_steps(seg):
        x += spd * math.sin(hr) * dt
        y += spd * math.cos(hr) * dt
        total += spd * dt
        raw_positions.append((x, y))
        cum_dist.append(total)

    dr_end = raw_positions[-1]
    closure_x = xN - dr_end[0]
    closure_y = yN - dr_end[1]

    positions = []
    for (rx, ry), cd in zip(raw_positions, cum_dist):
        t = cd / total if total > 0 else 0.0
        positions.append((rx + t * closure_x, ry + t * closure_y))

    return positions, (closure_x, closure_y), total


# ── Mode selection ────────────────────────────────────────────────────────────
#
# Which correction is valid is not a preference — it is decided by the anchor
# geometry, because that geometry is what determines *what can be identified at
# all*.
#
# Every both-anchored segment supplies exactly 2 equations (the east and north
# components of its required displacement).  The unknowns are the speed factor k,
# the heading offset θ, and — if the water was moving — the two components of the
# current.  So:
#
#   2 segments (4 equations)  →  k, θ, Cx, Cy      all four, exactly    [reciprocal]
#   1 segment  (2 equations)  →  k, θ              assuming no current  [joint]
#   0 segments (0 equations)  →  nothing
#
# A segment only counts if its *net* displacement is a meaningful fraction of the
# distance travelled.  On an out-and-back the diver returns to where they began,
# so net displacement is near zero no matter how far they swam — the equations go
# singular and the fit will cheerfully drive k towards zero to reconcile a long
# path with a short displacement.  That is a real least-squares minimiser and
# complete physical nonsense, so we refuse to report it as a sensor correction.

MIN_CLOSURE_RATIO = 0.25   # net displacement / DR displacement, below which a segment tells us nothing
MIN_DR_M          = 20.0   # a segment that barely moved cannot constrain anything either


def segment_metrics(seg: dict) -> tuple[float, float, float]:
    """(DR displacement, required displacement, ratio) for a both-anchored segment."""
    Sx, Sy, dx, dy = compute_dr_vectors(seg)
    S = math.hypot(Sx, Sy)
    D = math.hypot(dx, dy)
    return S, D, (D / S if S > 1e-6 else 0.0)


def is_informative(seg: dict) -> bool:
    S, _D, ratio = segment_metrics(seg)
    return S >= MIN_DR_M and ratio >= MIN_CLOSURE_RATIO


def reciprocal_conditioned(seg1: dict, seg2: dict) -> bool:
    """
    True when two segments are geometrically distinct enough to separate sensor
    error from current.  The reciprocal solve inverts (E² + F²); that vanishes
    when both legs have the same DR direction per unit time, i.e. the diver never
    really turned around.
    """
    Sx1, Sy1, _, _ = compute_dr_vectors(seg1)
    Sx2, Sy2, _, _ = compute_dr_vectors(seg2)
    _, t1 = _leg_stats(seg1)
    _, t2 = _leg_stats(seg2)
    if t1 <= 0:
        return False
    alpha = t2 / t1
    E = Sx2 - Sx1 * alpha
    F = Sy2 - Sy1 * alpha
    return (E * E + F * F) > 1.0


def choose_mode(segments: list[dict], solvable: list[dict], n_runs: int) -> tuple[str, list[str]]:
    """
    Pick the correction the data can actually support, and explain the choice.
    Returns (mode, diagnosis lines).
    """
    lines = []
    informative = [s for s in solvable if is_informative(s)]

    lines.append(f"{len(segments)} DR segment(s); {len(solvable)} anchored at both ends, "
                 f"{len(informative)} of those informative.")
    for seg in solvable:
        S, D, ratio = segment_metrics(seg)
        verdict = "informative" if is_informative(seg) else "returns to its start — tells us nothing"
        lines.append(f"  segment {seg['num']}: DR {S:.0f} m, net {D:.0f} m "
                     f"({100 * ratio:.0f}% of DR) — {verdict}")

    if (len(segments) == 2 and len(solvable) == 2 and len(informative) == 2
            and n_runs == 1 and reciprocal_conditioned(*solvable)):
        lines.append("")
        lines.append("→ reciprocal: two informative legs give 4 equations for 4 unknowns.")
        lines.append("  Solving for speed factor, heading offset AND current — all of them,")
        lines.append("  exactly, with no assumption left over.")
        return "reciprocal", lines

    if informative:
        lines.append("")
        lines.append("→ joint: one informative leg gives 2 equations for 2 unknowns.")
        lines.append("  Solving for speed factor and heading offset, ASSUMING no current.")
        lines.append("  A current would be silently absorbed into those two numbers; with only")
        lines.append("  one leg there is no way to tell the difference.")
        if len(informative) == 1:
            lines.append("  To separate them, snap a waypoint at the far end of the dive — that")
            lines.append("  turns one leg into two and makes the current solvable.")
        return "joint", lines

    if solvable:
        lines.append("")
        lines.append("→ proportional: every leg here returns to where it started, so nothing")
        lines.append("  about the sensors or the water can be recovered from this dive.")
        lines.append("  The track will be distributed to close on the fixes, which produces a")
        lines.append("  plausible-looking path — but it is cosmetic, not a measurement.")
        lines.append("  Next time, snap a waypoint at the far end of the dive: an anchor in the")
        lines.append("  MIDDLE is worth far more than fixes at both ends of a round trip.")
        return "proportional", lines

    lines.append("")
    lines.append("→ no correction: nothing is anchored at both ends.  Segments will be placed")
    lines.append("  by whichever single fix they have, with their shape left as logged.")
    return "joint", lines


# ── Reporting helpers ─────────────────────────────────────────────────────────

ANCHOR_NOTE = {
    "both":  "",
    "start": "  [start-anchored: logging ended while still on DR — not used in solve]",
    "end":   "  [end-anchored: logging began on DR — slid into place by its closing fix]",
    "none":  "  [UNANCHORED: no fix on either side — passed through uncorrected]",
}


def describe_anchor(seg: dict) -> str:
    return ANCHOR_NOTE[seg["anchored"]]


def print_anchor_lines(seg: dict) -> None:
    if seg["start_row"] is not None:
        print(f"    GPS start:       ({float(seg['start_row']['lat']):.6f}, "
              f"{float(seg['start_row']['lon']):.6f})")
    else:
        print(f"    GPS start:       — none (segment opens on a discontinuity)")
    if seg["end_row"] is not None:
        print(f"    GPS end:         ({float(seg['end_row']['lat']):.6f}, "
              f"{float(seg['end_row']['lon']):.6f})")
    else:
        print(f"    GPS end:         — none (segment ends on a discontinuity)")


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input_file")
    parser.add_argument("--mode", choices=["auto", "reciprocal", "joint", "proportional"],
                        default="auto",
                        help="Correction mode (default: auto — picks whichever correction the "
                             "anchor geometry can actually support, and says why)")
    parser.add_argument("--max-theta", type=float, default=None, metavar="DEG",
                        help="[joint] Constrain heading correction to ±DEG degrees")
    parser.add_argument("--max-k-error", type=float, default=None, metavar="FRAC",
                        help="[joint] Constrain speed factor to 1±FRAC (e.g. 0.15)")
    parser.add_argument("--max-dt", type=float, default=DEFAULT_MAX_DT_S, metavar="SECONDS",
                        help=f"Time step above which a gap in logging is assumed, for files "
                             f"with no 'seg' column (default: {DEFAULT_MAX_DT_S:g})")
    args = parser.parse_args()

    base, ext = os.path.splitext(args.input_file)
    output_file = f"{base}_corrected{ext}"

    with open(args.input_file, newline="") as f:
        reader = csv.DictReader(f)
        fieldnames = list(reader.fieldnames)
        rows = list(reader)

    if not rows:
        print("No data rows found.")
        sys.exit(1)

    inferred = assign_runs(rows, args.max_dt)
    blocks   = parse_blocks(rows)
    validate_blocks(blocks)
    segments = build_segments(blocks)

    n_runs    = len({b["run"] for b in blocks})
    n_segs    = len(segments)
    solvable  = [s for s in segments if s["anchored"] == "both"]
    unanchored = [s for s in segments if s["anchored"] == "none"]

    timebase = "t_s (assembled dive)" if "t_s" in fieldnames else "timestamp_ms (single file)"

    # ── Discontinuity report ──────────────────────────────────────────────────
    if n_runs > 1:
        print(f"Timebase:      {timebase}")
        if inferred:
            print(f"Discontinuities: {len(inferred)} inferred from the timebase "
                  f"(no 'seg' column; --max-dt = {args.max_dt:g}s)")
            for idx, dt in inferred:
                where = "time went backwards" if dt < 0 else f"{dt:.1f}s gap"
                print(f"    row {idx + 2}: {where} — DR will not be integrated across it")
        else:
            print(f"Discontinuities: {n_runs - 1} declared by the 'seg' column")
        print()

    if unanchored:
        print(f"WARNING: {len(unanchored)} DR segment(s) have no GPS fix on either side.")
        print(f"  Their shape is known but their position is not; they are passed through")
        print(f"  at their raw logged coordinates and are NOT corrected.")
        print()

    # ── Mode selection ────────────────────────────────────────────────────────
    if args.mode == "auto":
        mode, diagnosis = choose_mode(segments, solvable, n_runs)
        print("── What this dive can tell us ──────────────────────────────────")
        for line in diagnosis:
            print(f"  {line}" if line else "")
        print()
    else:
        mode = args.mode

    new_fields     = ["adj_pos_x_m", "adj_pos_y_m", "adj_lat", "adj_lon"]
    out_fieldnames = fieldnames + [f for f in new_fields if f not in fieldnames]

    # adj_pos: global row index → (adj_x, adj_y)
    adj_pos: dict[int, tuple[float, float]] = {}

    # Known positions (GPS fixes and waypoint snaps) always pass through at their
    # own lat/lon — they are measured, not inferred, so there is nothing to correct.
    for block in blocks:
        if block["type"] == "A":
            for i, row in enumerate(block["rows"]):
                try:
                    gx, gy = latlon_to_xy(float(row["lat"]), float(row["lon"]))
                except ValueError as e:
                    global_idx = block["start_idx"] + i
                    print(f"Error: bad lat/lon in anchor row {global_idx + 2} "
                          f"(CSV line {global_idx + 2}): lat={row['lat']!r} lon={row['lon']!r}")
                    print(f"  {e}")
                    sys.exit(1)
                adj_pos[block["start_idx"] + i] = (gx, gy)

    # ── Reciprocal calibration mode ───────────────────────────────────────────
    if mode == "reciprocal":
        if len(solvable) != 2 or n_segs != 2:
            print(f"Error: reciprocal mode requires exactly 2 fully-anchored DR segments "
                  f"(A-E-A-E-A), found {n_segs} segment(s), {len(solvable)} fully anchored.")
            sys.exit(1)
        if n_runs > 1:
            print("Error: reciprocal mode requires one continuous log — this file contains "
                  "a gap in logging.")
            sys.exit(1)

        seg1, seg2 = solvable[0], solvable[1]
        vec1 = compute_dr_vectors(seg1)
        vec2 = compute_dr_vectors(seg2)

        k, theta_rad, Cx, Cy, t1, t2, avg_spd1, avg_spd2 = \
            solve_reciprocal(seg1, seg2, vec1, vec2)

        for seg in segments:
            positions, _, _ = place_segment(seg, k, theta_rad, Cx, Cy)
            for i, pos in enumerate(positions):
                adj_pos[seg["start_idx"] + i] = pos

        # ── Calibration report ────────────────────────────────────────────────
        theta_deg    = (math.degrees(theta_rad) + 180) % 360 - 180
        current_spd  = math.hypot(Cx, Cy)
        current_dir  = math.degrees(math.atan2(Cx, Cy)) % 360

        g_start = seg1["start_row"]   # last G of first block
        g_mid   = seg1["end_row"]     # first G of middle block (turnaround)
        g_end   = seg2["end_row"]     # first G of last block
        x0, y0  = latlon_to_xy(float(g_start["lat"]), float(g_start["lon"]))
        xm, ym  = latlon_to_xy(float(g_mid["lat"]),   float(g_mid["lon"]))
        xe, ye  = latlon_to_xy(float(g_end["lat"]),   float(g_end["lon"]))

        mid_dist = math.hypot(xm - x0, ym - y0)
        mid_az   = math.degrees(math.atan2(xm - x0, ym - y0)) % 360
        end_dist = math.hypot(xe - x0, ye - y0)

        Sx1, Sy1, dx, dy = vec1
        Sx2, Sy2, ex, ey = vec2

        gap1 = place_segment(seg1, k, theta_rad, Cx, Cy)[1]
        gap2 = place_segment(seg2, k, theta_rad, Cx, Cy)[1]

        print(f"Mode:          reciprocal calibration")
        print(f"GPS start:     ({float(g_start['lat']):.6f}, {float(g_start['lon']):.6f})")
        print(f"GPS midpoint:  ({float(g_mid['lat']):.6f}, {float(g_mid['lon']):.6f})"
              f"  — {mid_dist:.1f} m @ {mid_az:.1f}°")
        print(f"GPS end:       ({float(g_end['lat']):.6f}, {float(g_end['lon']):.6f})"
              f"  — {end_dist:.1f} m from start"
              + (f"  (good return)" if end_dist < 10 else f"  (WARNING: not back at start)"))
        print()
        print(f"  Leg 1 (outbound):")
        print(f"    Duration:        {t1:.1f} s")
        print(f"    Avg sensor spd:  {avg_spd1:.3f} m/s  ({avg_spd1 * 1.9438:.2f} kn)")
        print(f"    Raw DR:          {math.hypot(Sx1, Sy1):.1f} m"
              f"  @ {math.degrees(math.atan2(Sx1, Sy1)) % 360:.1f}° az")
        print(f"    Required:        {math.hypot(dx, dy):.1f} m"
              f"  @ {math.degrees(math.atan2(dx, dy)) % 360:.1f}° az")
        print()
        print(f"  Leg 2 (return):")
        print(f"    Duration:        {t2:.1f} s")
        print(f"    Avg sensor spd:  {avg_spd2:.3f} m/s  ({avg_spd2 * 1.9438:.2f} kn)")
        print(f"    Raw DR:          {math.hypot(Sx2, Sy2):.1f} m"
              f"  @ {math.degrees(math.atan2(Sx2, Sy2)) % 360:.1f}° az")
        print(f"    Required:        {math.hypot(ex, ey):.1f} m"
              f"  @ {math.degrees(math.atan2(ex, ey)) % 360:.1f}° az")
        print()
        print(f"── Calibration results ─────────────────────────────────────────")
        print(f"  Speed factor k:    {k:.4f}×")
        true_spd1 = k * avg_spd1
        true_spd2 = k * avg_spd2
        print(f"  True DPV speed:    {true_spd1:.3f} m/s  ({true_spd1 * 1.9438:.2f} kn)"
              f"  [leg 1 avg sensor]")
        if abs(true_spd1 - true_spd2) > 0.02:
            print(f"                     {true_spd2:.3f} m/s  ({true_spd2 * 1.9438:.2f} kn)"
                  f"  [leg 2 — speeds differ, check throttle]")
        print(f"  Heading offset θ:  {theta_deg:+.2f}°"
              f"  ({'compass reads low' if theta_deg > 0 else 'compass reads high'})")
        print(f"  Current speed:     {current_spd:.3f} m/s  ({current_spd * 1.9438:.2f} kn)")
        print(f"  Current toward:    {current_dir:.1f}°")
        print()
        print(f"  Closure leg 1:     {gap1:.4f} m  (should be ~0)")
        print(f"  Closure leg 2:     {gap2:.4f} m  (should be ~0)")
        print()

    # ── Proportional mode ─────────────────────────────────────────────────────
    elif mode == "proportional":
        print(f"Mode:          proportional")
        print(f"Anchor blocks: {sum(1 for b in blocks if b['type'] == 'A')}  (GPS fixes / waypoint snaps)")
        print(f"DR segments:   {n_segs}"
              + (f"  ({len(solvable)} closable, {n_segs - len(solvable)} anchored on one side)"
                 if len(solvable) != n_segs else ""))
        print()

        for seg in segments:
            if seg["anchored"] == "both":
                positions, (cx, cy), total_path = proportional_correct_segment(seg)
                closure = math.hypot(cx, cy)
            else:
                # Cannot warp a segment closed against an anchor it doesn't have.
                positions, _, _ = place_segment(seg, 1.0, 0.0)
                cx = cy = closure = 0.0
                total_path = sum(spd * dt for dt, _hr, spd in _dr_steps(seg))

            for i, pos in enumerate(positions):
                adj_pos[seg["start_idx"] + i] = pos

            print(f"  Segment {seg['num']} of {n_segs}:{describe_anchor(seg)}")
            print_anchor_lines(seg)
            if seg["anchored"] == "both":
                x0, y0 = latlon_to_xy(float(seg["start_row"]["lat"]),
                                      float(seg["start_row"]["lon"]))
                xN, yN = latlon_to_xy(float(seg["end_row"]["lat"]),
                                      float(seg["end_row"]["lon"]))
                print(f"    GPS gap:         {math.hypot(xN - x0, yN - y0):.1f} m")
            print(f"    Estimated rows:  {len(seg['est_rows'])}")
            print(f"    Total path (DR): {total_path:.1f} m")
            if seg["anchored"] == "both":
                pct_str = (f"{100 * closure / total_path:.1f}% of path"
                           if total_path > 0 else "n/a, zero DR path")
                print(f"    DR closure err:  {closure:.1f} m  ({cx:+.1f} E, {cy:+.1f} N)  "
                      f"({pct_str})")
                print(f"    After correct:   0.0000 m  (exact by construction)")
            else:
                print(f"    DR closure err:  n/a  (cannot close a one-anchored segment)")
            print()

    # ── Joint mode ────────────────────────────────────────────────────────────
    else:
        solution = None
        if solvable:
            solve_vecs = [compute_dr_vectors(seg) for seg in solvable]
            solution = solve_k_theta_multi(solve_vecs)

            # Loop degeneracy.  Joint mode infers the speed factor from how far
            # the diver *net* travelled between two fixes.  On an out-and-back —
            # swim to the wreck, swim home — the net displacement is near zero no
            # matter how far they actually swam, so it says almost nothing about
            # speed, and the least-squares fit will happily shrink k towards zero
            # to reconcile a long path with a short displacement.  The number it
            # returns is a real minimiser and complete nonsense physically.
            num = sum(math.hypot(dx, dy) * math.hypot(Sx, Sy)
                      for Sx, Sy, dx, dy in solve_vecs)
            den = sum(Sx * Sx + Sy * Sy for Sx, Sy, _dx, _dy in solve_vecs)
            closure_ratio = num / den if den > 1e-6 else 0.0
            if solution is not None and closure_ratio < 0.25:
                print(f"WARNING: this dive comes back to where it started — net displacement")
                print(f"  is only {100 * closure_ratio:.0f}% of the distance travelled.  Joint mode")
                print(f"  infers the speed factor from net displacement, so on a loop like this")
                print(f"  it is ill-conditioned: the k below is a genuine least-squares fit and")
                print(f"  almost certainly physically meaningless.")
                print(f"  Prefer --mode proportional here, or bound the fit with --max-k-error.")
                print()

        if solution is None:
            # Nothing in this dive constrains the sensors: either no segment is
            # anchored at both ends, or the ones that are held still the whole
            # time.  We can still *place* a one-anchored segment — its shape is
            # real and one of its endpoints is known — but we have no basis on
            # which to scale or rotate it.  Apply the identity and say so
            # loudly, rather than pretending to a correction we cannot make.
            k_free, theta_free = 1.0, 0.0
            if not solvable:
                reason = "no DR segment is anchored at both ends"
            else:
                reason = "the fully-anchored segments carry almost no DR displacement"
            print(f"WARNING: {reason} — nothing constrains the solve.")
            print("  Applying k = 1.0, θ = 0.0 (no sensor correction).  Segments are placed")
            print("  by whichever single anchor they have; their shape is unadjusted.")
            print()
        else:
            k_free, theta_free = solution

        constrained = False
        k_used     = k_free
        theta_used = theta_free

        if args.max_theta is not None and solution is not None:
            max_rad = math.radians(args.max_theta)
            clipped = max(-max_rad, min(max_rad, theta_free))
            if clipped != theta_free:
                constrained = True
            theta_used = clipped

        if args.max_k_error is not None and solution is not None:
            clipped = max(1.0 - args.max_k_error, min(1.0 + args.max_k_error, k_free))
            if clipped != k_free:
                constrained = True
            k_used = clipped

        # Reintegrate with physical (k, θ) correction; collect closure gaps for reporting
        seg_results = {}
        for seg in segments:
            positions, gap_m, gap_xy = place_segment(seg, k_used, theta_used)
            seg_results[seg["num"]] = (gap_m, gap_xy)
            for i, pos in enumerate(positions):
                adj_pos[seg["start_idx"] + i] = pos

        theta_free_deg = (math.degrees(theta_free) + 180) % 360 - 180
        theta_used_deg = (math.degrees(theta_used) + 180) % 360 - 180

        print(f"Mode:          joint")
        print(f"Anchor blocks: {sum(1 for b in blocks if b['type'] == 'A')}  (GPS fixes / waypoint snaps)")
        print(f"DR segments:   {n_segs}"
              + (f"  ({len(solvable)} constrain the solve, "
                 f"{n_segs - len(solvable)} anchored on one side or not at all)"
                 if len(solvable) != n_segs else
                 ("  (LS fit, single (k,θ) applied to all)" if n_segs > 1 else "")))
        print()

        for seg in segments:
            print(f"  Segment {seg['num']} of {n_segs}:{describe_anchor(seg)}")
            print_anchor_lines(seg)
            print(f"    Estimated rows:  {len(seg['est_rows'])}")

            if seg["anchored"] == "both":
                Sx, Sy, dx, dy = compute_dr_vectors(seg)
                raw_az = math.degrees(math.atan2(Sx, Sy))
                req_az = math.degrees(math.atan2(dx, dy))
                print(f"    Raw DR:          {math.hypot(Sx, Sy):.1f} m  @ {raw_az:.1f}° az")
                print(f"    Required:        {math.hypot(dx, dy):.1f} m  @ {req_az:.1f}° az")
                gap_m, (gx, gy) = seg_results[seg["num"]]
                if solution is not None and len(solvable) == 1:
                    print(f"    Closure gap:     0.0000 m  (exact, single constrained segment)")
                else:
                    print(f"    Closure gap:     {gap_m:.2f} m  ({gx:+.1f} E, {gy:+.1f} N)")
            else:
                Sx, Sy = integrate(seg)[-1]
                raw_az = math.degrees(math.atan2(Sx, Sy))
                print(f"    Raw DR:          {math.hypot(Sx, Sy):.1f} m  @ {raw_az:.1f}° az")
                print(f"    Required:        n/a  (only one anchor — nothing to close against)")
            print()

        print(f"── {'Constrained' if constrained else 'Free'} solution ─────────────────────────────────────")
        print(f"  Speed factor k:    {k_used:.4f}×"
              + (f"  [free: {k_free:.4f}]" if constrained and k_used != k_free else ""))
        print(f"  Heading offset θ:  {theta_used_deg:+.2f}°  "
              f"({'compass reads low' if theta_used_deg > 0 else 'compass reads high'})"
              + (f"  [free: {theta_free_deg:+.2f}°]" if constrained and theta_used != theta_free else ""))
        print()

    # ── Write output ──────────────────────────────────────────────────────────
    output_rows = []
    for i, row in enumerate(rows):
        r = {k: v for k, v in row.items() if not k.startswith("_")}
        ax, ay = adj_pos[i]
        r["adj_pos_x_m"] = f"{ax:.2f}"
        r["adj_pos_y_m"] = f"{ay:.2f}"
        if row["pos_src"] in ANCHOR_SRCS:
            r["adj_lat"] = row["lat"]
            r["adj_lon"] = row["lon"]
        else:
            adj_lat, adj_lon = xy_to_latlon(ax, ay)
            r["adj_lat"] = f"{adj_lat:.8f}"
            r["adj_lon"] = f"{adj_lon:.8f}"
        output_rows.append(r)

    with open(output_file, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=out_fieldnames)
        writer.writeheader()
        writer.writerows(output_rows)

    print(f"Output:        {output_file}")


if __name__ == "__main__":
    main()
