#!/usr/bin/env python3
"""
Audit a level bench heading test: logged heading vs a known reference, per stop.

Test procedure the log should come from:
  - scooter level on a non-magnetic stand, log level HIGH (needs mag_*_cal, pitch/roll)
  - optional static lead-in: leave it still at the first heading (reported as field drift)
  - stop every --step degrees, or any multiple of it (magnetic, from a reference compass),
    through one or more full circles one way, then reverse for one or more circles the other
    way (e.g. CW, CW, CCW, CCW). Each stop's actual heading is inferred from its measured step
    rounded to the nearest --step multiple, so uneven spacing and an accidental extra stop are
    both handled -- it only needs the error to stay well under half a --step between stops.
  - first stop at --first-actual (default 000)
A static-only log gets just the drift report.

Usage:
    python circle_audit.py "logs/high log circles.csv" \\
        --fourier "../baseline cal jsons/20260911 cal files/hdg_fourier (1).json" \\
        --mount   "../baseline cal jsons/20260911 cal files/mag_mount (1).json"

Pipeline this assumes (nav_main.cpp): logged heading_deg is TRUE = Mahony yaw + decl, with
hdg_cal::apply() in the magnetic domain, then motor_cal heading_offset_deg. The display's
MAG mode shows logged - decl (display_main.cpp), so "displayed" here == what the diver read.
--fourier must be the hdg_fourier.json that was installed while the log was recorded.

Reports:
  - stop matching, and per-stop error for each circle
  - displayed error per bin: samples / stops / mean / max, worst case + RMS
  - per circle: error summary and the horizontal field centre -- a centre that moves between
    circles means the scooter's own field changed
  - circle pairs at the same headings: two circles the same way, adjacent in time, differ only
    by time; two circles either side of a reversal differ mainly by direction
  - constant / 1 / 2 / 3-cycle decomposition, displayed and pre-Fourier
  - loaded Fourier vs a refit on this data (is the heading cal stale?)
  - |B| and level |Bh| gates with a horizontal circle fit (leftover hard iron)
  - held-out RMS: fit one circle, score the others; 12-point-style refit scored on the rest
  - field drift over a static lead-in
  - with --mount: what a level-only mounted fit would give vs the installed mount stage

Reading the harmonics: constant = reference/mount misalignment + motor offset; 1-cycle =
hard iron; 2-cycle = soft iron / mount-scale error. 2nd and 3rd harmonics are NOT proof of a
code problem: a leftover hard-iron offset d against horizontal field R produces heading
harmonics of roughly (d/R)^k / k radians, so a large offset generates them by itself.
"""

import argparse
import csv
import json
import sys
from itertools import combinations

import numpy as np

LIS3MDL_LSB_PER_UT = 68.42  # +/-4 gauss range; cal JSON biases are counts, log columns are uT
LEAD_IN_S = 120.0           # a first stretch at one heading longer than this is a static lead-in
LEAD_IN_BAND_DEG = 5.0      # ...and it lasts until the heading leaves this band


def wrap180(a):
    return (np.asarray(a) + 180.0) % 360.0 - 180.0


def cmean(deg):
    r = np.radians(deg)
    return np.degrees(np.arctan2(np.sin(r).mean(), np.cos(r).mean())) % 360.0


def design(theta_deg, n):
    th = np.radians(theta_deg)
    cols = [np.ones_like(th)]
    for k in range(1, n + 1):
        cols += [np.cos(k * th), np.sin(k * th)]
    return np.column_stack(cols)


def lsq(x_deg, y, n):
    cf = np.linalg.lstsq(design(x_deg, n), y, rcond=None)[0]
    return cf, y - design(x_deg, n) @ cf


def harm_str(cf):
    parts = [f"const {cf[0]:6.2f}"]
    for k in range(1, (len(cf) - 1) // 2 + 1):
        a, b = cf[2 * k - 1], cf[2 * k]
        parts.append(f"{k}-cyc {np.hypot(a, b):5.2f}@{np.degrees(np.arctan2(b, a)) % 360:03.0f}")
    return "  ".join(parts)


def rms(x):
    return float(np.sqrt(np.mean(np.square(x))))


def fourier_corr(x_deg, c, n):
    th = np.radians(x_deg)
    s = np.full_like(th, c[0], dtype=float)
    for k in range(1, n + 1):
        s += c[2 * k - 1] * np.cos(k * th) + c[2 * k] * np.sin(k * th)
    return s


def circle_fit(x, y):
    A = np.c_[2 * x, 2 * y, np.ones_like(x)]
    s = np.linalg.lstsq(A, x ** 2 + y ** 2, rcond=None)[0]
    return s[0], s[1], float(np.sqrt(s[2] + s[0] ** 2 + s[1] ** 2))


def axis_scale(u, v):
    """Axis-aligned ellipse semi-axes of centred data, as fit_mounted does (unregularised)."""
    M = np.array([[np.sum(u ** 4), np.sum(u ** 2 * v ** 2)], [np.sum(u ** 2 * v ** 2), np.sum(v ** 4)]])
    a, c = np.linalg.solve(M, np.array([np.sum(u ** 2), np.sum(v ** 2)]))
    rx, ry = 1 / np.sqrt(a), 1 / np.sqrt(c)
    avg = (rx + ry) / 2
    return avg / rx, avg / ry


def find_stops(disp, still_deg=0.8, min_len=3, merge_deg=5.0):
    """Dwell = consecutive samples moving < still_deg. Consecutive dwells within merge_deg are
    one stop ("move, then correct when the compass catches up") and the later dwell is kept.
    The first sample of a dwell is still settling and is dropped when the dwell is long enough."""
    still = np.r_[False, np.abs(wrap180(np.diff(disp))) < still_deg]
    segs, cur = [], [0]
    for i in range(1, len(disp)):
        if still[i]:
            cur.append(i)
        else:
            if len(cur) >= min_len:
                segs.append(cur)
            cur = [i]
    if len(cur) >= min_len:
        segs.append(cur)
    stops = []
    for s in segs:
        if stops and abs(wrap180(cmean(disp[s]) - cmean(disp[stops[-1]]))) < merge_deg:
            stops[-1] = s
        else:
            stops.append(s)
    return [np.array(s[1:] if len(s) > 3 else s) for s in stops]


def split_sweeps(hs):
    """Stop indices where the rotation direction reverses (sustained for 3 steps)."""
    sgn = np.sign(wrap180(np.diff(hs)))
    if not len(sgn):
        return [0, len(hs)], 1.0
    d0 = float(np.sign(np.median(sgn[:5]))) or 1.0
    bounds, cur = [0], d0
    for i in range(len(sgn)):
        if sgn[i] != cur and np.all(sgn[i:i + 3] == -cur):
            bounds.append(i + 1)
            cur = -cur
    return bounds + [len(hs)], d0


def fnum(s):
    try:
        return float(s)
    except ValueError:
        return np.nan


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", help="HIGH-level nav log CSV")
    ap.add_argument("--fourier", help="hdg_fourier.json installed while the log was recorded (omit if none)")
    ap.add_argument("--mount", help="mag_mount.json installed while the log was recorded (adds the level-only mounted-fit check)")
    ap.add_argument("--decl", type=float, default=14.7, help="firmware DEFAULT_DECLINATION_DEG (default 14.7)")
    ap.add_argument("--motor", type=float, default=-3.0, help="motor_cal.json heading_offset_deg (default -3)")
    ap.add_argument("--first-actual", type=float, default=0.0, help="actual heading of the first stop (default 0)")
    ap.add_argument("--step", type=float, default=10.0,
                    help="reference grid quantum, deg (default 10); stop spacing may be any multiple of it")
    ap.add_argument("--second-start", type=float, help="actual heading of the first stop after the first reversal (default: auto)")
    args = ap.parse_args()

    rows = list(csv.DictReader(open(args.csv, newline="")))
    col = lambda k: np.array([fnum(r[k]) for r in rows])
    try:
        mx, my, mz = col("mag_x_cal"), col("mag_y_cal"), col("mag_z_cal")
        pitch, roll = col("pitch_deg"), col("roll_deg")
    except KeyError:
        sys.exit("needs a HIGH-level log (mag_*_cal, pitch_deg, roll_deg columns)")
    tboot = col("timestamp_ms") / 1000.0
    t = tboot - tboot[0]
    h = col("heading_deg")
    # Prefer the magnetometer's own die temperature: the MS5837 in the nose lags
    # what the sensor actually feels by ~23 s (measured 2026-09-11), which is
    # exactly the wrong property for explaining a drifting hard-iron offset.
    if "mag_temp_c" in rows[0]:
        temp, temp_src = col("mag_temp_c"), "mag_temp_c (LIS3MDL die)"
    elif "water_temp_c" in rows[0]:
        temp, temp_src = col("water_temp_c"), "water_temp_c (MS5837, lags ~23 s)"
    else:
        temp, temp_src = np.full(len(rows), np.nan), "none"
    cal = np.c_[mx, my, mz]

    # ---- heading stages ----------------------------------------------------------------
    disp = (h - args.decl) % 360.0         # display MAG mode: what the diver read
    fout = (disp - args.motor) % 360.0     # Fourier output, before the motor offset
    if args.fourier:
        hf = json.load(open(args.fourier))
        c, n = np.array(hf["c"], float), int(hf["n"])
    else:
        c, n = np.zeros(1), 0
    grid = np.arange(0, 360, 0.01)
    F = grid + fourier_corr(grid, c, n)
    slope = np.gradient(F, grid)
    print(f"log: {len(rows)} rows, {t[-1]:.0f} s, started {tboot[0]:.0f} s after boot.  decl {args.decl}, motor offset {args.motor}")
    print(f"temperature column: {temp_src}")
    if n:
        amps = ", ".join(f"{k}-cyc {np.hypot(c[2*k-1], c[2*k]):.1f}" for k in range(1, n + 1))
        print(f"loaded Fourier n={n}: const {c[0]:.2f}, {amps} deg;  map slope {slope.min():.2f}..{slope.max():.2f}")
    pre = None
    if slope.min() > 0:
        pre = np.interp((fout - F[0]) % 360.0 + F[0], F, grid)   # pre-Fourier (Mahony) magnetic heading
    else:
        print("WARNING: Fourier map folds (slope <= 0) -- two headings display the same; pre-Fourier skipped")

    # Direct tilt-compensated mag heading; sign convention chosen to best match Mahony.
    p, rl = np.radians(pitch), np.radians(roll)
    ref = pre if pre is not None else fout
    best = None
    for sr in (1, -1):
        for sp in (1, -1):
            xh = mx * np.cos(sp * p) + my * np.sin(sr * rl) * np.sin(sp * p) + mz * np.cos(sr * rl) * np.sin(sp * p)
            yh = my * np.cos(sr * rl) - mz * np.sin(sr * rl)
            hm = np.degrees(np.arctan2(yh, xh)) % 360.0
            s = wrap180(ref - hm).std()
            if best is None or s < best[0]:
                best = (s, xh, yh)
    _, xh, yh = best
    B = np.sqrt(mx ** 2 + my ** 2 + mz ** 2)
    Bh = np.hypot(xh, yh)

    # ---- static lead-in ----------------------------------------------------------------
    away = np.where(np.abs(wrap180(disp - cmean(disp[:10]))) > LEAD_IN_BAND_DEG)[0]
    lead_end = int(away[0]) if len(away) else len(disp)
    lead = np.arange(lead_end) if t[lead_end - 1] > LEAD_IN_S else None
    if lead is not None:
        tb = tboot[lead]
        span = tb[-1] - tb[0]
        step = 120.0 if span > 900 else 60.0
        print(f"\nFIELD DRIFT over the static lead-in ({span / 60:.1f} min at one heading)")
        print("  t_boot   temp |  cal x    cal y    cal z  (uT) | displayed hdg  pitch  roll")
        for a in np.arange(tb[0], tb[-1], step):
            m = lead[(tb >= a) & (tb < a + step)]
            if len(m):
                print(f"  {a:6.0f} {np.nanmean(temp[m]):6.2f} | {mx[m].mean():7.2f} {my[m].mean():7.2f} {mz[m].mean():7.2f}      |"
                      f"   {cmean(disp[m]):7.2f}     {pitch[m].mean():5.2f} {roll[m].mean():5.2f}")
        f, l = lead[tb < tb[0] + step], lead[tb >= tb[-1] - step]
        d = cal[l].mean(0) - cal[f].mean(0)
        print(f"  first->last {step:.0f} s: field ({d[0]:+.2f}, {d[1]:+.2f}, {d[2]:+.2f}) uT, displayed heading "
              f"{float(wrap180(cmean(disp[l]) - cmean(disp[f]))):+.2f} deg, temp {np.nanmean(temp[l]) - np.nanmean(temp[f]):+.2f} C")
        ok = np.isfinite(temp[lead])
        if ok.sum() > 10 and np.ptp(temp[lead][ok]) > 0.1:
            for k, ax in enumerate("xyz"):
                y = cal[lead, k][ok]
                print(f"  cal {ax} vs {temp_src.split(' ')[0]}: {np.polyfit(temp[lead][ok], y, 1)[0]:+.2f} uT/C"
                      f" (r {np.corrcoef(temp[lead][ok], y)[0, 1]:+.2f})")

    # ---- stops, sweeps, circles --------------------------------------------------------
    stops = find_stops(disp)
    if lead is not None:
        stops = [lead[-5:]] + [s for s in stops if s[0] >= lead_end]
    if len(stops) < 8:
        print("\nno circles after the lead-in -- drift report only")
        return
    hs = np.array([cmean(disp[s]) for s in stops])
    bounds, d0 = split_sweeps(hs)
    name = lambda d: "CW" if d > 0 else "CCW"

    def e_at(s, a):
        return float(wrap180(cmean(disp[s]) - a))

    recs, count = [], {1.0: 0, -1.0: 0}

    def infer_acts(SS, start):
        """Actual heading per stop: each step is the measured step rounded to the nearest grid
        multiple, so stops need not be evenly spaced. Returns (actuals, cumulative travel)."""
        hsw = np.array([cmean(disp[s]) for s in SS])
        inc = np.r_[0.0, np.round(wrap180(np.diff(hsw)) / args.step) * args.step]
        return start + np.cumsum(inc), np.cumsum(np.abs(inc))

    def add_sweep(sw, SS, acts, travel, d):
        ncirc, seen = 0, {}
        for k, (s, a) in enumerate(zip(SS, acts)):
            ci = int(np.floor((travel[k] - 1e-6) / 360.0)) if travel[k] > 0 else 0
            ncirc = max(ncirc, ci + 1)
            lab = f"{name(d)}{count[d] + ci + 1}"
            am = round(float(a % 360), 3)
            closure = am in seen.setdefault(lab, set())
            seen[lab].add(am)
            recs.append(dict(label=lab, dir=name(d), sweep=sw, act=am, closure=closure, t=float(t[s[0]]), idx=s,
                             e=e_at(s, a),
                             e_pre=float(wrap180(cmean(pre[s]) - a)) if pre is not None else np.nan,
                             pre=cmean(pre[s]) if pre is not None else np.nan,
                             Bh=float(Bh[s].mean()), temp=float(np.nanmean(temp[s]))))
        count[d] += ncirc

    SS0 = stops[bounds[0]:bounds[1]]
    acts, travel = infer_acts(SS0, args.first_actual)
    add_sweep(0, SS0, acts, travel, d0)
    d = d0
    for sw in range(1, len(bounds) - 1):
        d = -d
        SS = stops[bounds[sw]:bounds[sw + 1]]
        if sw == 1 and args.second_start is not None:
            start = args.second_start
        else:
            known = {}
            for r in recs:
                if not r["closure"]:
                    known.setdefault(r["act"], []).append(r["e"])
            cands = {}
            for j in (-1, 0, 1, 2, 3, 4):
                a0 = acts[-1] + d * args.step * j
                aa, _ = infer_acts(SS, a0)
                diffs = [e_at(s, a) - np.median(known[round(float(a % 360), 3)])
                         for s, a in zip(SS, aa) if round(float(a % 360), 3) in known]
                if diffs:
                    cands[a0 % 360] = float(np.median(np.abs(diffs)))
            start = min(cands, key=cands.get)
            print(f"sweep {sw + 1} start candidates (median |disagreement| with earlier circles): "
                  + ", ".join(f"{a:03.0f}: {v:.1f}" for a, v in sorted(cands.items(), key=lambda kv: kv[1])[:4]))
        acts, travel = infer_acts(SS, start)
        add_sweep(sw, SS, acts, travel, d)

    labels = list(dict.fromkeys(r["label"] for r in recs))
    circ = {L: [r for r in recs if r["label"] == L] for L in labels}
    core = {L: [r for r in circ[L] if not r["closure"]] for L in labels}
    scored = [r for r in recs if not r["closure"]]
    print("stops: " + ", ".join(f"{L} {len(core[L])} ({core[L][0]['act']:03.0f}->{core[L][-1]['act']:03.0f})" for L in labels)
          + ("  [override the first reversal with --second-start]" if len(labels) > 1 else ""))

    # ---- per-stop table ----------------------------------------------------------------
    print("\nPER STOP (error = displayed - actual, incl. motor offset)")
    print(" act | " + " ".join(f"{L:>6s}" for L in labels) + " | level |Bh| uT")
    for a in sorted({r["act"] for r in recs}):
        cells = []
        for L in labels:
            m = [r for r in core[L] if r["act"] == a]
            cells.append(f"{m[0]['e']:6.1f}" if m else "     -")
        print(f" {a:03.0f} | {' '.join(cells)} | {np.mean([r['Bh'] for r in recs if r['act'] == a]):5.1f}")
    for r in recs:
        if r["closure"]:
            f0 = next(q for q in core[r["label"]] if q["act"] == r["act"])
            print(f" closure {r['label']}: {r['act']:03.0f} read {f0['e']:.1f} at t={f0['t']:.0f}s, {r['e']:.1f} at t={r['t']:.0f}s")

    # ---- bins --------------------------------------------------------------------------
    print(f"\nBINS ({args.step:.0f} deg, displayed error, every settled sample)")
    print("  bin | samples stops |  mean    max")
    sa = np.array([(r["act"], float(wrap180(disp[i] - r["act"]))) for r in recs for i in r["idx"]])
    worst = (0.0, 0.0)
    for a in np.arange(0, 360, args.step):
        mk = np.abs(wrap180(sa[:, 0] - a)) < args.step / 2
        ns = sum(1 for r in recs if abs(wrap180(r["act"] - a)) < args.step / 2)
        if mk.any():
            e = sa[mk, 1]
            j = np.abs(e).argmax()
            print(f"  {a:03.0f} | {mk.sum():7d} {ns:5d} | {e.mean():6.1f} {e[j]:6.1f}")
            if abs(e[j]) > abs(worst[1]):
                worst = (a, e[j])
        else:
            print(f"  {a:03.0f} |       0     0 |  (no coverage)")
    es = np.array([r["e"] for r in recs])
    print(f"  worst sample {worst[1]:.1f} at {worst[0]:03.0f};  RMS stops {rms(es):.2f}, samples {rms(sa[:, 1]):.2f};"
          f"  mean {es.mean():.2f};  RMS without motor offset {rms(es - args.motor):.2f}")

    # ---- per circle and circle pairs ---------------------------------------------------
    print("\nPER CIRCLE (a horizontal centre that moves between circles = the scooter's own field changed)")
    print(" circle    t (s)      temp  stops   mean    RMS   worst      centre (uT)       R")
    for L in labels:
        C = core[L]
        e = np.array([r["e"] for r in C])
        j = np.abs(e).argmax()
        ii = np.concatenate([r["idx"] for r in circ[L]])
        cx, cy, R = circle_fit(xh[ii], yh[ii]) if len(C) >= 8 else (np.nan, np.nan, np.nan)
        print(f" {L:6s} {C[0]['t']:5.0f}-{C[-1]['t']:5.0f} {np.nanmean(temp[ii]):6.2f} {len(C):5d} {e.mean():6.2f} {rms(e):6.2f}"
              f" {e[j]:6.1f}@{C[j]['act']:03.0f}  ({cx:6.2f}, {cy:6.2f}) {R:6.2f}")
    if len(labels) > 1:
        print("\nCIRCLE PAIRS at the same headings (A - B). Same direction = time only; across a reversal = direction too.")
        print(" pair          shared  mean diff  RMS diff  max|diff|   dt (s)  dtemp")
        for A, Bc in combinations(labels, 2):
            ea = {r["act"]: r for r in core[A]}
            eb = {r["act"]: r for r in core[Bc]}
            sh = sorted(set(ea) & set(eb))
            if len(sh) < 8:
                continue
            dd = np.array([ea[a]["e"] - eb[a]["e"] for a in sh])
            dt = np.mean([eb[a]["t"] - ea[a]["t"] for a in sh])
            dT = np.mean([eb[a]["temp"] - ea[a]["temp"] for a in sh])
            print(f" {A:>5s}-{Bc:<5s}   {len(sh):5d}   {dd.mean():7.2f}  {rms(dd):7.2f}   {np.abs(dd).max():7.1f}   {dt:7.0f} {dT:6.2f}")

    # ---- harmonics ---------------------------------------------------------------------
    print("\nHARMONICS vs actual heading (amp@heading of the most positive error)")
    A_s = np.array([r["act"] for r in scored])
    e_s = np.array([r["e"] for r in scored])
    cf, res = lsq(A_s, e_s, 3)
    print(f"  displayed, all       {harm_str(cf)}   resid {rms(res):.2f}")
    if len(labels) > 1:
        acts_u = sorted({r["act"] for r in scored})
        mean_e = np.array([np.mean([r["e"] for r in scored if r["act"] == a]) for a in acts_u])
        cf, res = lsq(np.array(acts_u), mean_e, 3)
        print(f"  circle mean          {harm_str(cf)}   resid {rms(res):.2f}")
        for L in labels:
            if len(core[L]) >= 12:
                cf, res = lsq(np.array([r["act"] for r in core[L]]), np.array([r["e"] for r in core[L]]), 3)
                print(f"  {L:20s} {harm_str(cf)}   resid {rms(res):.2f}")
    if pre is not None:
        ep = np.array([r["e_pre"] for r in scored])
        cf, res = lsq(A_s, ep, 3)
        print(f"  pre-Fourier          {harm_str(cf)}   resid {rms(res):.2f}")
        if n:
            nf = max(n, 3)
            cf, _ = lsq(np.array([r["pre"] for r in scored]), -ep, nf)
            print(f"  Fourier refit on this data vs loaded (a stale heading cal shows up here):")
            for k in range(1, nf + 1):
                lc, ls = (c[2 * k - 1], c[2 * k]) if k <= n else (0.0, 0.0)
                print(f"    {k}-cyc: refit {np.hypot(cf[2*k-1], cf[2*k]):5.2f}  loaded {np.hypot(lc, ls):5.2f}"
                      f"  vector diff {np.hypot(cf[2*k-1] - lc, cf[2*k] - ls):4.2f}")
            print(f"    const: refit {cf[0]:5.2f}  loaded {c[0]:5.2f}")

    # ---- |B| gate ----------------------------------------------------------------------
    idx_all = np.concatenate([r["idx"] for r in recs])
    print("\n|B| GATE (settled samples)")
    Bs, Bhs = B[idx_all], Bh[idx_all]
    print(f"  |B|  total  {Bs.mean():5.2f} uT, spread {Bs.std():.2f} ({100 * Bs.std() / Bs.mean():.1f}%)  "
          f"<- understates: dominated by the vertical field")
    print(f"  |Bh| level  {Bhs.mean():5.2f} uT, spread {Bhs.std():.2f} ({100 * Bhs.std() / Bhs.mean():.1f}%), "
          f"{Bhs.min():.1f}..{Bhs.max():.1f}")
    cx, cy, R = circle_fit(xh[idx_all], yh[idx_all])
    dc = np.hypot(cx, cy)
    print(f"  horizontal circle: centre ({cx:.2f}, {cy:.2f}) uT, offset {dc:.2f} vs radius {R:.2f} (ratio {dc / R:.2f})"
          f" -> ~{np.degrees(np.arcsin(min(dc / R, 1))):.0f} deg 1-cycle before any Fourier")

    # ---- held-out ----------------------------------------------------------------------
    if len(labels) > 1:
        print("\nHELD-OUT (displayed error, fit on one circle, score on each other circle)")
        for N in range(4):
            ins, out = [], []
            for A in labels:
                for Bc in labels:
                    if A == Bc or len(core[A]) < 2 * N + 4:
                        continue
                    xa, ya = np.array([r["act"] for r in core[A]]), np.array([r["e"] for r in core[A]])
                    xb, yb = np.array([r["act"] for r in core[Bc]]), np.array([r["e"] for r in core[Bc]])
                    cfN, resN = lsq(xa, ya, N)
                    ins.append(rms(resN))
                    out.append(rms(yb - design(xb, N) @ cfN))
            if ins:
                print(f"  up to {N}-cyc: in-sample {np.mean(ins):.2f}   held-out {np.mean(out):.2f}")
    if pre is not None:
        print("  12-point-style refit (every 3rd heading), scored on the other headings:")
        for N in (1, 2, 3):
            ins, out = [], []
            for off in (0, 1, 2):
                tr = [r for r in scored if round(r["act"] / args.step) % 3 == off]
                te = [r for r in scored if round(r["act"] / args.step) % 3 != off]
                X = lambda S: design(np.array([r["pre"] for r in S]), N)
                Y = lambda S: -np.array([r["e_pre"] for r in S])
                cfN = np.linalg.lstsq(X(tr), Y(tr), rcond=None)[0]
                ins.append(rms(Y(tr) - X(tr) @ cfN))
                out.append(rms(Y(te) - X(te) @ cfN))
            print(f"    n={N}: in-sample {np.mean(ins):.2f}   held-out {np.mean(out):.2f}")

    # ---- mounted-stage check -----------------------------------------------------------
    if args.mount:
        mm = json.load(open(args.mount))
        Sm = np.array(mm["softIron"], float)
        bm = np.array([mm["bias"][k] for k in "xyz"], float) / LIS3MDL_LSB_PER_UT
        bc = (np.linalg.inv(Sm) @ cal.T).T + bm          # undo the mount stage -> what fit_mounted sees
        lvl = idx_all[(np.abs(pitch[idx_all]) < 5) & (np.abs(roll[idx_all]) < 5)]
        # tilt-compensate before fitting: even 1-2 deg of stand tilt leaks ~1 uT of vertical field into x/y
        bxh = bc[:, 0] * np.cos(p) - bc[:, 1] * np.sin(rl) * np.sin(p) + bc[:, 2] * np.cos(rl) * np.sin(p)
        byh = bc[:, 1] * np.cos(rl) + bc[:, 2] * np.sin(rl)
        bx, by, _ = circle_fit(bxh[lvl], byh[lvl])
        sx, sy = axis_scale(bxh[lvl] - bx, byh[lvl] - by)
        print("\nMOUNTED STAGE: installed vs a level-only, tilt-compensated fit of this log (uT, base-corrected frame)")
        print(f"  installed      bias ({bm[0]:6.2f}, {bm[1]:6.2f})  scale ({Sm[0, 0]:.3f}, {Sm[1, 1]:.3f})")
        print(f"  level-only fit bias ({bx:6.2f}, {by:6.2f})  scale ({sx:.3f}, {sy:.3f})   [{len(lvl)} samples]"
              f"  = counts ({bx * LIS3MDL_LSB_PER_UT:.1f}, {by * LIS3MDL_LSB_PER_UT:.1f})")
        print("  A lopsided installed scale with a round level fit means the mounted cal's tilted circles were")
        print("  fit as if level (fit_mounted has no tilt compensation): vertical field leaks into x when an end is raised.")


if __name__ == "__main__":
    main()
