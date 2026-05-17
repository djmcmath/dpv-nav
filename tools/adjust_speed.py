#!/usr/bin/env python3
"""
Recalculate DPV-nav log positions with a corrected speed factor.

Reads a log CSV, multiplies speed_ms by SPEED_FACTOR, and re-integrates
dead-reckoning to produce adjusted_pos_x_m / adjusted_pos_y_m /
adjusted_lat / adjusted_lon columns.

GPS-truth rows (pos_src == 'G') snap the adjusted track to the GPS fix,
exactly as the nav device does at runtime.

Usage:
    python tools/adjust_speed.py tools/003.csv [speed_factor]
    # speed_factor defaults to 7.0
"""

import csv
import math
import os
import sys

# Must match DEFAULT_BASELINE_LAT / DEFAULT_BASELINE_LON in src/config.h
BASELINE_LAT = 47.5889
BASELINE_LON = -122.284

M_PER_DEG_LAT = 111320.0
DEFAULT_SPEED_FACTOR = 7.0


def m_per_deg_lon(lat_deg: float) -> float:
    return M_PER_DEG_LAT * math.cos(math.radians(lat_deg))


def latlon_to_xy(lat: float, lon: float, base_lat: float, base_lon: float):
    x = (lon - base_lon) * m_per_deg_lon(base_lat)
    y = (lat - base_lat) * M_PER_DEG_LAT
    return x, y


def xy_to_latlon(x: float, y: float, base_lat: float, base_lon: float):
    lat = base_lat + y / M_PER_DEG_LAT
    lon = base_lon + x / m_per_deg_lon(base_lat)
    return lat, lon


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <logfile.csv> [speed_factor]")
        sys.exit(1)

    input_file = sys.argv[1]
    speed_factor = float(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_SPEED_FACTOR

    base, ext = os.path.splitext(input_file)
    output_file = f"{base}_adjusted{ext}"

    with open(input_file, newline="") as f:
        reader = csv.DictReader(f)
        fieldnames = reader.fieldnames
        rows = list(reader)

    if not rows:
        print("No data rows found.")
        sys.exit(1)

    new_fields = ["adjusted_speed_ms", "adjusted_pos_x_m", "adjusted_pos_y_m",
                  "adjusted_lat", "adjusted_lon"]
    out_fieldnames = list(fieldnames) + new_fields

    adj_x = None
    adj_y = None
    prev_ts_ms = None

    output_rows = []

    for row in rows:
        ts_ms = int(row["timestamp_ms"])
        heading_deg = float(row["heading_deg"])
        speed_ms = float(row["speed_ms"])
        pos_src = row["pos_src"]
        orig_lat = float(row["lat"])
        orig_lon = float(row["lon"])

        adj_speed = speed_ms * speed_factor

        if pos_src == "G":
            # GPS truth: snap adjusted track to GPS fix (ground truth, speed-independent)
            adj_x, adj_y = latlon_to_xy(orig_lat, orig_lon, BASELINE_LAT, BASELINE_LON)
        elif adj_x is None:
            # First row with no GPS fix yet: seed from logged position
            adj_x = float(row["pos_x_m"])
            adj_y = float(row["pos_y_m"])
        else:
            # Dead-reckoning step with adjusted speed
            dt = (ts_ms - prev_ts_ms) / 1000.0
            heading_rad = math.radians(heading_deg)
            adj_x += adj_speed * math.sin(heading_rad) * dt
            adj_y += adj_speed * math.cos(heading_rad) * dt

        prev_ts_ms = ts_ms

        adj_lat, adj_lon = xy_to_latlon(adj_x, adj_y, BASELINE_LAT, BASELINE_LON)

        row["adjusted_speed_ms"] = f"{adj_speed:.4f}"
        row["adjusted_pos_x_m"] = f"{adj_x:.2f}"
        row["adjusted_pos_y_m"] = f"{adj_y:.2f}"
        row["adjusted_lat"] = f"{adj_lat:.8f}"
        row["adjusted_lon"] = f"{adj_lon:.8f}"
        output_rows.append(row)

    with open(output_file, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=out_fieldnames)
        writer.writeheader()
        writer.writerows(output_rows)

    start = output_rows[0]
    end = output_rows[-1]
    dx = float(end["adjusted_pos_x_m"]) - float(start["adjusted_pos_x_m"])
    dy = float(end["adjusted_pos_y_m"]) - float(start["adjusted_pos_y_m"])
    total_dist = math.hypot(dx, dy)

    print(f"Speed factor:   {speed_factor}x")
    print(f"Rows processed: {len(output_rows)}")
    print(f"Output:         {output_file}")
    print(f"Net displacement (adjusted): {total_dist:.1f} m "
          f"({dx:.1f} E, {dy:.1f} N)")


if __name__ == "__main__":
    main()
