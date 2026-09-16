#!/usr/bin/env python3
"""
Fit magnetometer thermal-drift coefficients from stationary heat/cool logs, and
write the /mag_temp.json the nav firmware loads.

Test procedure the logs should come from (docs/mag-temperature-compensation.md):
  - scooter stationary and level, nothing magnetic moving nearby, log level MID or HIGH
  - ~15 s of baseline, heat the nav board (heat gun, 45-60 s), then leave it untouched
    while it cools back toward ambient (30+ min)
  - ideally one log per cardinal heading: the drift is a body-fixed offset, so every
    heading should give the same coefficients -- agreement across headings is the check

Usage:
    # MID logs carry calibrated columns, so the cal that was INSTALLED while logging
    # is needed to take them back to the raw (pre-calibration) frame:
    python mag_temp_fit.py "../baseline cal jsons/thermal testing/"{north,east,south,west1}.csv \\
        --base  "../baseline cal jsons/20260911 cal files/mag_base (2).json" \\
        --mount "../baseline cal jsons/20260911 level-fit mount v2/mag_mount.json" \\
        --out mag_temp.json

    # HIGH logs carry mag_*_raw, so no cal files are needed:
    python mag_temp_fit.py cold_test_high.csv --out mag_temp.json

    # A log recorded WITH compensation running only shows what it missed. Pass the
    # file that was installed and the fit reports the total:
    python mag_temp_fit.py new_test.csv --applied mag_temp.json --out mag_temp.json

What it fits: on each log's cooling leg (from --skip-s after the die-temperature peak to the
end), mag = intercept_file + coeff * mag_temp_c, per axis, one shared coeff and one intercept
per log. Only the cooling leg by default: it is slow, so the die and the field are in step,
and it is the leg the heading guideline cares about. --whole uses every row instead.

Why the raw frame: the firmware applies the correction before any calibration, so the same
coefficients stay valid through recalibration. A slope measured on calibrated columns is in
the calibrated frame; it is mapped back through the inverse of the installed soft-iron
matrix. Get --base/--mount wrong and the coefficients come out wrong -- the old tilted mount
(scale 0.80/1.35) versus the level-fit one moves x by ~20 %.

Refuses to fit a log whose die temperature moved less than --min-range-c: a slope over a
range comparable to the sensor's 0.125 °C step is noise with a sign.
"""

import argparse
import csv
import datetime
import json
import math
import os
import sys

import numpy as np

AXES = "xyz"


def load_matrix(path):
    with open(path) as f:
        d = json.load(f)
    return np.array(d["softIron"], dtype=float)


def read_log(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        sys.exit(f"{path}: empty log")
    cols = rows[0].keys()
    if "mag_temp_c" not in cols:
        sys.exit(f"{path}: no mag_temp_c column (needs firmware from 2026-09-11 or later)")
    if all(f"mag_{a}_raw" in cols for a in AXES):
        frame, prefix = "raw", "raw"
    elif all(f"mag_{a}_cal" in cols for a in AXES):
        frame, prefix = "cal", "cal"
    else:
        sys.exit(f"{path}: needs mag_*_raw (HIGH) or mag_*_cal (MID) columns")

    t, temp, mag = [], [], []
    for r in rows:
        try:
            tc = float(r["mag_temp_c"])
            m = [float(r[f"mag_{a}_{prefix}"]) for a in AXES]
            ts = float(r["timestamp_ms"]) / 1000.0
        except (TypeError, ValueError):
            continue
        # MARK rows write zeros for every sensor column; nan temps are failed reads.
        if not math.isfinite(tc) or not all(map(math.isfinite, m)) or m == [0.0, 0.0, 0.0]:
            continue
        t.append(ts)
        temp.append(tc)
        mag.append(m)
    return frame, np.array(t), np.array(temp), np.array(mag)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", help="MID or HIGH log CSVs")
    ap.add_argument("--base", help="mag_base.json installed while the MID logs were recorded")
    ap.add_argument("--mount", help="mag_mount.json installed while the MID logs were recorded (omit if none)")
    ap.add_argument("--applied", help="mag_temp.json that was ACTIVE while logging; its coeffs are added back")
    ap.add_argument("--ref", type=float, default=21.0,
                    help="ref_temp_c to write: die temperature the calibrations were collected at (default 21.0)")
    ap.add_argument("--skip-s", type=float, default=15.0, help="seconds after the temperature peak to skip (default 15)")
    ap.add_argument("--whole", action="store_true", help="fit every row, not just the cooling leg")
    ap.add_argument("--min-range-c", type=float, default=3.0,
                    help="minimum die-temperature range per log to fit it (default 3.0)")
    ap.add_argument("--out", help="write mag_temp.json here")
    args = ap.parse_args()

    M_eff = None
    if args.base:
        M_eff = load_matrix(args.base)
        if args.mount:
            M_eff = load_matrix(args.mount) @ M_eff
    elif args.mount:
        sys.exit("--mount needs --base")

    applied = np.zeros(3)
    if args.applied:
        with open(args.applied) as f:
            a = json.load(f)
        applied = np.array([a["coeff_uT_per_c"][k] for k in AXES], dtype=float)

    # Stack (temperature, one-hot log intercept) design across logs.
    temps, mags, owners, names, frames = [], [], [], [], set()
    print(f"{'log':28s} {'rows':>5s} {'die range °C':>14s}  per-log slope µT/°C (fit frame) x / y / z")
    for path in args.logs:
        frame, t, temp, mag = read_log(path)
        if not args.whole:
            ipk = int(np.argmax(temp))
            keep = t >= t[ipk] + args.skip_s
            t, temp, mag = t[keep], temp[keep], mag[keep]
        name = os.path.basename(path)
        rng = (temp.max() - temp.min()) if len(temp) else 0.0
        if len(temp) < 20 or rng < args.min_range_c:
            print(f"{name:28s} {len(temp):5d} {rng:14.2f}  SKIPPED: die temperature range below "
                  f"{args.min_range_c} °C (or too few rows) -- cannot support a slope")
            continue
        X = np.c_[temp, np.ones_like(temp)]
        per = np.linalg.lstsq(X, mag, rcond=None)[0][0]
        print(f"{name:28s} {len(temp):5d} {temp.min():6.2f}-{temp.max():6.2f}  "
              f"{per[0]:+.3f} / {per[1]:+.3f} / {per[2]:+.3f}")
        frames.add(frame)
        temps.append(temp)
        mags.append(mag)
        owners.append(np.full(len(temp), len(names)))
        names.append(name)

    if not names:
        sys.exit("nothing to fit")
    if len(frames) > 1:
        sys.exit("mixed MID and HIGH logs: fit them separately")
    frame = frames.pop()
    if frame == "cal" and M_eff is None:
        sys.exit("MID logs are in the calibrated frame: pass --base (and --mount) for the cal installed while logging")
    if frame == "raw" and M_eff is not None:
        print("NOTE: HIGH logs carry raw columns; --base/--mount ignored")

    temp = np.concatenate(temps)
    mag = np.vstack(mags)
    owner = np.concatenate(owners)
    X = np.zeros((len(temp), 1 + len(names)))
    X[:, 0] = temp
    X[np.arange(len(temp)), 1 + owner] = 1.0
    beta, *_ = np.linalg.lstsq(X, mag, rcond=None)
    slope_fit = beta[0]
    resid = mag - X @ beta
    dof = len(temp) - X.shape[1]
    se = np.sqrt((resid ** 2).sum(axis=0) / dof * np.linalg.inv(X.T @ X)[0, 0])

    slope_raw = np.linalg.solve(M_eff, slope_fit) if frame == "cal" else slope_fit
    total = slope_raw + applied

    print(f"\npooled slope, fit frame ({frame}): "
          f"{slope_fit[0]:+.3f} / {slope_fit[1]:+.3f} / {slope_fit[2]:+.3f} µT/°C  "
          f"(SE {se[0]:.3f} / {se[1]:.3f} / {se[2]:.3f}; residual RMS "
          f"{np.sqrt((resid ** 2).mean(axis=0)).round(2).tolist()} µT)")
    if frame == "cal":
        print(f"raw (logical) frame via installed soft iron: "
              f"{slope_raw[0]:+.3f} / {slope_raw[1]:+.3f} / {slope_raw[2]:+.3f} µT/°C")
    if args.applied:
        print(f"+ applied {applied.round(3).tolist()} = total {total.round(3).tolist()} µT/°C")
    print(f"horizontal magnitude {math.hypot(total[0], total[1]):.3f} µT/°C; "
          f"fitted die range {temp.min():.2f}-{temp.max():.2f} °C -- do not trust it outside that range")

    if args.out:
        doc = {
            "ref_temp_c": args.ref,
            "coeff_uT_per_c": {k: round(float(v), 4) for k, v in zip(AXES, total)},
            "notes": {
                "fitted": datetime.date.today().isoformat(),
                "logs": names,
                "fit_frame": frame,
                "base": os.path.basename(args.base) if args.base else None,
                "mount": os.path.basename(args.mount) if args.mount else None,
                "applied": os.path.basename(args.applied) if args.applied else None,
                "die_range_c": [round(float(temp.min()), 2), round(float(temp.max()), 2)],
                "leg": "whole" if args.whole else f"cooling (peak + {args.skip_s:g} s)",
                "se_uT_per_c": {k: round(float(v), 4) for k, v in zip(AXES, se)},
            },
        }
        with open(args.out, "w") as f:
            json.dump(doc, f, indent=2)
            f.write("\n")
        print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
