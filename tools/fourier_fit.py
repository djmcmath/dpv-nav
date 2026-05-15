#!/usr/bin/env python3
"""
Fit a Fourier heading-correction series to heading error data.

Usage:
    python fourier_fit.py hdg_samples.csv
    python fourier_fit.py hdg_samples.csv --plot

Input CSV (two columns, with header):
    actual,indicated
    0,353
    30,42
    ...

Output: hdg_fourier.json  (Fourier coefficients for the nav device)
        hdg_fourier_fit.png  (diagnostic plot, with --plot)
"""

import sys
import csv
import json
import argparse
import numpy as np
import matplotlib.pyplot as plt


def load_csv(path):
    with open(path, newline='') as f:
        reader = csv.DictReader(f)
        data = []
        for row in reader:
            actual    = float(row.get('actual',    row.get('Actual',    0)))
            indicated = float(row.get('indicated', row.get('Indicated', 0)))
            data.append((actual, indicated))
    return data


def wrap(deg):
    return (deg + 180) % 360 - 180


def design_matrix(theta_rad, n_harmonics):
    cols = [np.ones_like(theta_rad)]
    for k in range(1, n_harmonics + 1):
        cols += [np.cos(k * theta_rad), np.sin(k * theta_rad)]
    return np.column_stack(cols)


def fit(indicated_deg, correction_deg, n):
    theta  = np.deg2rad(indicated_deg)
    A      = design_matrix(theta, n)
    coeffs, *_ = np.linalg.lstsq(A, correction_deg, rcond=None)
    pred   = A @ coeffs
    resid  = correction_deg - pred
    rms    = np.sqrt(np.mean(resid**2))
    maxerr = np.max(np.abs(resid))
    return coeffs, pred, resid, rms, maxerr


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('csv', help='Path to heading sample CSV')
    parser.add_argument('--plot', action='store_true', help='Show and save diagnostic plot')
    args = parser.parse_args()

    data = load_csv(args.csv)
    if len(data) < 6:
        print(f"ERROR: need at least 6 data points, got {len(data)}")
        sys.exit(1)

    indicated_deg  = np.array([d[1] for d in data], dtype=float)
    correction_deg = np.array([wrap(d[0] - d[1]) for d in data])

    print(f"Loaded {len(data)} points from {args.csv}")
    print(f"Correction range: {correction_deg.min():.1f}° to {correction_deg.max():.1f}°  "
          f"(peak-to-peak {correction_deg.max() - correction_deg.min():.1f}°)\n")

    print(f"{'N':>4}  {'Params':>6}  {'RMS':>8}  {'Max err':>8}")
    print("-" * 34)
    fits = {}
    for n in range(1, 5):
        coeffs, pred, resid, rms, maxerr = fit(indicated_deg, correction_deg, n)
        fits[n] = dict(coeffs=coeffs, pred=pred, resid=resid, rms=rms, maxerr=maxerr)
        print(f"{n:>4}  {2*n+1:>6}  {rms:>7.2f}°  {maxerr:>7.2f}°")

    best_n = next((n for n in range(1, 5) if fits[n]['maxerr'] < 3.0), 3)
    print(f"\nSelected: {best_n} harmonic(s)  "
          f"(RMS {fits[best_n]['rms']:.2f}°, max {fits[best_n]['maxerr']:.2f}°)")

    coeffs = fits[best_n]['coeffs']
    print(f"\nHarmonic amplitudes:")
    print(f"  DC offset:    {coeffs[0]:+.2f}°")
    for k in range(1, best_n + 1):
        amp = np.sqrt(coeffs[2*k-1]**2 + coeffs[2*k]**2)
        print(f"  {k}{'st' if k==1 else 'nd' if k==2 else 'rd' if k==3 else 'th'} harmonic: {amp:.2f}° amplitude")

    out = {'n': best_n, 'c': [round(float(v), 6) for v in coeffs]}
    with open('hdg_fourier.json', 'w') as f:
        json.dump(out, f, indent=2)
    print(f"\nSaved hdg_fourier.json")

    print(f"\n{'Actual':>7} {'Indic':>7} {'Meas corr':>10} {'Fit corr':>9} {'Residual':>10}")
    for i, (act, ind) in enumerate(data):
        print(f"{act:>7.0f} {ind:>7.0f} {correction_deg[i]:>+10.1f}"
              f" {fits[best_n]['pred'][i]:>+9.1f} {fits[best_n]['resid'][i]:>+10.1f}")

    if args.plot:
        theta_plot = np.linspace(0, 2 * np.pi, 720)
        deg_plot   = np.rad2deg(theta_plot)
        fig, axes  = plt.subplots(2, 2, figsize=(14, 9))
        fig.suptitle(f'Heading Fourier correction fit — {args.csv}')
        for idx, n in enumerate([1, 2, 3, 4]):
            ax       = axes[idx // 2][idx % 2]
            fit_plot = design_matrix(theta_plot, n) @ fits[n]['coeffs']
            ax.scatter(indicated_deg, correction_deg, s=30, zorder=5, label='Measured')
            ax.plot(deg_plot, fit_plot, color='crimson', linewidth=1.5,
                    label=f'{n}-harmonic fit')
            ax.bar(indicated_deg, fits[n]['resid'], width=8, alpha=0.4,
                   color='orange', label='Residual')
            ax.axhline(0, color='#888', linewidth=0.5)
            ax.set_title(f'{n} harmonic(s) — RMS {fits[n]["rms"]:.1f}°, '
                         f'max {fits[n]["maxerr"]:.1f}°')
            ax.set_xlabel('Indicated heading (°)')
            ax.set_ylabel('Correction (°)')
            ax.set_xlim(0, 360)
            ax.legend(fontsize=8)
            ax.grid(True, alpha=0.3)
        plt.tight_layout()
        plt.savefig('hdg_fourier_fit.png', dpi=150)
        plt.show()
        print("Plot saved to hdg_fourier_fit.png")


if __name__ == '__main__':
    main()
