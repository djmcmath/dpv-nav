#!/usr/bin/env python3
"""
Tests for correct_track.py — timebase, discontinuity handling, and one-sided
anchoring.

Run:  python3 tools/test_correct_track.py

These use synthetic tracks whose right answer is known by construction: a diver
heading due east at a constant 1 m/s, with GPS fixes that agree exactly with the
dead reckoning.  The correct solution is therefore k = 1.0, θ = 0.0, and any
departure from that is the tool inventing something.
"""

import math
import sys

import correct_track as ct


PASSED = 0
FAILED = 0

# Coordinates in these logs are written with 8 decimal places, which at this
# latitude is about 0.75 mm of longitude.  Fixes therefore round-trip through
# lat/lon with sub-millimetre error, and a track built from them cannot close
# any tighter than that.  Assertions are made at 1 mm; anything larger is the
# tool, not the file format.
TOL_M = 1e-3
TOL_K = 1e-5


def check(name: str, ok: bool, detail: str = "") -> None:
    global PASSED, FAILED
    if ok:
        PASSED += 1
        print(f"  PASS  {name}")
    else:
        FAILED += 1
        print(f"  FAIL  {name}" + (f"\n          {detail}" if detail else ""))


def east_of(lat: float, lon: float, metres: float):
    """A point `metres` due east of (lat, lon)."""
    x, y = ct.latlon_to_xy(lat, lon)
    return ct.xy_to_latlon(x + metres, y)


LAT0, LON0 = 47.60, -122.30


def gps_row(t_s: float, lat: float, lon: float) -> dict:
    return {"timestamp_ms": str(int(t_s * 1000)), "heading_deg": "90",
            "speed_ms": "0", "lat": f"{lat:.8f}", "lon": f"{lon:.8f}",
            "pos_src": "G"}


def dr_row(t_s: float, lat: float, lon: float, speed: float = 1.0) -> dict:
    return {"timestamp_ms": str(int(t_s * 1000)), "heading_deg": "90",
            "speed_ms": f"{speed}", "lat": f"{lat:.8f}", "lon": f"{lon:.8f}",
            "pos_src": "E"}


def straight_east_track(n: int = 100, t0: float = 0.0):
    """
    G at origin, then n DR rows heading east at 1 m/s (one per second), then a
    G fix exactly n metres east.  Dead reckoning agrees with GPS exactly.
    """
    rows = [gps_row(t0, LAT0, LON0)]
    for i in range(1, n + 1):
        lat, lon = east_of(LAT0, LON0, i)
        rows.append(dr_row(t0 + i, lat, lon))
    end_lat, end_lon = east_of(LAT0, LON0, n)
    rows.append(gps_row(t0 + n + 1, end_lat, end_lon))
    return rows


def prepare(rows: list[dict], max_dt: float = ct.DEFAULT_MAX_DT_S):
    ct.assign_runs(rows, max_dt)
    blocks = ct.parse_blocks(rows)
    return ct.build_segments(blocks)


# ── 1. The baseline case still solves exactly ─────────────────────────────────

print("\nBaseline: G … E … G, DR agrees with GPS")
segs = prepare(straight_east_track())
check("one segment, anchored at both ends", len(segs) == 1 and segs[0]["anchored"] == "both")

sol = ct.solve_k_theta_multi([ct.compute_dr_vectors(s) for s in segs])
check("solves k = 1.0", sol is not None and abs(sol[0] - 1.0) < TOL_K,
      f"got k = {sol[0] if sol else None}")
check("solves θ = 0.0", sol is not None and abs(math.degrees(sol[1])) < 1e-3,
      f"got θ = {math.degrees(sol[1]) if sol else None}°")

_, gap, _ = ct.place_segment(segs[0], 1.0, 0.0)
check("closes on the end fix", gap < TOL_M, f"gap = {gap:.6f} m")


# ── 2. t_s and timestamp_ms must agree ────────────────────────────────────────

print("\nTimebase: t_s must give the same answer as timestamp_ms")
rows_ms = straight_east_track()
rows_ts = [dict(r, t_s=str(int(r["timestamp_ms"]) / 1000.0), seg="0")
           for r in straight_east_track()]

pos_ms, _, _ = ct.place_segment(prepare(rows_ms)[0], 1.0, 0.0)
pos_ts, _, _ = ct.place_segment(prepare(rows_ts)[0], 1.0, 0.0)
worst = max(math.hypot(a[0] - b[0], a[1] - b[1]) for a, b in zip(pos_ms, pos_ts))
check("identical positions from either timebase", worst < 1e-9, f"worst = {worst:g} m")


# ── 3. A logging gap must not fabricate a leg ─────────────────────────────────
#
# The diver swims east for 50 s, logging stops for 600 s, then resumes and they
# swim east for another 50 s.  We have no data for the gap.  The integrator must
# not multiply the resumed speed (1 m/s) by the 600 s gap and invent 600 m of
# eastward travel that was never observed.

print("\nDiscontinuity: a 600 s logging gap must not become 600 m of travel")
gap_rows = [gps_row(0, LAT0, LON0)]
for i in range(1, 51):
    lat, lon = east_of(LAT0, LON0, i)
    gap_rows.append(dr_row(i, lat, lon))
# ...600 s of nothing...
for i in range(1, 51):
    lat, lon = east_of(LAT0, LON0, 50 + i)
    gap_rows.append(dr_row(650 + i, lat, lon))
end_lat, end_lon = east_of(LAT0, LON0, 100)
gap_rows.append(gps_row(701, end_lat, end_lon))

gap_segs = prepare(gap_rows)
check("gap splits the DR into two segments", len(gap_segs) == 2,
      f"got {len(gap_segs)}")
check("first segment is start-anchored", gap_segs[0]["anchored"] == "start",
      f"got {gap_segs[0]['anchored']}")
check("second segment is end-anchored", gap_segs[1]["anchored"] == "end",
      f"got {gap_segs[1]['anchored']}")

# Total DR distance actually integrated across both segments.
integrated = 0.0
for s in gap_segs:
    Sx, Sy = ct.integrate(s)[-1]
    integrated += math.hypot(Sx, Sy)
check("integrates ~100 m of DR, not ~700 m", abs(integrated - 99.0) < 2.0,
      f"integrated {integrated:.1f} m")

# What the old, gap-blind integrator would have done: one E-block, one dt of
# 600 s multiplied by the speed the diver resumed at.
naive = 0.0
prev_t = 0.0
for r in gap_rows:
    if r["pos_src"] != "E":
        continue
    t = float(r["timestamp_ms"]) / 1000.0
    naive += float(r["speed_ms"]) * (t - prev_t)
    prev_t = t
check("gap-blind integration would have fabricated ~600 m", naive > 600.0,
      f"naive = {naive:.1f} m (this is the bug being prevented)")


# ── 4. An end-anchored segment is placed by its closing fix ───────────────────
#
# This is the "I restarted the log partway out" dive: the track opens on DR with
# no fix behind it, and the only truth available is the fix at the end.

print("\nEnd-anchored: a track that opens on DR is slid into place by its closing fix")
tail_rows = straight_east_track()[1:]      # drop the leading GPS block
tail_segs = prepare(tail_rows)
check("segment is end-anchored", len(tail_segs) == 1 and tail_segs[0]["anchored"] == "end",
      f"got {[s['anchored'] for s in tail_segs]}")

positions, gap, _ = ct.place_segment(tail_segs[0], 1.0, 0.0)
end_x, end_y = ct.latlon_to_xy(float(tail_segs[0]["end_row"]["lat"]),
                               float(tail_segs[0]["end_row"]["lon"]))
landed = math.hypot(positions[-1][0] - end_x, positions[-1][1] - end_y)
check("last DR row lands exactly on the closing fix", landed < TOL_M,
      f"off by {landed:.6f} m")
check("reported closure gap is zero", gap < TOL_M, f"gap = {gap:.6f} m")

# The shape must survive the translation: still a straight 99 m run due east.
span = math.hypot(positions[-1][0] - positions[0][0], positions[-1][1] - positions[0][1])
check("shape preserved (99 m due east)", abs(span - 99.0) < TOL_M, f"span = {span:.4f} m")
drift = max(abs(p[1] - positions[0][1]) for p in positions)
check("no north/south drift introduced", drift < TOL_M, f"drift = {drift:g} m")


# ── 5. A waypoint snap anchors exactly like a GPS fix ─────────────────────────
#
# The diver snapped to a known waypoint mid-dive, so the unit jumped to that
# position in real time — the track demonstrably passed through it.  A lone W row
# is therefore a single-row anchor block: it closes the DR before it and opens
# the DR after it, splitting one E-block into two constrained segments.

print("\nWaypoint snap: a lone W row anchors the DR on both sides of itself")
wp_rows = [gps_row(0, LAT0, LON0)]
for i in range(1, 51):
    lat, lon = east_of(LAT0, LON0, i)
    wp_rows.append(dr_row(i, lat, lon))
wp_lat, wp_lon = east_of(LAT0, LON0, 50)
wp_rows.append(dict(gps_row(51, wp_lat, wp_lon), pos_src="W"))   # the snap
for i in range(1, 51):
    lat, lon = east_of(LAT0, LON0, 50 + i)
    wp_rows.append(dr_row(51 + i, lat, lon))
end_lat, end_lon = east_of(LAT0, LON0, 100)
wp_rows.append(gps_row(102, end_lat, end_lon))

wp_segs = prepare(wp_rows)
check("W splits the DR into two segments", len(wp_segs) == 2, f"got {len(wp_segs)}")
check("both segments are fully anchored",
      all(s["anchored"] == "both" for s in wp_segs),
      f"got {[s['anchored'] for s in wp_segs]}")
check("the W row is the closing anchor of segment 1",
      wp_segs[0]["end_row"]["pos_src"] == "W")
check("the W row is the opening anchor of segment 2",
      wp_segs[1]["start_row"]["pos_src"] == "W")

wp_sol = ct.solve_k_theta_multi([ct.compute_dr_vectors(s) for s in wp_segs])
check("still solves k = 1.0 across the snap", wp_sol is not None and abs(wp_sol[0] - 1.0) < TOL_K,
      f"got k = {wp_sol[0] if wp_sol else None}")
for s in wp_segs:
    _, wgap, _ = ct.place_segment(s, 1.0, 0.0)
    check(f"segment {s['num']} closes on its anchor", wgap < TOL_M, f"gap = {wgap:.6f} m")

# A W run adjacent to a G run is one anchor block, not two.
adj_rows = [gps_row(0, LAT0, LON0), dict(gps_row(1, LAT0, LON0), pos_src="W")]
for i in range(1, 51):
    lat, lon = east_of(LAT0, LON0, i)
    adj_rows.append(dr_row(1 + i, lat, lon))
adj_rows.append(gps_row(52, *east_of(LAT0, LON0, 50)))
ct.assign_runs(adj_rows, ct.DEFAULT_MAX_DT_S)
adj_blocks = ct.parse_blocks(adj_rows)
check("adjacent G and W rows form a single anchor block",
      len(adj_blocks) == 3 and adj_blocks[0]["type"] == "A" and len(adj_blocks[0]["rows"]) == 2,
      f"got {[(b['type'], len(b['rows'])) for b in adj_blocks]}")


# ── 6. Auto mode picks what the geometry can actually support ─────────────────
#
# The mode is not a preference.  Each both-anchored segment gives 2 equations;
# the unknowns are k, θ, and (if the water moved) the current.  Auto's job is to
# refuse to report a correction the data cannot support.

print("\nAuto mode: pick the correction the geometry can support")


def leg(t0: float, x0: float, x1: float, n: int = 60):
    """DR rows running from x0 to x1 metres east of the origin, 1 row/second."""
    rows = []
    for i in range(1, n + 1):
        x = x0 + (x1 - x0) * i / n
        lat, lon = east_of(LAT0, LON0, x)
        rows.append(dr_row(t0 + i, lat, lon, speed=abs(x1 - x0) / n))
        rows[-1]["heading_deg"] = "90" if x1 > x0 else "270"
    return rows


def auto_for(rows):
    segs = prepare(rows)
    solvable = [s for s in segs if s["anchored"] == "both"]
    n_runs = len({r["_run"] for r in rows})
    return ct.choose_mode(segs, solvable, n_runs)[0]


# One straight leg out to a fix 300 m away: net displacement is the whole path.
# Two equations, two unknowns → joint.
straight = ([gps_row(0, LAT0, LON0)] + leg(0, 0, 300)
            + [gps_row(61, *east_of(LAT0, LON0, 300))])
check("one informative leg -> joint", auto_for(straight) == "joint",
      f"got {auto_for(straight)}")

# Out to the wreck and straight back to the beach, with NO fix at the wreck.
# One segment, and it ends where it began: net displacement ~0 however far the
# diver swam.  Nothing is identifiable — the track can be drawn but not measured.
loop = ([gps_row(0, LAT0, LON0)] + leg(0, 0, 300) + leg(60, 300, 0)
        + [gps_row(121, LAT0, LON0)])
check("an out-and-back with no mid-dive fix -> proportional (nothing identifiable)",
      auto_for(loop) == "proportional", f"got {auto_for(loop)}")

# The same dive, but the diver snapped a waypoint at the wreck.  That one row
# splits the useless loop into two informative legs: 4 equations, 4 unknowns,
# so k, θ AND the current all become solvable.  This is the whole argument for
# snapping a waypoint at the far end of a dive.
with_snap = ([gps_row(0, LAT0, LON0)] + leg(0, 0, 300)
             + [dict(gps_row(61, *east_of(LAT0, LON0, 300)), pos_src="W")]
             + leg(61, 300, 0) + [gps_row(122, LAT0, LON0)])
check("...but snap a waypoint at the wreck and it becomes reciprocal",
      auto_for(with_snap) == "reciprocal", f"got {auto_for(with_snap)}")

# Nothing anchored at both ends: place by the single fix, correct nothing.
tail_only = leg(0, 0, 300) + [gps_row(61, *east_of(LAT0, LON0, 300))]
check("no both-anchored segment -> no correction (joint applies the identity)",
      auto_for(tail_only) == "joint", f"got {auto_for(tail_only)}")


# ── 7. Nothing to solve against degrades, it does not crash ───────────────────

print("\nUnconstrained: a dive with no both-anchored segment must still produce a track")
check("solver returns None rather than exiting", ct.solve_k_theta_multi([]) is None)
check("stationary anchored segment also returns None",
      ct.solve_k_theta_multi([(0.0, 0.0, 5.0, 5.0)]) is None)


print(f"\n{PASSED} passed, {FAILED} failed")
sys.exit(1 if FAILED else 0)
