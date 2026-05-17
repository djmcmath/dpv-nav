#!/usr/bin/env python3
"""
Correct a DPV-nav dead-reckoning track to close on known GPS endpoints.

Expects a log CSV with the structure:
    GPS rows  →  Estimated rows  →  GPS rows   (no multi-gap)

Two correction modes (--mode):

  joint (default)
    Solves jointly for a constant speed scale (k) and constant heading offset
    (theta) such that the re-integrated DR track starts at the last pre-dive
    GPS fix and ends exactly at the first post-dive GPS fix.  Works best for
    dives with significant net displacement between the two GPS fixes.
    Optional --max-theta / --max-k-error flags clip to a feasible box and
    report the residual closure error.

  proportional
    Distributes the raw DR closure error (DR endpoint → post-dive GPS fix)
    linearly across the track, weighted by cumulative path distance.  Makes no
    assumption about error cause.  Works for loop dives where the joint solver
    degenerates.  Always closes with 0 residual.

GPS rows pass through unchanged in both modes.
Adds columns: adj_pos_x_m, adj_pos_y_m, adj_lat, adj_lon

Usage:
    python tools/correct_track.py <logfile.csv> [--mode joint|proportional]
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


def find_estimated_segment(src: list[str]):
    """Return (first_e, last_e, start_gps_idx, end_gps_idx) or exit."""
    first_e = next((i for i, s in enumerate(src) if s == "E"), None)
    if first_e is None:
        print("Error: no estimated rows found in CSV.")
        sys.exit(1)

    last_e_rev = next((i for i, s in enumerate(reversed(src)) if s == "E"), None)
    last_e = len(src) - 1 - last_e_rev

    start_gps_idx = first_e - 1
    end_gps_idx = last_e + 1

    if start_gps_idx < 0 or src[start_gps_idx] != "G":
        print("Error: no GPS row immediately before the estimated section.")
        sys.exit(1)

    if end_gps_idx >= len(src) or src[end_gps_idx] != "G":
        print("Error: no GPS row immediately after the estimated section.")
        sys.exit(1)

    return first_e, last_e, start_gps_idx, end_gps_idx


def solve_k_theta(start_row, end_row, est_rows: list[dict]):
    """
    Compute speed scale k and heading offset theta (radians) such that
    re-integrating est_rows with (k * speed, heading + theta) produces a
    displacement equal to (end_GPS - start_GPS).

    Derivation: the re-integration sums to:
        adj_x = k * (cos(theta)*Sx + sin(theta)*Sy)
        adj_y = k * (cos(theta)*Sy - sin(theta)*Sx)
    Setting (adj_x, adj_y) = (dx, dy) and solving:
        k     = hypot(dx, dy) / hypot(Sx, Sy)
        theta = atan2(Sy, Sx) - atan2(dy, dx)

    Sign convention for theta:
        positive → raw DR points clockwise of target; add theta to all headings
                   (physically: compass was reading too low)
        negative → raw DR points counterclockwise of target; add theta to all
                   headings (physically: compass was reading too high)

    Returns (k, theta_rad, Sx, Sy, dx, dy).
    """
    x0, y0 = latlon_to_xy(float(start_row["lat"]), float(start_row["lon"]))
    xN, yN = latlon_to_xy(float(end_row["lat"]), float(end_row["lon"]))

    dx = xN - x0
    dy = yN - y0

    Sx = Sy = 0.0
    prev_ts = int(start_row["timestamp_ms"])

    for row in est_rows:
        ts = int(row["timestamp_ms"])
        dt = (ts - prev_ts) / 1000.0
        heading_rad = math.radians(float(row["heading_deg"]))
        speed = float(row["speed_ms"])
        Sx += speed * math.sin(heading_rad) * dt
        Sy += speed * math.cos(heading_rad) * dt
        prev_ts = ts

    raw_dist = math.hypot(Sx, Sy)
    if raw_dist < 1e-6:
        print("Error: near-zero raw DR displacement — cannot solve for k and theta.")
        sys.exit(1)

    req_dist = math.hypot(dx, dy)
    k = req_dist / raw_dist
    theta_rad = math.atan2(Sy, Sx) - math.atan2(dy, dx)

    return k, theta_rad, Sx, Sy, dx, dy


def closure_error(start_row, est_rows, end_row, k, theta_rad):
    """Re-integrate and return distance between last estimated point and end GPS."""
    x, y = latlon_to_xy(float(start_row["lat"]), float(start_row["lon"]))
    prev_ts = int(start_row["timestamp_ms"])
    for row in est_rows:
        ts = int(row["timestamp_ms"])
        dt = (ts - prev_ts) / 1000.0
        hr = math.radians(float(row["heading_deg"]))
        spd = float(row["speed_ms"])
        x += k * spd * math.sin(hr + theta_rad) * dt
        y += k * spd * math.cos(hr + theta_rad) * dt
        prev_ts = ts
    ex, ey = latlon_to_xy(float(end_row["lat"]), float(end_row["lon"]))
    return math.hypot(x - ex, y - ey), x, y


def reintegrate(start_row, est_rows: list[dict], k: float, theta_rad: float):
    """Return list of (adj_x, adj_y) for each estimated row."""
    x, y = latlon_to_xy(float(start_row["lat"]), float(start_row["lon"]))
    prev_ts = int(start_row["timestamp_ms"])
    positions = []
    for row in est_rows:
        ts = int(row["timestamp_ms"])
        dt = (ts - prev_ts) / 1000.0
        hr = math.radians(float(row["heading_deg"]))
        spd = float(row["speed_ms"])
        x += k * spd * math.sin(hr + theta_rad) * dt
        y += k * spd * math.cos(hr + theta_rad) * dt
        prev_ts = ts
        positions.append((x, y))
    return positions


def proportional_correct(start_row, est_rows: list[dict], end_row):
    """
    Raw DR integration + distance-proportional closure correction.

    For each estimated row at cumulative path fraction t (0→1):
        adj_pos(i) = DR_pos(i) + t(i) * (GPS_end - DR_end)

    Weighting by cumulative path distance rather than time means stationary
    periods don't accumulate correction, which is more physically motivated.
    Always closes with 0 residual.

    Returns (positions, dr_end, closure_vec, total_path_m).
    """
    x0, y0 = latlon_to_xy(float(start_row["lat"]), float(start_row["lon"]))
    xN, yN = latlon_to_xy(float(end_row["lat"]),   float(end_row["lon"]))

    # Pass 1: raw DR + cumulative distances
    x, y = x0, y0
    prev_ts = int(start_row["timestamp_ms"])
    raw_positions = []
    cum_dist = []
    total = 0.0

    for row in est_rows:
        ts  = int(row["timestamp_ms"])
        dt  = (ts - prev_ts) / 1000.0
        hr  = math.radians(float(row["heading_deg"]))
        spd = float(row["speed_ms"])
        step = spd * dt
        x += spd * math.sin(hr) * dt
        y += spd * math.cos(hr) * dt
        total += step
        prev_ts = ts
        raw_positions.append((x, y))
        cum_dist.append(total)

    dr_end = raw_positions[-1]
    closure_x = xN - dr_end[0]
    closure_y = yN - dr_end[1]

    # Pass 2: apply proportional correction
    positions = []
    for (rx, ry), cd in zip(raw_positions, cum_dist):
        t = cd / total if total > 0 else 0.0
        positions.append((rx + t * closure_x, ry + t * closure_y))

    return positions, dr_end, (closure_x, closure_y), total


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input_file")
    parser.add_argument("--mode", choices=["joint", "proportional"], default="joint",
                        help="Correction mode (default: joint)")
    parser.add_argument("--max-theta", type=float, default=None, metavar="DEG",
                        help="[joint] Constrain heading correction to ±DEG degrees")
    parser.add_argument("--max-k-error", type=float, default=None, metavar="FRAC",
                        help="[joint] Constrain speed factor to 1±FRAC (e.g. 0.15)")
    args = parser.parse_args()

    input_file = args.input_file
    base, ext = os.path.splitext(input_file)
    output_file = f"{base}_corrected{ext}"

    with open(input_file, newline="") as f:
        reader = csv.DictReader(f)
        fieldnames = list(reader.fieldnames)
        rows = list(reader)

    if not rows:
        print("No data rows found.")
        sys.exit(1)

    src = [r["pos_src"] for r in rows]
    first_e, last_e, start_gps_idx, end_gps_idx = find_estimated_segment(src)

    start_row = rows[start_gps_idx]
    end_row   = rows[end_gps_idx]
    est_rows  = rows[first_e:last_e + 1]

    new_fields = ["adj_pos_x_m", "adj_pos_y_m", "adj_lat", "adj_lon"]
    out_fieldnames = fieldnames + new_fields

    # ── Proportional mode ───────────────────────────────────────────────────
    if args.mode == "proportional":
        adj_positions, dr_end, (cx, cy), total_path = \
            proportional_correct(start_row, est_rows, end_row)

        output_rows = []
        for i, row in enumerate(rows):
            r = dict(row)
            if first_e <= i <= last_e:
                ax, ay = adj_positions[i - first_e]
                adj_lat, adj_lon = xy_to_latlon(ax, ay)
                r["adj_pos_x_m"] = f"{ax:.2f}"
                r["adj_pos_y_m"] = f"{ay:.2f}"
                r["adj_lat"]     = f"{adj_lat:.8f}"
                r["adj_lon"]     = f"{adj_lon:.8f}"
            else:
                gx, gy = latlon_to_xy(float(row["lat"]), float(row["lon"]))
                r["adj_pos_x_m"] = f"{gx:.2f}"
                r["adj_pos_y_m"] = f"{gy:.2f}"
                r["adj_lat"]     = row["lat"]
                r["adj_lon"]     = row["lon"]
            output_rows.append(r)

        with open(output_file, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=out_fieldnames)
            writer.writeheader()
            writer.writerows(output_rows)

        x0, y0 = latlon_to_xy(float(start_row["lat"]), float(start_row["lon"]))
        xN, yN = latlon_to_xy(float(end_row["lat"]),   float(end_row["lon"]))
        gps_gap = math.hypot(xN - x0, yN - y0)
        closure = math.hypot(cx, cy)

        print(f"Mode:               proportional")
        print(f"GPS start:          ({float(start_row['lat']):.6f}, {float(start_row['lon']):.6f})")
        print(f"GPS end:            ({float(end_row['lat']):.6f}, {float(end_row['lon']):.6f})")
        print(f"GPS start→end gap:  {gps_gap:.1f} m")
        print(f"Estimated rows:     {len(est_rows)}")
        print(f"Total path (DR):    {total_path:.1f} m")
        print()
        print(f"DR end → GPS end:   {closure:.1f} m  ({cx:+.1f} E, {cy:+.1f} N)")
        print(f"  as % of path:     {100*closure/total_path:.1f}%")
        print(f"Peak mid-path correction: ~{closure/2:.1f} m  (at 50% of path distance)")
        print(f"Closure error:      0.0000 m  (exact by construction)")
        print()
        print(f"Output:             {output_file}")
        return

    # ── Joint mode (default) ────────────────────────────────────────────────
    k_free, theta_free, Sx, Sy, dx, dy = solve_k_theta(start_row, end_row, est_rows)

    constrained = False
    k_used     = k_free
    theta_used = theta_free

    if args.max_theta is not None:
        max_theta_rad = math.radians(args.max_theta)
        clipped = max(-max_theta_rad, min(max_theta_rad, theta_free))
        if clipped != theta_free:
            constrained = True
        theta_used = clipped

    if args.max_k_error is not None:
        k_lo = 1.0 - args.max_k_error
        k_hi = 1.0 + args.max_k_error
        clipped = max(k_lo, min(k_hi, k_free))
        if clipped != k_free:
            constrained = True
        k_used = clipped

    adj_positions = reintegrate(start_row, est_rows, k_used, theta_used)
    residual, _, _ = closure_error(start_row, est_rows, end_row, k_used, theta_used)

    output_rows = []
    for i, row in enumerate(rows):
        r = dict(row)
        if first_e <= i <= last_e:
            ax, ay = adj_positions[i - first_e]
            adj_lat, adj_lon = xy_to_latlon(ax, ay)
            r["adj_pos_x_m"] = f"{ax:.2f}"
            r["adj_pos_y_m"] = f"{ay:.2f}"
            r["adj_lat"]     = f"{adj_lat:.8f}"
            r["adj_lon"]     = f"{adj_lon:.8f}"
        else:
            gx, gy = latlon_to_xy(float(row["lat"]), float(row["lon"]))
            r["adj_pos_x_m"] = f"{gx:.2f}"
            r["adj_pos_y_m"] = f"{gy:.2f}"
            r["adj_lat"]     = row["lat"]
            r["adj_lon"]     = row["lon"]
        output_rows.append(r)

    with open(output_file, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=out_fieldnames)
        writer.writeheader()
        writer.writerows(output_rows)

    theta_free_deg = (math.degrees(theta_free) + 180) % 360 - 180
    theta_used_deg = (math.degrees(theta_used) + 180) % 360 - 180
    raw_az = math.degrees(math.atan2(Sx, Sy))
    req_az = math.degrees(math.atan2(dx, dy))

    print(f"Mode:               joint")
    print(f"GPS start:          ({float(start_row['lat']):.6f}, {float(start_row['lon']):.6f})")
    print(f"GPS end:            ({float(end_row['lat']):.6f}, {float(end_row['lon']):.6f})")
    print(f"Estimated rows:     {len(est_rows)}")
    print()
    print(f"Raw DR:             {math.hypot(Sx, Sy):.1f} m  @ {raw_az:.1f}° az")
    print(f"Required:           {math.hypot(dx, dy):.1f} m  @ {req_az:.1f}° az")
    print()
    print(f"── Free solution ──────────────────────────────────")
    print(f"  Speed factor k:   {k_free:.4f}×")
    print(f"  Heading offset θ: {theta_free_deg:+.2f}°  "
          f"({'compass reads low' if theta_free_deg > 0 else 'compass reads high'})")
    print(f"  Closure error:    0.0000 m  (exact by construction)")
    if constrained:
        print()
        print(f"── Constrained solution ───────────────────────────")
        print(f"  Speed factor k:   {k_used:.4f}×"
              + (f"  [clipped from {k_free:.4f}]" if k_used != k_free else ""))
        print(f"  Heading offset θ: {theta_used_deg:+.2f}°"
              + (f"  [clipped from {theta_free_deg:+.2f}°]" if theta_used != theta_free else ""))
        print(f"  Closure error:    {residual:.2f} m")
    print()
    print(f"Output:             {output_file}")


if __name__ == "__main__":
    main()
