#!/usr/bin/env python3
"""
Tests for assemble_track.py — run splitting, wall-clock rebasing, segment
assignment, and idle trimming.

Run:  python3 tools/test_assemble_track.py
"""

import sys
from datetime import datetime, timedelta

import assemble_track as at


PASSED = 0
FAILED = 0


def check(name: str, ok: bool, detail: str = "") -> None:
    global PASSED, FAILED
    if ok:
        PASSED += 1
        print(f"  PASS  {name}")
    else:
        FAILED += 1
        print(f"  FAIL  {name}" + (f"\n          {detail}" if detail else ""))


T0 = datetime(2026, 7, 5, 9, 0, 0)


def row(ms: int, *, at_time: datetime = None, src: str = "E", speed: float = 1.0):
    """One log row.  `at_time` omitted means the row carries no GPS time."""
    return {
        "timestamp_ms": str(ms),
        "local_time": at_time.isoformat() if at_time else "",
        "heading_deg": "90",
        "speed_ms": str(speed),
        "lat": "47.60000000",
        "lon": "-122.30000000",
        "pos_src": src,
    }


def run_of(n: int, *, start_ms: int, start_time: datetime, src: str = "E", speed: float = 1.0):
    """n rows at 1 Hz."""
    return [row(start_ms + i * 1000, at_time=start_time + timedelta(seconds=i),
                src=src, speed=speed) for i in range(n)]


# ── 1. A single corrupt row must not be integrated through ────────────────────

print("\nRun splitting: a corrupt row is quarantined, not integrated across")
rows = run_of(50, start_ms=100_000, start_time=T0)
corrupt = row(17, at_time=T0 + timedelta(seconds=50))   # millis is garbage
rows = rows[:50] + [corrupt] + run_of(50, start_ms=151_000,
                                      start_time=T0 + timedelta(seconds=51))

runs = at.split_runs(rows, at.DEFAULT_MAX_DT_S)
check("splits into three runs around the bad row", len(runs) == 3,
      f"got {len(runs)}: {[len(r) for r in runs]}")
check("the bad row is alone in its run", len(runs[1]) == 1, f"got {len(runs[1])}")


# ── 2. …but the runs either side are one segment, because wall clock is fine ──
#
# This is the bug that cost an entire dive's correction.  `timestamp_ms` broke,
# so run-splitting fires — but no *time* was lost: the rows either side are one
# second apart on the wall clock.  The dead reckoning chains straight through.
# Treating that as a discontinuity would split one segment anchored at both ends
# into a start-anchored half and an end-anchored half, neither of which
# constrains the solve, and the dive would silently lose its correction.

print("\nSegments: a broken timestamp is not a broken dive")
good = []
for r in (runs[0], runs[2]):
    start, end = at.wall_clock(r)
    good.append({"rows": r, "start": start, "end": end, "profile": {"src": "x"}})

at.assign_segments(good, at.DEFAULT_MAX_DT_S)
check("wall clock either side of the bad row is contiguous",
      (good[1]["start"] - good[0]["end"]).total_seconds() <= at.DEFAULT_MAX_DT_S,
      f"gap = {(good[1]['start'] - good[0]['end']).total_seconds():.1f}s")
check("both runs land in the SAME segment",
      good[0]["seg"] == good[1]["seg"] == 0,
      f"got segs {[g['seg'] for g in good]}")

# A genuine gap, however, must still earn its own segment.
far = [
    {"rows": runs[0], "start": T0, "end": T0 + timedelta(seconds=50),
     "profile": {"src": "a"}},
    {"rows": runs[2], "start": T0 + timedelta(minutes=8),
     "end": T0 + timedelta(minutes=9), "profile": {"src": "b"}},
]
at.assign_segments(far, at.DEFAULT_MAX_DT_S)
check("a real 8-minute gap does start a new segment",
      far[0]["seg"] == 0 and far[1]["seg"] == 1,
      f"got segs {[f['seg'] for f in far]}")


# ── 3. Wall clock from a single fix anywhere in the run ───────────────────────

print("\nTimebase: one fix anywhere recovers the clock for the whole run")
late = [row(1000 + i * 1000) for i in range(10)]          # no local_time yet
late[7]["local_time"] = (T0 + timedelta(seconds=7)).isoformat()
start, end = at.wall_clock(late)
check("start is back-derived from a fix 7 rows in",
      start is not None and abs((start - T0).total_seconds()) < 0.01,
      f"got {start}")
check("end is derived too",
      end is not None and abs((end - (T0 + timedelta(seconds=9))).total_seconds()) < 0.01,
      f"got {end}")

blind = [row(1000 + i * 1000) for i in range(10)]          # never saw GPS
check("a run with no fix at all has no clock", at.wall_clock(blind) == (None, None))


# ── 4. Trimming touches anchors only ──────────────────────────────────────────

print("\nTrimming: collapse idle GPS, never touch dead reckoning")
idle = run_of(300, start_ms=0, start_time=T0, src="G", speed=0.0)
trimmed = at.trim_idle_anchors(idle, at.DEFAULT_IDLE_SPEED, at.DEFAULT_MIN_IDLE_S)
check("300 stationary GPS rows collapse to their endpoints", len(trimmed) == 2,
      f"got {len(trimmed)}")
check("the first row is kept", trimmed[0] is idle[0])
check("the last row is kept — it anchors the DR that follows", trimmed[-1] is idle[-1])

parked = run_of(300, start_ms=0, start_time=T0, src="E", speed=0.0)
check("300 stationary DR rows are left completely alone",
      len(at.trim_idle_anchors(parked, at.DEFAULT_IDLE_SPEED, at.DEFAULT_MIN_IDLE_S)) == 300)

moving = run_of(300, start_ms=0, start_time=T0, src="G", speed=2.0)
check("a moving GPS run is not trimmed",
      len(at.trim_idle_anchors(moving, at.DEFAULT_IDLE_SPEED, at.DEFAULT_MIN_IDLE_S)) == 300)


# ── 5. Classification ─────────────────────────────────────────────────────────

print("\nClassification")

def classify(rows):
    return at.profile(rows, "x", at.DEFAULT_MIN_ROWS, at.DEFAULT_MIN_PATH_M)["classification"]

check("no fix anywhere -> unplaceable",
      classify(run_of(100, start_ms=0, start_time=T0, src="E")) == "unplaceable")
check("a fix but going nowhere -> idle (this is an anchor, not junk)",
      classify(run_of(100, start_ms=0, start_time=T0, src="G", speed=0.0)) == "idle")
check("too short -> trivial",
      classify(run_of(3, start_ms=0, start_time=T0, src="G")) == "trivial")

moves_from_dr = run_of(100, start_ms=0, start_time=T0, src="E", speed=2.0)
moves_from_dr[-1]["pos_src"] = "G"       # a fix arrives at the end
check("moves, opens on DR, has a later fix -> unanchored",
      classify(moves_from_dr) == "unanchored")

moves_from_fix = run_of(100, start_ms=0, start_time=T0, src="E", speed=2.0)
moves_from_fix[0]["pos_src"] = "G"
check("moves, opens on a fix -> good", classify(moves_from_fix) == "good")


print(f"\n{PASSED} passed, {FAILED} failed")
sys.exit(1 if FAILED else 0)
