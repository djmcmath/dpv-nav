#!/usr/bin/env python3
"""
Correct a DPV-nav dead-reckoning track to close on known GPS endpoints.

Supports any number of GPS "anchor" blocks in the log — one at the start,
one at the end, and optionally one or more in the middle (e.g. a manually
inserted wreck fix, a surface interval mid-dive, etc.).

GPS block structure:
    - A "GPS block" is a contiguous run of rows with pos_src = "G".
    - The *last* row of a GPS block anchors the start of the following DR
      segment (most recent fix before going under).
    - The *first* row of a GPS block anchors the end of the preceding DR
      segment (first confirmed fix after surfacing).
    - For a single-row GPS block (e.g. a manually inserted midpoint), both
      anchors are the same row.

Three correction modes (--mode):

  reciprocal
    Calibration mode.  Requires a GEGEG log structure — outbound leg, GPS
    midpoint fix, return leg on a reciprocal course.  Solves simultaneously
    for speed factor (k), heading offset (θ), and water current vector (Cx, Cy)
    using a 4×4 linear system derived from the two GPS-constrained leg
    displacements.  Exact solution (no LS); current and sensor errors are
    isolated without approximation.

    Corrected EPs include current drift: position = k·speed in dir (heading+θ)
    plus current velocity, each dt.  Use this mode to characterize sensor errors
    at different DPV speeds and measure ambient current.

  joint (default)
    Solves jointly for a constant speed scale (k) and constant heading
    offset (θ) using closed-form weighted least squares across all DR
    segments.  A single (k, θ) pair is applied to every segment —
    appropriate when the systematic errors (compass bias, flow-meter
    k-factor) are constant across the whole dive.

    Corrected EPs are re-integrated as: adj[i+1] = adj[i] + k·speed·dt
    in direction (heading + θ).  This is the physically meaningful
    reconstruction — what the device would have shown with correct
    sensors.  GPS anchors are used only to solve for (k, θ); the
    corrected track is not warped to force closure (use proportional
    mode for that).  Per-segment closure gaps after correction are
    reported and reflect the quality of the joint fit.

    Normal equations decouple (A^T A = D · I₂, D = Σ|DR_i|²), so no
    numpy is required.  The single-segment case is exact (zero gap).

    Optional --max-theta / --max-k-error flags constrain the solution.

  proportional
    Applies distance-weighted closure correction independently to each
    segment.  Makes no assumption about error cause.  Works for loop
    segments where joint degenerates.  Always closes each segment exactly.

Note that GPS rows pass through unchanged in both modes.
Adds columns: adj_pos_x_m, adj_pos_y_m, adj_lat, adj_lon
In order to re-ingest into dive map, copy the adj_ columns back to the lat/lon columns.

Usage:
    python tools/correct_track.py <logfile.csv> [--mode reciprocal|joint|proportional]
                                                 [--max-theta DEG]
                                                 [--max-k-error FRAC]

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


# ── Block parsing ─────────────────────────────────────────────────────────────

def parse_blocks(rows: list[dict]) -> list[dict]:
    """
    Group consecutive rows with the same pos_src into blocks.
    Returns list of {'type': str, 'rows': list[dict], 'start_idx': int}.
    """
    if not rows:
        return []
    blocks = []
    cur_type  = rows[0]["pos_src"]
    cur_rows  = [rows[0]]
    cur_start = 0
    for i, row in enumerate(rows[1:], 1):
        src = row["pos_src"]
        if src == cur_type:
            cur_rows.append(row)
        else:
            blocks.append({"type": cur_type, "rows": list(cur_rows), "start_idx": cur_start})
            cur_type  = src
            cur_rows  = [row]
            cur_start = i
    blocks.append({"type": cur_type, "rows": cur_rows, "start_idx": cur_start})
    return blocks


def validate_blocks(blocks: list[dict]) -> None:
    """Validate: must alternate G/E starting and ending with G."""
    if not blocks:
        print("Error: no data rows found.")
        sys.exit(1)
    if blocks[0]["type"] != "G":
        print("Error: file must begin with GPS rows (pos_src = 'G').")
        sys.exit(1)
    if blocks[-1]["type"] != "G":
        print("Error: file must end with GPS rows (pos_src = 'G').")
        sys.exit(1)
    for i, b in enumerate(blocks):
        expected = "G" if i % 2 == 0 else "E"
        if b["type"] != expected:
            print(f"Error: expected {'GPS' if expected == 'G' else 'estimated'} block "
                  f"at block position {i}, found '{b['type']}'.")
            sys.exit(1)
    if not any(b["type"] == "E" for b in blocks):
        print("Error: no estimated rows found.")
        sys.exit(1)


def build_segments(blocks: list[dict]) -> list[dict]:
    """
    Return one segment dict per E-block.
    start_row: last G in the preceding G-block (most recent fix before DR)
    end_row:   first G in the following G-block (first fix after DR)
    """
    segments = []
    for i, b in enumerate(blocks):
        if b["type"] == "E":
            segments.append({
                "num":       len(segments) + 1,
                "start_row": blocks[i - 1]["rows"][-1],
                "end_row":   blocks[i + 1]["rows"][0],
                "est_rows":  b["rows"],
                "start_idx": b["start_idx"],
            })
    return segments


# ── Per-segment math ──────────────────────────────────────────────────────────

def compute_dr_vectors(seg: dict) -> tuple[float, float, float, float]:
    """
    Integrate the raw DR for one segment.
    Returns (Sx, Sy, dx, dy) where:
        (Sx, Sy) = raw DR displacement sum
        (dx, dy) = GPS required displacement (end − start)
    """
    x0, y0 = latlon_to_xy(float(seg["start_row"]["lat"]), float(seg["start_row"]["lon"]))
    xN, yN = latlon_to_xy(float(seg["end_row"]["lat"]),   float(seg["end_row"]["lon"]))
    dx, dy = xN - x0, yN - y0

    Sx = Sy = 0.0
    prev_ts = int(seg["start_row"]["timestamp_ms"])
    for row in seg["est_rows"]:
        ts  = int(row["timestamp_ms"])
        dt  = (ts - prev_ts) / 1000.0
        hr  = math.radians(float(row["heading_deg"]))
        spd = float(row["speed_ms"])
        Sx += spd * math.sin(hr) * dt
        Sy += spd * math.cos(hr) * dt
        prev_ts = ts

    return Sx, Sy, dx, dy



def reintegrate(seg: dict, k: float, theta_rad: float):
    """
    Re-integrate one segment under (k, θ): adj[i+1] = adj[i] + k·spd·dt
    in direction (heading + θ), starting from the GPS anchor start_row.

    Returns (positions, gap_m, (gap_x, gap_y)) where gap is the distance
    from the last corrected EP to the GPS end anchor — the closure residual
    after physical correction.  For a single-segment exact solution this
    is zero; for multi-segment LS it reflects how well the joint (k, θ)
    fits this segment.
    """
    start_row = seg["start_row"]
    end_row   = seg["end_row"]
    est_rows  = seg["est_rows"]

    x, y = latlon_to_xy(float(start_row["lat"]), float(start_row["lon"]))
    xN, yN = latlon_to_xy(float(end_row["lat"]), float(end_row["lon"]))

    prev_ts = int(start_row["timestamp_ms"])
    positions: list[tuple[float, float]] = []

    for row in est_rows:
        ts  = int(row["timestamp_ms"])
        dt  = (ts - prev_ts) / 1000.0
        hr  = math.radians(float(row["heading_deg"]))
        spd = float(row["speed_ms"])
        x += k * spd * math.sin(hr + theta_rad) * dt
        y += k * spd * math.cos(hr + theta_rad) * dt
        prev_ts = ts
        positions.append((x, y))

    gap_x = xN - positions[-1][0]
    gap_y = yN - positions[-1][1]
    return positions, math.hypot(gap_x, gap_y), (gap_x, gap_y)


def reintegrate_with_current(seg: dict, k: float, theta_rad: float,
                              Cx: float, Cy: float):
    """
    Re-integrate one segment with DPV correction (k, θ) plus current (Cx, Cy).
    Each step: adj += (k·spd·(sin(h+θ), cos(h+θ)) + (Cx, Cy)) · dt

    Returns (positions, gap_m, (gap_x, gap_y)).  For reciprocal calibration the
    gap is exact-solution residual (should be near-zero floating-point noise).
    """
    start_row = seg["start_row"]
    end_row   = seg["end_row"]
    est_rows  = seg["est_rows"]

    x, y = latlon_to_xy(float(start_row["lat"]), float(start_row["lon"]))
    xN, yN = latlon_to_xy(float(end_row["lat"]), float(end_row["lon"]))

    prev_ts = int(start_row["timestamp_ms"])
    positions: list[tuple[float, float]] = []

    for row in est_rows:
        ts  = int(row["timestamp_ms"])
        dt  = (ts - prev_ts) / 1000.0
        hr  = math.radians(float(row["heading_deg"]))
        spd = float(row["speed_ms"])
        x += (k * spd * math.sin(hr + theta_rad) + Cx) * dt
        y += (k * spd * math.cos(hr + theta_rad) + Cy) * dt
        prev_ts = ts
        positions.append((x, y))

    gap_x = xN - positions[-1][0]
    gap_y = yN - positions[-1][1]
    return positions, math.hypot(gap_x, gap_y), (gap_x, gap_y)


def _leg_stats(seg: dict) -> tuple[float, float]:
    """Return (Σspd·dt, elapsed_s) for a segment — average speed = ratio."""
    prev_ts   = int(seg["start_row"]["timestamp_ms"])
    spd_int   = 0.0
    for row in seg["est_rows"]:
        ts      = int(row["timestamp_ms"])
        dt      = (ts - prev_ts) / 1000.0
        spd_int += float(row["speed_ms"]) * dt
        prev_ts  = ts
    elapsed = (int(seg["est_rows"][-1]["timestamp_ms"]) -
               int(seg["start_row"]["timestamp_ms"])) / 1000.0
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

    Returns (k, theta_rad).
    """
    num_c = num_s = denom = 0.0
    for Sx, Sy, dx, dy in vec_list:
        num_c += Sx * dx + Sy * dy
        num_s += Sy * dx - Sx * dy
        denom += Sx * Sx + Sy * Sy

    if denom < 1e-6:
        print("Error: near-zero total DR displacement — cannot solve for k and θ.")
        sys.exit(1)

    c = num_c / denom
    s = num_s / denom
    return math.hypot(c, s), math.atan2(s, c)


# ── Proportional solver ───────────────────────────────────────────────────────

def proportional_correct_segment(seg: dict):
    """
    Distance-weighted proportional closure for one segment.
    Returns (positions, dr_end, (closure_x, closure_y), total_path_m).
    """
    x, y = latlon_to_xy(float(seg["start_row"]["lat"]), float(seg["start_row"]["lon"]))
    xN, yN = latlon_to_xy(float(seg["end_row"]["lat"]), float(seg["end_row"]["lon"]))

    prev_ts = int(seg["start_row"]["timestamp_ms"])
    raw_positions: list[tuple[float, float]] = []
    cum_dist: list[float] = []
    total = 0.0

    for row in seg["est_rows"]:
        ts  = int(row["timestamp_ms"])
        dt  = (ts - prev_ts) / 1000.0
        hr  = math.radians(float(row["heading_deg"]))
        spd = float(row["speed_ms"])
        x += spd * math.sin(hr) * dt
        y += spd * math.cos(hr) * dt
        total += spd * dt
        prev_ts = ts
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


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input_file")
    parser.add_argument("--mode", choices=["reciprocal", "joint", "proportional"],
                        default="joint", help="Correction mode (default: joint)")
    parser.add_argument("--max-theta", type=float, default=None, metavar="DEG",
                        help="[joint] Constrain heading correction to ±DEG degrees")
    parser.add_argument("--max-k-error", type=float, default=None, metavar="FRAC",
                        help="[joint] Constrain speed factor to 1±FRAC (e.g. 0.15)")
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

    blocks   = parse_blocks(rows)
    validate_blocks(blocks)
    segments = build_segments(blocks)

    n_segs = len(segments)
    new_fields     = ["adj_pos_x_m", "adj_pos_y_m", "adj_lat", "adj_lon"]
    out_fieldnames = fieldnames + new_fields

    # adj_pos: global row index → (adj_x, adj_y)
    adj_pos: dict[int, tuple[float, float]] = {}

    # GPS rows always pass through at their own lat/lon
    for block in blocks:
        if block["type"] == "G":
            for i, row in enumerate(block["rows"]):
                try:
                    gx, gy = latlon_to_xy(float(row["lat"]), float(row["lon"]))
                except ValueError as e:
                    global_idx = block["start_idx"] + i
                    print(f"Error: bad lat/lon in GPS row {global_idx + 2} "
                          f"(CSV line {global_idx + 2}): lat={row['lat']!r} lon={row['lon']!r}")
                    print(f"  {e}")
                    sys.exit(1)
                adj_pos[block["start_idx"] + i] = (gx, gy)

    # ── Reciprocal calibration mode ───────────────────────────────────────────
    if args.mode == "reciprocal":
        if n_segs != 2:
            print(f"Error: reciprocal mode requires exactly 2 DR segments (G-E-G-E-G), "
                  f"found {n_segs}.")
            sys.exit(1)

        seg1, seg2 = segments[0], segments[1]
        vec1 = compute_dr_vectors(seg1)
        vec2 = compute_dr_vectors(seg2)

        k, theta_rad, Cx, Cy, t1, t2, avg_spd1, avg_spd2 = \
            solve_reciprocal(seg1, seg2, vec1, vec2)

        for seg in segments:
            positions, gap_m, _ = reintegrate_with_current(seg, k, theta_rad, Cx, Cy)
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

        gap1 = reintegrate_with_current(seg1, k, theta_rad, Cx, Cy)[1]
        gap2 = reintegrate_with_current(seg2, k, theta_rad, Cx, Cy)[1]

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
    elif args.mode == "proportional":
        print(f"Mode:          proportional")
        print(f"GPS blocks:    {n_segs + 1}")
        print(f"DR segments:   {n_segs}")
        print()

        for seg in segments:
            positions, (cx, cy), total_path = proportional_correct_segment(seg)
            for i, pos in enumerate(positions):
                adj_pos[seg["start_idx"] + i] = pos

            x0, y0 = latlon_to_xy(float(seg["start_row"]["lat"]), float(seg["start_row"]["lon"]))
            xN, yN = latlon_to_xy(float(seg["end_row"]["lat"]),   float(seg["end_row"]["lon"]))
            gps_gap = math.hypot(xN - x0, yN - y0)
            closure = math.hypot(cx, cy)

            print(f"  Segment {seg['num']} of {n_segs}:")
            print(f"    GPS start:       ({float(seg['start_row']['lat']):.6f}, "
                  f"{float(seg['start_row']['lon']):.6f})")
            print(f"    GPS end:         ({float(seg['end_row']['lat']):.6f}, "
                  f"{float(seg['end_row']['lon']):.6f})")
            print(f"    GPS gap:         {gps_gap:.1f} m")
            print(f"    Estimated rows:  {len(seg['est_rows'])}")
            print(f"    Total path (DR): {total_path:.1f} m")
            print(f"    DR closure err:  {closure:.1f} m  ({cx:+.1f} E, {cy:+.1f} N)  "
                  f"({100 * closure / total_path:.1f}% of path)")
            print(f"    After correct:   0.0000 m  (exact by construction)")
            print()

    # ── Joint mode ────────────────────────────────────────────────────────────
    else:
        vec_list = [compute_dr_vectors(seg) for seg in segments]
        k_free, theta_free = solve_k_theta_multi(vec_list)

        constrained = False
        k_used     = k_free
        theta_used = theta_free

        if args.max_theta is not None:
            max_rad = math.radians(args.max_theta)
            clipped = max(-max_rad, min(max_rad, theta_free))
            if clipped != theta_free:
                constrained = True
            theta_used = clipped

        if args.max_k_error is not None:
            clipped = max(1.0 - args.max_k_error, min(1.0 + args.max_k_error, k_free))
            if clipped != k_free:
                constrained = True
            k_used = clipped

        # Reintegrate with physical (k, θ) correction; collect closure gaps for reporting
        seg_results = []  # (positions, gap_m, (gap_x, gap_y))
        for seg in segments:
            result = reintegrate(seg, k_used, theta_used)
            seg_results.append(result)
            positions, _, _ = result
            for i, pos in enumerate(positions):
                adj_pos[seg["start_idx"] + i] = pos

        theta_free_deg = (math.degrees(theta_free) + 180) % 360 - 180
        theta_used_deg = (math.degrees(theta_used) + 180) % 360 - 180

        print(f"Mode:          joint")
        print(f"GPS blocks:    {n_segs + 1}")
        print(f"DR segments:   {n_segs}"
              + ("  (LS fit, single (k,θ) applied to all)" if n_segs > 1 else ""))
        print()

        for seg, (Sx, Sy, dx, dy), (_, gap_m, (gx, gy)) in zip(segments, vec_list, seg_results):
            raw_az = math.degrees(math.atan2(Sx, Sy))
            req_az = math.degrees(math.atan2(dx, dy))
            print(f"  Segment {seg['num']} of {n_segs}:")
            print(f"    GPS start:       ({float(seg['start_row']['lat']):.6f}, "
                  f"{float(seg['start_row']['lon']):.6f})")
            print(f"    GPS end:         ({float(seg['end_row']['lat']):.6f}, "
                  f"{float(seg['end_row']['lon']):.6f})")
            print(f"    Estimated rows:  {len(seg['est_rows'])}")
            print(f"    Raw DR:          {math.hypot(Sx, Sy):.1f} m  @ {raw_az:.1f}° az")
            print(f"    Required:        {math.hypot(dx, dy):.1f} m  @ {req_az:.1f}° az")
            if n_segs == 1:
                print(f"    Closure gap:     0.0000 m  (exact, single segment)")
            else:
                print(f"    Closure gap:     {gap_m:.2f} m  ({gx:+.1f} E, {gy:+.1f} N)")
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
        r = dict(row)
        ax, ay = adj_pos[i]
        r["adj_pos_x_m"] = f"{ax:.2f}"
        r["adj_pos_y_m"] = f"{ay:.2f}"
        if row["pos_src"] == "G":
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
