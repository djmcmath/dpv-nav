#!/usr/bin/env python3
"""
Assemble raw DPV-nav log files into dives, ready for correct_track.py.

A dive is not a file.  The unit gets power cycled on the boat, logging is
started late, logging is stopped early, and one dive routinely lands on disk as
two or three files.  Meanwhile the *device* uploads everything it is holding,
indiscriminately — including logs of the unit sitting on the beach with a good
GPS fix and going nowhere.  Sorting that out is this tool's job.

Given a pile of uploaded logs, it:

  1. Splits each file into *runs* — stretches with no break in the data.  A
     well-behaved log is one run; a file that was hand-spliced or that skipped
     is several.
  2. Puts every run on absolute wall-clock time, taken from `local_time`.
  3. Discards what cannot be used, and says why (see Classification).
  4. Groups the survivors into dives by contiguous wall-clock time.
  5. Trims long stationary stretches of GPS.
  6. Writes one merged CSV per dive, carrying the two columns correct_track.py
     needs to splice safely: `t_s` and `seg`.


Timebase
────────
`timestamp_ms` is millis-since-boot.  It restarts at zero on every power cycle,
so it cannot order two files, let alone span them.  `local_time` comes from GPS
and is the only wall clock available.

A run needs only *one* fix anywhere in it to be placed on the timeline, not a
fix at the start: millis is monotonic within a run, so a single `local_time`
anchors the whole thing.  Absolute time is derived as

    t_abs(row) = local_time(ref) + (timestamp_ms(row) − timestamp_ms(ref)) / 1000

which keeps the millisecond precision of `timestamp_ms` for dt — `local_time`
only has one-second resolution, and integrating DR against rounded one-second
steps would throw away the real sample interval.

A run with no fix *anywhere* has no clock and no position.  Its shape is real
but it cannot be placed in space or time, so it is dropped as `unplaceable`
rather than being given a guessed timestamp.


Segments
────────
`seg` increments at every discontinuity: each splice between runs in a dive, and
any interval the unit was not logging.  correct_track.py never integrates dead
reckoning across one, because the diver kept moving through a gap we have no
data for — and integrating through it would multiply the speed they resumed at
by a gap-sized dt and invent a leg that never happened.

Trimming does *not* create a segment boundary.  Only interior rows of a
stationary run of anchors are dropped, and anchors are never integrated — they
are only read as fixed positions — so collapsing them is free.  The first and
last row of every anchor run are always kept, which is what preserves the anchor
that opens the following DR segment.  Dead-reckoned rows are never trimmed, at
any speed: dropping those is exactly what re-introduces the fabricated leg.


Classification
──────────────
  good         Moves, and opens on a known position.  The easy case.
  unanchored   Moves, but opens on dead reckoning — logging was started after
               the unit had already left its last fix.  Perfectly usable:
               correct_track.py slides it into place by a later fix.
  idle         Has a fix but never goes anywhere — the unit sitting on the boat.
               NOT junk.  This is a GPS anchor block, and merging it ahead of an
               unanchored run is what gives that run something to close against.
  trivial      Too short to be anything.
  unplaceable  No fix anywhere: no clock, no position.  Dropped.

Usage:
    python tools/assemble_track.py LOG [LOG ...] [-o OUTDIR]
                                   [--gap-minutes 30] [--max-dt 30]
                                   [--idle-speed 0.2] [--min-idle 60]
                                   [--json manifest.json]

Output: OUTDIR/dive_<YYYYMMDD>_<HHMMSS>.csv, one per dive.
"""

import argparse
import csv
import json
import math
import os
import sys
from datetime import datetime, timedelta

# Position sources that are known rather than inferred.  Kept in step with
# correct_track.ANCHOR_SRCS.
ANCHOR_SRCS = {"G", "W"}

M_PER_DEG_LAT = 111320.0

DEFAULT_GAP_MINUTES = 30.0    # wall-clock gap that separates one dive from the next
DEFAULT_MAX_DT_S    = 30.0    # time step above which we assume logging stopped
DEFAULT_IDLE_SPEED  = 0.2     # m/s below which the unit is considered stationary
DEFAULT_MIN_IDLE_S  = 60.0    # stationary stretch worth collapsing
DEFAULT_MIN_ROWS    = 10      # fewer rows than this is not a dive
DEFAULT_MIN_PATH_M  = 20.0    # travelled less than this and it never went anywhere


# ── Loading and run-splitting ─────────────────────────────────────────────────

def load_rows(path: str) -> tuple[list[dict], list[str]]:
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        return list(reader), list(reader.fieldnames or [])


def split_runs(rows: list[dict], max_dt: float) -> list[list[dict]]:
    """
    Split one file's rows into runs with no break in the data.

    A well-behaved log is a single run.  A file whose millis goes backwards, or
    jumps by more than max_dt, was spliced by hand or stopped logging — and the
    interval it covers is one we have no data for.  Those are real
    discontinuities and must survive into the output as segment boundaries
    rather than being quietly integrated through.
    """
    if not rows:
        return []
    runs = [[rows[0]]]
    prev = int(rows[0]["timestamp_ms"])
    for row in rows[1:]:
        ts = int(row["timestamp_ms"])
        dt = (ts - prev) / 1000.0
        if dt < 0 or dt > max_dt:
            runs.append([])
        runs[-1].append(row)
        prev = ts
    return runs


def parse_local_time(value: str):
    if not value:
        return None
    try:
        return datetime.fromisoformat(value.strip())
    except ValueError:
        return None


def wall_clock(run: list[dict]):
    """
    Put a run on absolute time using the first row that carries a `local_time`.

    Returns (start_dt, end_dt), or (None, None) when the run never saw GPS and
    so has no clock at all.
    """
    ref = None
    for row in run:
        dt = parse_local_time(row.get("local_time", ""))
        if dt is not None:
            ref = (dt, int(row["timestamp_ms"]))
            break
    if ref is None:
        return None, None

    ref_dt, ref_ms = ref
    first_ms = int(run[0]["timestamp_ms"])
    last_ms  = int(run[-1]["timestamp_ms"])
    start = ref_dt + timedelta(seconds=(first_ms - ref_ms) / 1000.0)
    end   = ref_dt + timedelta(seconds=(last_ms  - ref_ms) / 1000.0)
    return start, end


# ── Profiling ─────────────────────────────────────────────────────────────────

def profile(run: list[dict], src: str, min_rows: int, min_path_m: float) -> dict:
    start, end = wall_clock(run)

    path_m = 0.0
    prev_ms = int(run[0]["timestamp_ms"])
    max_speed = 0.0
    for row in run:
        ms  = int(row["timestamp_ms"])
        dt  = (ms - prev_ms) / 1000.0
        spd = float(row["speed_ms"] or 0.0)
        path_m += spd * dt
        max_speed = max(max_speed, spd)
        prev_ms = ms

    anchors = sum(1 for r in run if r["pos_src"] in ANCHOR_SRCS)
    duration = (int(run[-1]["timestamp_ms"]) - int(run[0]["timestamp_ms"])) / 1000.0

    p = {
        "src":              src,
        "rows":             len(run),
        "duration_s":       duration,
        "started_at":       start.isoformat() if start else None,
        "ended_at":         end.isoformat() if end else None,
        "anchor_rows":      anchors,
        "has_fix":          anchors > 0,
        "starts_with_fix":  run[0]["pos_src"] in ANCHOR_SRCS,
        "path_m":           path_m,
        "max_speed_ms":     max_speed,
    }

    if not p["has_fix"]:
        p["classification"] = "unplaceable"
    elif len(run) < min_rows or duration < 10.0:
        p["classification"] = "trivial"
    elif path_m < min_path_m:
        p["classification"] = "idle"
    elif not p["starts_with_fix"]:
        p["classification"] = "unanchored"
    else:
        p["classification"] = "good"

    return p


# ── Trimming ──────────────────────────────────────────────────────────────────

def trim_idle_anchors(run: list[dict], idle_speed: float, min_idle_s: float) -> list[dict]:
    """
    Collapse long stationary stretches of anchor rows down to their first and
    last row.  There is no value in plotting half an hour of the unit sitting in
    one spot, and no cost to dropping it: anchors are read as positions, never
    integrated, and keeping the endpoints preserves both the run's time extent
    and the anchor that opens the DR segment after it.

    Dead-reckoned rows are never touched, however slowly the diver was moving.
    """
    keep = [True] * len(run)
    i = 0
    while i < len(run):
        if run[i]["pos_src"] not in ANCHOR_SRCS or float(run[i]["speed_ms"] or 0.0) >= idle_speed:
            i += 1
            continue

        j = i
        while (j + 1 < len(run)
               and run[j + 1]["pos_src"] in ANCHOR_SRCS
               and float(run[j + 1]["speed_ms"] or 0.0) < idle_speed):
            j += 1

        span = (int(run[j]["timestamp_ms"]) - int(run[i]["timestamp_ms"])) / 1000.0
        if span >= min_idle_s and j - i >= 2:
            for m in range(i + 1, j):
                keep[m] = False
        i = j + 1

    return [row for row, k in zip(run, keep) if k]


# ── Grouping ──────────────────────────────────────────────────────────────────

def group_into_dives(runs: list[dict], gap_minutes: float) -> list[list[dict]]:
    """
    Runs whose wall-clock ranges are separated by less than `gap_minutes` are the
    same dive.  Runs arrive sorted by start time.
    """
    if not runs:
        return []
    dives = [[runs[0]]]
    for r in runs[1:]:
        prev_end = dives[-1][-1]["end"]
        if (r["start"] - prev_end) > timedelta(minutes=gap_minutes):
            dives.append([])
        dives[-1].append(r)
    return dives


# ── Output ────────────────────────────────────────────────────────────────────

def assign_segments(dive: list[dict], max_dt: float) -> None:
    """
    Number the segments of a dive by *wall-clock* contiguity, not by run.

    A run boundary means only that `timestamp_ms` broke — a power cycle, or a
    single corrupt row whose millis is garbage.  Neither is necessarily a break
    in the *data*.  If the wall clock either side of the boundary is continuous,
    no time was lost and no distance went unobserved, so the dead reckoning
    still chains across it and the two runs are one segment.

    This matters more than it sounds.  Splitting a continuous DR chain in two
    turns one segment anchored at both ends — which constrains the speed factor
    and the heading offset — into a start-anchored half and an end-anchored half,
    neither of which constrains anything.  A single bad row would otherwise cost
    us the entire correction for the dive.

    Only a genuine gap, longer than max_dt, earns a new segment.
    """
    seg = 0
    dive[0]["seg"] = seg
    for prev, run in zip(dive, dive[1:]):
        if (run["start"] - prev["end"]).total_seconds() > max_dt:
            seg += 1
        run["seg"] = seg


def write_dive(dive: list[dict], fieldnames: list[str], outdir: str) -> tuple[str, int]:
    """
    Write one dive as a single CSV: every run concatenated in time order, on one
    `t_s` timeline measured from the start of the dive, with `seg` marking each
    real break in the data.

    `t_s` is rebased per run from that run's own `local_time`, because millis is
    only meaningful within a single boot.  That is what lets two runs from
    different power cycles sit on one continuous timeline.
    """
    dive_start = dive[0]["start"]
    stamp = dive_start.strftime("%Y%m%d_%H%M%S")
    path = os.path.join(outdir, f"dive_{stamp}.csv")

    extra = [f for f in ("t_s", "seg", "src_file") if f not in fieldnames]
    out_fields = fieldnames + extra

    n = 0
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=out_fields, extrasaction="ignore")
        writer.writeheader()
        for run in dive:
            ref_ms = int(run["rows"][0]["timestamp_ms"])
            offset = (run["start"] - dive_start).total_seconds()
            for row in run["rows"]:
                t_s = offset + (int(row["timestamp_ms"]) - ref_ms) / 1000.0
                out = dict(row)
                out["t_s"] = f"{t_s:.3f}"
                out["seg"] = str(run["seg"])
                out["src_file"] = run["profile"]["src"]
                writer.writerow(out)
                n += 1
    return path, n


CLASS_NOTE = {
    "good":        "moves, opens on a fix",
    "unanchored":  "moves, opens on DR — will be placed by a later fix",
    "idle":        "never moves — kept as a GPS anchor block",
    "trivial":     "too short to be anything",
    "unplaceable": "no fix anywhere — no clock, no position",
}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", metavar="LOG")
    ap.add_argument("-o", "--outdir", default=".", help="Where to write assembled dives")
    ap.add_argument("--gap-minutes", type=float, default=DEFAULT_GAP_MINUTES,
                    help=f"Wall-clock gap separating dives (default: {DEFAULT_GAP_MINUTES:g})")
    ap.add_argument("--max-dt", type=float, default=DEFAULT_MAX_DT_S,
                    help=f"Time step above which logging is assumed to have stopped "
                         f"(default: {DEFAULT_MAX_DT_S:g}s)")
    ap.add_argument("--idle-speed", type=float, default=DEFAULT_IDLE_SPEED,
                    help=f"Speed below which the unit is stationary (default: "
                         f"{DEFAULT_IDLE_SPEED:g} m/s)")
    ap.add_argument("--min-idle", type=float, default=DEFAULT_MIN_IDLE_S,
                    help=f"Stationary stretch worth collapsing (default: "
                         f"{DEFAULT_MIN_IDLE_S:g}s)")
    ap.add_argument("--min-rows", type=int, default=DEFAULT_MIN_ROWS,
                    help=f"Fewer rows than this is not a dive (default: {DEFAULT_MIN_ROWS})")
    ap.add_argument("--min-path", type=float, default=DEFAULT_MIN_PATH_M,
                    help=f"Travelled less than this and it never went anywhere "
                         f"(default: {DEFAULT_MIN_PATH_M:g} m)")
    ap.add_argument("--no-trim", action="store_true", help="Keep every stationary anchor row")
    ap.add_argument("--json", metavar="FILE", help="Write a manifest of what was decided")
    args = ap.parse_args()

    fieldnames: list[str] = []
    runs: list[dict] = []
    dropped: list[dict] = []

    for path in args.logs:
        rows, fields = load_rows(path)
        if not rows:
            print(f"  skip  {os.path.basename(path)}: empty")
            continue
        if not fieldnames:
            fieldnames = fields

        src = os.path.basename(path)
        file_runs = split_runs(rows, args.max_dt)
        if len(file_runs) > 1:
            print(f"  note  {src}: contains {len(file_runs)} runs — the file has breaks in "
                  f"it (hand-spliced, or logging stopped and restarted)")

        for idx, run in enumerate(file_runs):
            label = src if len(file_runs) == 1 else f"{src}#{idx}"
            p = profile(run, label, args.min_rows, args.min_path)
            start, end = wall_clock(run)

            if p["classification"] in ("unplaceable", "trivial"):
                dropped.append(p)
                continue

            if not args.no_trim:
                run = trim_idle_anchors(run, args.idle_speed, args.min_idle)

            runs.append({"rows": run, "start": start, "end": end, "profile": p})

    if dropped:
        print("\nDropped:")
        for p in dropped:
            print(f"  {p['src']}: {p['classification']} — {CLASS_NOTE[p['classification']]} "
                  f"({p['rows']} rows, {p['duration_s']:.0f}s)")

    if not runs:
        print("\nNothing usable to assemble.")
        sys.exit(1)

    runs.sort(key=lambda r: r["start"])
    dives = group_into_dives(runs, args.gap_minutes)

    os.makedirs(args.outdir, exist_ok=True)

    manifest = []
    print(f"\n{len(dives)} dive(s) from {len(runs)} run(s):\n")
    for i, dive in enumerate(dives, 1):
        assign_segments(dive, args.max_dt)
        path, n = write_dive(dive, fieldnames, args.outdir)
        start = dive[0]["start"]
        end   = dive[-1]["end"]
        span  = (end - start).total_seconds() / 60.0

        print(f"  Dive {i}: {start:%Y-%m-%d %H:%M:%S} — {end:%H:%M:%S}  ({span:.0f} min)")
        prev = None
        for run in dive:
            p = run["profile"]
            trimmed = p["rows"] - len(run["rows"])
            note = f", {trimmed} idle rows trimmed" if trimmed else ""
            if prev is not None:
                gap = (run["start"] - prev["end"]).total_seconds()
                if run["seg"] == prev["seg"]:
                    print(f"      · joined across a {gap:.1f}s break — wall clock is "
                          f"continuous, so the DR chains straight through")
                else:
                    print(f"      ✂ {gap / 60:.1f} min gap — no data; DR will not be "
                          f"integrated across it")
            print(f"    seg {run['seg']}: {p['src']}  [{p['classification']}] "
                  f"{len(run['rows'])} rows, {p['path_m']:.0f} m{note}")
            prev = run

        n_segs = len({r["seg"] for r in dive})
        if dive[0]["rows"][0]["pos_src"] not in ANCHOR_SRCS:
            print(f"    → opens on dead reckoning: correct_track.py will place it by a "
                  f"later fix")
        print(f"    → {path}  ({n} rows, {n_segs} segment(s))\n")

        manifest.append({
            "dive": i, "output": path, "rows": n, "segments": n_segs,
            "started_at": start.isoformat(), "ended_at": end.isoformat(),
            "runs": [dict(r["profile"], seg=r["seg"]) for r in dive],
        })

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"dives": manifest, "dropped": dropped}, f, indent=2)
        print(f"Manifest: {args.json}")


if __name__ == "__main__":
    main()
