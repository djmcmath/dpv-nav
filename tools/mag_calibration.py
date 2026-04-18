#!/usr/bin/env python3
"""
Magnetometer Ellipsoid Calibration Tool
Reads raw magnetometer samples (CSV format) and computes hard-iron offset + soft-iron correction matrix.

Usage:
    python mag_calibration.py <mag_samples.csv>

Output:
    - Prints calibration parameters
    - Generates calib_mag_cal.json file for uploading to ESP32

Theory:
    Raw magnetometer readings form an ellipsoid centered away from origin due to:
    - Hard-iron distortion (bias/offset): Nearby ferromagnetic materials creating constant field
    - Soft-iron distortion (scale/rotation): Magnetically permeable materials distorting field direction

    This script fits an ellipsoid to the raw samples and extracts:
    - Center (hard-iron offset)
    - Principal axes and radii (soft-iron correction)
"""

import numpy as np
import sys
import json
from scipy import linalg

def fit_ellipsoid_simple(X):
    """
    Simplified ellipsoid fitting using axis-aligned approach.
    More robust than full algebraic fit for noisy data.

    Assumes ellipsoid is roughly axis-aligned (valid for most magnetometer data).
    """
    # Step 1: Find approximate center using mean
    center_approx = np.mean(X, axis=0)

    # Step 2: Center the data
    X_centered = X - center_approx

    # Step 3: Find range along each axis
    min_vals = np.min(X_centered, axis=0)
    max_vals = np.max(X_centered, axis=0)

    # Step 4: Refined center (midpoint of range)
    center_offset = (max_vals + min_vals) / 2.0
    center = center_approx + center_offset

    # Step 5: Re-center with refined center
    X_centered = X - center

    # Step 6: Compute radii along each axis
    radii = (max_vals - min_vals) / 2.0

    # Step 7: PCA to find principal axes (rotation) of the data cloud
    cov = np.cov(X_centered.T)
    eigenvalues, eigenvectors = linalg.eigh(cov)
    rotation = eigenvectors

    # Step 8: Compute radii as min/max half-spans along the principal axes.
    # Min/max is more robust than covariance-based radii for non-uniform sample
    # distributions: covariance overestimates axes where samples cluster near the
    # poles, while min/max tracks the true geometric extent.
    X_aligned = X_centered @ rotation  # rotate data to principal frame
    radii = (np.max(X_aligned, axis=0) - np.min(X_aligned, axis=0)) / 2.0

    return center, radii, rotation


def fit_circle_2d(samples_xy):
    """
    Fit a 2D circle to X,Y samples using algebraic least-squares.

    This is the most accurate way to determine the horizontal hard-iron bias
    (bias_x, bias_y) for heading calibration:
    - Uses only the two axes that directly determine heading (atan2(y, x))
    - Not affected by how samples are distributed in elevation (upper vs lower
      hemisphere) — any azimuth-uniform flat rotation gives an accurate center
    - Works with partial azimuth coverage as long as it spans ≥180°

    Math: for each sample (x, y), the circle equation (x-cx)² + (y-cy)² = r²
    can be rewritten as the linear system:  2cx·x + 2cy·y + F = x² + y²
    where F = r² - cx² - cy².  Solve for (2cx, 2cy, F) by least squares.

    Returns (cx, cy, r).
    """
    # Pre-center for numerical stability (magnetometer counts can be ~thousands)
    offset = (np.max(samples_xy, axis=0) + np.min(samples_xy, axis=0)) / 2.0
    xy = samples_xy - offset
    x, y = xy[:, 0], xy[:, 1]

    A = np.column_stack([x, y, np.ones_like(x)])
    b = x**2 + y**2
    p, _, _, _ = np.linalg.lstsq(A, b, rcond=None)

    cx = p[0] / 2.0 + offset[0]
    cy = p[1] / 2.0 + offset[1]
    r = np.sqrt((p[0] / 2.0)**2 + (p[1] / 2.0)**2 + p[2])
    return cx, cy, r


def fit_ellipsoid_algebraic(X):
    """
    Algebraic ellipsoid fitting with robustness checks.

    Returns:
        center: (3,) hard-iron offset
        radii: (3,) ellipsoid radii
        rotation: (3, 3) rotation matrix
    """
    # Pre-center data for numerical stability.
    # The algebraic SVD fit is ill-conditioned when the ellipsoid is far from
    # the origin — common for magnetometers with large hard-iron offset (e.g.
    # DPV motors/batteries). The quadratic constant term d gets swamped by the
    # linear terms, causing the post-fit validity check to fail. Centering first
    # makes all coefficients the same order of magnitude.
    offset = (np.max(X, axis=0) + np.min(X, axis=0)) / 2.0
    X = X - offset

    x = X[:, 0]
    y = X[:, 1]
    z = X[:, 2]

    # Design matrix for ellipsoid equation:
    # ax^2 + by^2 + cz^2 + 2fxy + 2gxz + 2hyz + 2px + 2qy + 2rz + d = 0
    D = np.array([
        x*x, y*y, z*z, 2*x*y, 2*x*z, 2*y*z, 2*x, 2*y, 2*z, np.ones_like(x)
    ]).T

    # Solve using SVD (more stable than eigendecomposition)
    u, s, vh = linalg.svd(D)
    v = vh[-1, :]  # Last right singular vector (smallest singular value)

    # Extract quadratic coefficients
    A = np.array([
        [v[0], v[3], v[4]],
        [v[3], v[1], v[5]],
        [v[4], v[5], v[2]]
    ])

    b = v[6:9]
    d = v[9]

    # Check if A is positive definite (required for ellipsoid)
    eigvals_A = linalg.eigvalsh(A)

    if np.all(eigvals_A < 0):
        # All-negative eigenvalues: SVD solution came out sign-flipped.
        # Negating the entire solution (v → -v) gives the same center and
        # normalized ellipsoid but makes A positive definite.
        A = -A
        b = -b
        d = -d
        eigvals_A = -eigvals_A
    elif np.any(eigvals_A <= 0):
        print(f"\nWARNING: Algebraic fit produced non-ellipsoid (mixed eigenvalues: {eigvals_A})")
        print("Falling back to simple axis-aligned method...")
        c, r, rot = fit_ellipsoid_simple(X)
        return c + offset, r, rot

    # Solve for center: A * center = -b
    try:
        center = -linalg.solve(A, b)
    except linalg.LinAlgError:
        print("\nWARNING: Singular matrix in center calculation")
        print("Falling back to simple axis-aligned method...")
        c, r, rot = fit_ellipsoid_simple(X)
        return c + offset, r, rot

    # Translate to center
    # Ellipsoid equation at center: (x-c)^T A (x-c) + constant = 0
    # constant = c^T A c - b^T c + d
    const = center.T @ A @ center - b.T @ center + d

    # Normalize: (x-c)^T A (x-c) = -constant
    # For proper ellipsoid, -constant must be positive
    if -const <= 0:
        print(f"\nWARNING: Invalid ellipsoid constant: {const}")
        print("Falling back to simple axis-aligned method...")
        c, r, rot = fit_ellipsoid_simple(X)
        return c + offset, r, rot

    A_normalized = A / (-const)

    # Eigendecomposition to get radii and rotation
    eigenvalues, eigenvectors = linalg.eigh(A_normalized)

    # Check eigenvalues are all positive
    if np.any(eigenvalues <= 0):
        print(f"\nWARNING: Non-positive eigenvalues: {eigenvalues}")
        print("Falling back to simple axis-aligned method...")
        c, r, rot = fit_ellipsoid_simple(X)
        return c + offset, r, rot

    radii = 1.0 / np.sqrt(eigenvalues)
    rotation = eigenvectors

    # Translate center back to original (uncentered) coordinate frame
    center = center + offset

    return center, radii, rotation


def _refine_horizontal_bias(flat_samples, bias, soft_iron):
    """Correct residual horizontal-plane hard-iron offset using dedicated flat-rotation data.

    The global 3D ellipsoid fit leaves a small center offset in the horizontal
    plane because upper-hemisphere samples dominate the fit. This offset causes
    a sin(θ) heading error (maximum at 90°/270° headings).

    Requires a separate CSV of flat-rotation samples (device lying flat, rotated
    through a full 360° in yaw). These samples form a 2D circle in calibrated X-Y
    space; we correct the bias so that circle is centered at the origin.

    Do NOT try to extract horizontal samples from the spherical calibration CSV —
    the spherical data typically has very few near-horizontal points and they rarely
    cover the full 360° azimuth needed for an accurate min/max center estimate.
    """
    n = len(flat_samples)
    print(f"\n  Horizontal-plane refinement ({n} flat-rotation samples):")

    if n < 50:
        print(f"  Skipped — need ≥50 samples")
        return bias

    # Apply current calibration to all flat samples
    cal = (soft_iron @ (flat_samples - bias).T).T  # (n, 3)

    # 2D circle center in calibrated X-Y (min/max midpoint — valid when all
    # headings are covered, as they should be in a dedicated flat rotation)
    cx = (np.max(cal[:, 0]) + np.min(cal[:, 0])) / 2.0
    cy = (np.max(cal[:, 1]) + np.min(cal[:, 1])) / 2.0

    r_xy = (np.max(cal[:, 0]) - np.min(cal[:, 0])) / 2.0
    offset_dist = np.sqrt(cx**2 + cy**2)
    offset_pct = 100.0 * offset_dist / r_xy if r_xy > 0 else 0.0

    # LSB/µT for LIS3MDL at ±4 Gauss: 6842 LSB/Gauss = 68.42 LSB/µT
    LSB_PER_UT = 68.42
    print(f"  Horizontal circle center: ({cx:.0f}, {cy:.0f}) counts"
          f"  =  ({cx/LSB_PER_UT:.2f}, {cy/LSB_PER_UT:.2f}) µT")
    print(f"  Center offset from origin: {offset_pct:.1f}% of horizontal radius")

    if offset_pct < 1.0:
        print("  Offset < 1% — no correction applied")
        return bias

    # Shift bias so the horizontal circle center lands at (0, 0):
    #   SI @ (raw - new_bias) = SI @ (raw - old_bias) - [cx, cy, 0]
    #   → new_bias = old_bias + SI⁻¹ @ [cx, cy, 0]
    delta = linalg.solve(soft_iron, np.array([cx, cy, 0.0]))
    new_bias = bias + delta
    print(f"  Bias adjustment: dX={delta[0]:+.1f}  dY={delta[1]:+.1f} counts")
    print(f"  Refined bias:    X={new_bias[0]:.2f}  Y={new_bias[1]:.2f}  Z={new_bias[2]:.2f}")
    return new_bias


def compute_calibration(samples, flat_samples=None):
    """
    Compute magnetometer calibration from raw samples.

    Args:
        samples: (N, 3) array of raw magnetometer samples from spherical rotation
        flat_samples: optional (M, 3) array from a dedicated flat-rotation pass
                      (device lying flat, rotated full 360° in yaw). When provided,
                      used to refine the horizontal-plane hard-iron offset, reducing
                      sin(θ) heading error.

    Returns:
        bias: (3,) hard-iron offset in sensor units
        soft_iron: (3, 3) soft-iron correction matrix
        info: dict with diagnostic information
    """
    # Print data statistics
    print(f"\nData statistics:")
    print(f"  X range: [{np.min(samples[:, 0]):.1f}, {np.max(samples[:, 0]):.1f}]")
    print(f"  Y range: [{np.min(samples[:, 1]):.1f}, {np.max(samples[:, 1]):.1f}]")
    print(f"  Z range: [{np.min(samples[:, 2]):.1f}, {np.max(samples[:, 2]):.1f}]")
    print(f"  X span: {np.max(samples[:, 0]) - np.min(samples[:, 0]):.1f}")
    print(f"  Y span: {np.max(samples[:, 1]) - np.min(samples[:, 1]):.1f}")
    print(f"  Z span: {np.max(samples[:, 2]) - np.min(samples[:, 2]):.1f}")

    # Check for sufficient variation
    spans = [np.max(samples[:, i]) - np.min(samples[:, i]) for i in range(3)]
    if any(span < 100 for span in spans):
        print(f"\nWARNING: Insufficient variation in one or more axes!")
        print(f"         Rotation coverage may be incomplete.")
        print(f"         Expected span > 1000, got: X={spans[0]:.0f}, Y={spans[1]:.0f}, Z={spans[2]:.0f}")

    if flat_samples is not None:
        # --- Hybrid approach (flat data provided) ---
        #
        # The 3D ellipsoid fit cannot reliably determine bias_x/bias_y when
        # samples are unevenly distributed in elevation (as they always are
        # with hand-collected data): the fit minimizes sphere residuals across
        # all samples, which pulls the center toward the denser upper-hemisphere
        # data and leaves the horizontal plane off-center.
        #
        # Fix: use each data source for what it's actually good at —
        #   bias_x, bias_y : 2D algebraic circle fit on flat rotation X,Y
        #                    (direct measurement of horizontal circle center;
        #                     immune to elevation-distribution bias)
        #   bias_z         : midpoint of the 3D Z range
        #                    (heading doesn't depend on Z for a flat device)
        #   soft-iron      : PCA of 3D data centered on the flat-derived bias
        #                    (3D data is well-suited for shape/rotation fit)
        #
        # This separates the two error sources and avoids the "3D RMS degrades"
        # issue that occurs when the old _refine_horizontal_bias shifts the
        # bias away from the 3D-optimum after the fact.

        bias_x, bias_y, r_flat = fit_circle_2d(flat_samples[:, :2])
        bias_z = (np.max(samples[:, 2]) + np.min(samples[:, 2])) / 2.0
        bias = np.array([bias_x, bias_y, bias_z])

        LSB_PER_UT = 68.42
        print(f"\n  Bias from 2D circle fit (flat data) + 3D Z midpoint:")
        print(f"  X: {bias_x:8.2f}  ({bias_x/LSB_PER_UT:.2f} uT)  -- from flat rotation")
        print(f"  Y: {bias_y:8.2f}  ({bias_y/LSB_PER_UT:.2f} uT)  -- from flat rotation")
        print(f"  Z: {bias_z:8.2f}  ({bias_z/LSB_PER_UT:.2f} uT)  -- from 3D Z range")
        print(f"  Flat circle radius: {r_flat:.1f} counts ({r_flat/LSB_PER_UT:.2f} uT)")

        # Soft-iron from PCA of 3D data centered on the flat-derived bias.
        # The center shift is small (~100-250 counts) relative to the ellipsoid
        # radius (~3400 counts), so PCA still finds the correct principal axes.
        X_centered = samples - bias
        cov = np.cov(X_centered.T)
        eigenvalues, eigenvectors = linalg.eigh(cov)
        rotation = eigenvectors
        X_aligned = X_centered @ rotation
        radii = (np.max(X_aligned, axis=0) - np.min(X_aligned, axis=0)) / 2.0
        avg_radius = np.mean(radii)
        center = bias  # for diagnostics consistency

    else:
        # --- 3D-only approach (no flat data) ---
        # Fit full ellipsoid to determine both bias and soft-iron from 3D data.
        # NOTE: bias_x/bias_y accuracy depends on having well-balanced elevation
        # coverage. Use the flat-rotation CSV argument for better heading accuracy.
        try:
            center, radii, rotation = fit_ellipsoid_algebraic(samples)
        except Exception as e:
            print(f"\nAlgebraic fit failed: {e}")
            print("Using simple axis-aligned method instead...")
            center, radii, rotation = fit_ellipsoid_simple(samples)

        bias = center
        avg_radius = np.mean(radii)

    # Build soft-iron correction matrix: M = R * diag(avg_r / radii) * R^T
    scale_matrix = np.diag(avg_radius / radii)
    soft_iron = rotation @ scale_matrix @ rotation.T

    # Diagnostics
    info = {
        'center': center,
        'radii': radii,
        'avg_radius': avg_radius,
        'rotation': rotation,
        'num_samples': len(samples),
        'sample_std': np.std(samples, axis=0),
        'radii_variation': np.std(radii) / avg_radius if avg_radius > 0 else float('nan')
    }

    return bias, soft_iron, info


def compute_residuals(samples, bias, soft_iron, avg_radius):
    """
    Apply calibration to all samples and measure how close each lands to the
    reference sphere (radius = avg_radius).

    A perfect calibration maps every raw point to exactly avg_radius distance
    from the origin. Residuals show the per-point error in sensor units.
    """
    centered = samples - bias                          # (N, 3): subtract hard-iron
    calibrated = (soft_iron @ centered.T).T            # (N, 3): apply soft-iron
    magnitudes = np.linalg.norm(calibrated, axis=1)    # (N,): distance from origin
    errors = magnitudes - avg_radius                   # signed error vs reference sphere

    rms = np.sqrt(np.mean(errors**2))
    return {
        'magnitudes': magnitudes,
        'errors': errors,
        'mean_magnitude': np.mean(magnitudes),
        'std_magnitude': np.std(magnitudes),
        'rms_error': rms,
        'max_abs_error': np.max(np.abs(errors)),
        'rms_pct': 100.0 * rms / avg_radius if avg_radius > 0 else float('nan'),
        'max_abs_pct': 100.0 * np.max(np.abs(errors)) / avg_radius if avg_radius > 0 else float('nan'),
    }


def load_csv(filename):
    """Load magnetometer samples from CSV file.

    Both formats return data in **logical frame** (axis map already applied):

    1. Simple format: X,Y,Z raw counts (one sample per line, optional header).
       Written by magBinCalDumpCSV(), which stores readMagRaw() output (post-axis-map).

    2. Nav logger format: full log CSV with many columns. Detected by the presence
       of "mag_x_raw" in the header. Extracts mag_x/y/z_raw (µT), converts to counts.
       mag_x_raw is logged from readMag_raw_cal() which calls readMagRaw() internally,
       so the axis map is already applied — data is in logical frame.

    In both cases the output is in logical frame (post-axis-map counts), which is what
    readMag() expects when applying the stored calibration at runtime.
    """
    with open(filename, 'r') as f:
        header = f.readline().strip()

    header_cols = [c.strip() for c in header.split(',')]

    if 'mag_x_raw' in header_cols:
        # Nav logger format — extract and convert mag columns
        ix = header_cols.index('mag_x_raw')
        iy = header_cols.index('mag_y_raw')
        iz = header_cols.index('mag_z_raw')
        data = np.loadtxt(filename, delimiter=',', skiprows=1, usecols=(ix, iy, iz))
        # mag_x/y/z_raw in µT, logical frame (readMag_raw_cal → readMagRaw → axis map applied)
        mag_ut = data
        # Convert µT → raw logical-frame counts
        LSB_PER_UT = 68.42
        counts = mag_ut * LSB_PER_UT
        print(f"  (nav-log format detected: extracted mag_x/y/z_raw, converted to logical-frame counts)")
        return counts

    try:
        data = np.loadtxt(filename, delimiter=',', skiprows=1)
    except Exception as e:
        print(f"Error loading CSV: {e}")
        print("Trying without header row...")
        data = np.loadtxt(filename, delimiter=',')

    if data.shape[1] < 3:
        raise ValueError(f"CSV must have at least 3 columns (X,Y,Z), got {data.shape[1]}")

    return data[:, :3]  # X, Y, Z columns


def deduplicate(samples):
    """
    Remove exact duplicate samples.

    Duplicates are now rejected at collection time on the device (imu.cpp
    magBinCalTick checks each new sample against the last accepted sample for
    that bin before storing it).  This function is kept as a safety net for
    CSV files collected with older firmware that did not have runtime dedup.
    """
    unique = np.unique(samples, axis=0)
    n_removed = len(samples) - len(unique)
    if n_removed > 0:
        print(f"  [dedup] Removed {n_removed} exact duplicate rows from legacy CSV "
              f"({len(unique)} unique samples remain)")
    return unique


def save_json_calibration(filename, bias, soft_iron):
    """Save calibration to JSON format compatible with ESP32 storage.cpp"""
    cal_data = {
        "type": "MagCalib",
        "bias": {
            "x": float(bias[0]),
            "y": float(bias[1]),
            "z": float(bias[2])
        },
        "softIron": [
            [float(soft_iron[0, 0]), float(soft_iron[0, 1]), float(soft_iron[0, 2])],
            [float(soft_iron[1, 0]), float(soft_iron[1, 1]), float(soft_iron[1, 2])],
            [float(soft_iron[2, 0]), float(soft_iron[2, 1]), float(soft_iron[2, 2])]
        ]
    }

    with open(filename, 'w') as f:
        json.dump(cal_data, f, indent=2)

    print(f"\nSaved calibration to {filename}")


def compute_mounted_correction(mounted_samples, base_bias, base_soft_iron):
    """
    Solve for mounted-cal correction (M_mount, b_mount) from base-corrected residuals.

    Inputs:
        mounted_samples  — (N, 3) raw logical-frame counts from mounted cal CSV
        base_bias        — (3,) b_base from mag_base.json
        base_soft_iron   — (3, 3) M_base from mag_base.json

    Returns:
        b_mount     — (3,) hard-iron offset in base-corrected space (b_z always 0)
        M_mount     — (3, 3) soft-iron correction in base-corrected space (s_z always 1)
        info        — diagnostic dict

    Transform chain: corrected = M_mount * (M_base * (raw - b_base) - b_mount)

    Constrained fit — only b_x, b_y, s_x, s_y are solved:
      b_z = 0, s_z = 1 (locked)
    Mounted cal only covers ±30° pitch, which is insufficient to constrain the Z
    axis reliably.  Solving b_z / s_z from sparse, asymmetric vertical data would
    produce noisy corrections that hurt heading accuracy more than they help.

    Both b_x/b_y and s_x/s_y are regularized toward their priors (0 and 1) via a
    weighted-prior blend: estimate = (raw * n + prior * LAMBDA) / (n + LAMBDA).
    This prevents extreme corrections when the mounted cal dataset is small.

    b_x, b_y are estimated via an algebraic 2D circle fit (more robust than
    min/max midpoint for the heading-critical horizontal axes).
    """
    # Regularization strengths, in "equivalent prior sample counts".
    # At n = 100 samples: bias shrinkage ~33%, scale shrinkage ~33%.
    # At n = 500 samples: shrinkage ~9%.  Adjust if data is routinely sparse/dense.
    LAMBDA_BIAS  = 50.0
    LAMBDA_SCALE = 50.0

    # Apply base calibration to all mounted samples
    base_corrected = (base_soft_iron @ (mounted_samples - base_bias).T).T  # (N, 3)
    n = len(base_corrected)

    # --- Hard-iron offset: b_x, b_y only; b_z locked to 0 ---
    # 2D algebraic circle fit on X,Y is more accurate than min/max midpoint:
    # it's not thrown off by azimuth gaps and handles full or partial coverage.
    b_x_raw, b_y_raw, r_flat = fit_circle_2d(base_corrected[:, :2])

    # Regularize toward 0: b_reg = b_raw * n / (n + LAMBDA)
    b_x = b_x_raw * n / (n + LAMBDA_BIAS)
    b_y = b_y_raw * n / (n + LAMBDA_BIAS)
    b_mount = np.array([b_x, b_y, 0.0])

    # --- Scale correction: s_x, s_y only; s_z locked to 1 ---
    # Solve A*u^2 + C*v^2 = 1 with known centre (2-parameter algebraic LS fit).
    # More robust than per-axis RMS: RMS is biased when samples cluster near
    # 0°/180° vs 90°/270° (common in hand-collected mounted-cal data because
    # operators tend to hold one heading longer while turning around).
    # The algebraic fit uses all samples optimally via normal equations.
    centered = base_corrected - b_mount
    u = centered[:, 0]
    v = centered[:, 1]
    M22 = np.array([
        [np.sum(u**4),        np.sum(u**2 * v**2)],
        [np.sum(u**2 * v**2), np.sum(v**4)       ],
    ])
    rhs22 = np.array([np.sum(u**2), np.sum(v**2)])
    scale_method = "algebraic"
    rx = ry = 1.0  # fallback values
    try:
        Ac = np.linalg.solve(M22, rhs22)  # [A, C]
        if Ac[0] > 0 and Ac[1] > 0:
            rx = 1.0 / np.sqrt(Ac[0])
            ry = 1.0 / np.sqrt(Ac[1])
        else:
            raise ValueError("non-positive semi-axis")
    except Exception:
        # Fallback: per-axis RMS (less accurate for skewed distributions)
        rms_xy = np.sqrt(np.mean(centered[:, :2]**2, axis=0))
        rx, ry = rms_xy[0], rms_xy[1]
        scale_method = "RMS (algebraic fallback)"
        print("  WARNING: algebraic scale fit failed — using per-axis RMS fallback")

    avg_r_fit = (rx + ry) / 2.0
    s_xy_raw = np.array([avg_r_fit / rx, avg_r_fit / ry])

    # Regularize toward 1: s_reg = (s_raw * n + 1.0 * LAMBDA) / (n + LAMBDA)
    s_xy_reg = (s_xy_raw * n + LAMBDA_SCALE) / (n + LAMBDA_SCALE)
    scale = np.array([s_xy_reg[0], s_xy_reg[1], 1.0])
    M_mount = np.diag(scale)

    # Heading error estimate from XY ellipticity (before correction)
    diff_rc = abs(rx - ry)
    hdg_err_raw = float(np.degrees(np.arctan2(diff_rc, avg_r_fit))) if avg_r_fit > 0 else 0.0

    # Diagnostics (residuals after applying the full correction)
    corrected_final = (M_mount @ centered.T).T
    magnitudes = np.linalg.norm(corrected_final, axis=1)
    avg_r = np.mean(magnitudes)
    errors = magnitudes - avg_r
    rms_err = np.sqrt(np.mean(errors**2))

    info = {
        'n_samples': n,
        'b_mount': b_mount,
        'b_raw': np.array([b_x_raw, b_y_raw, 0.0]),
        'scale': scale,
        'scale_raw': np.array([s_xy_raw[0], s_xy_raw[1], 1.0]),
        'r_flat': r_flat,
        'rx': rx, 'ry': ry,
        'hdg_err_raw_deg': hdg_err_raw,
        'scale_method': scale_method,
        'avg_radius': avg_r,
        'rms_error': rms_err,
        'rms_pct': 100.0 * rms_err / avg_r if avg_r > 0 else float('nan'),
    }

    return b_mount, M_mount, info


def load_json_calibration(filename):
    """Load MagCalib JSON produced by save_json_calibration()."""
    with open(filename, 'r') as f:
        d = json.load(f)
    bias = np.array([d['bias']['x'], d['bias']['y'], d['bias']['z']])
    si = d['softIron']
    soft_iron = np.array([[si[r][c] for c in range(3)] for r in range(3)])
    return bias, soft_iron


def run_baseline(args):
    """Handle --mode baseline: full ellipsoid fit → mag_base.json"""
    input_file = args.input
    flat_file   = args.flat
    output_file = args.output or "mag_base.json"

    print(f"Loading samples from {input_file}...")
    samples = load_csv(input_file)
    print(f"Loaded {len(samples)} samples")

    samples = deduplicate(samples)

    if len(samples) < 100:
        print("WARNING: Less than 100 samples. Recommend at least 500 for accurate calibration.")

    flat_samples = None
    if flat_file:
        print(f"\nLoading flat-rotation samples from {flat_file}...")
        flat_samples = load_csv(flat_file)
        flat_samples = deduplicate(flat_samples)
        print(f"Loaded {len(flat_samples)} flat-rotation samples")

    print("\nFitting ellipsoid...")
    bias, soft_iron, info = compute_calibration(samples, flat_samples=flat_samples)

    print("\n" + "="*60)
    print("BASELINE CALIBRATION RESULTS")
    print("="*60)

    print(f"\nHard-Iron Offset (bias):")
    print(f"  X: {bias[0]:8.2f}")
    print(f"  Y: {bias[1]:8.2f}")
    print(f"  Z: {bias[2]:8.2f}")

    print(f"\nSoft-Iron Correction Matrix:")
    for i in range(3):
        print(f"  [{soft_iron[i, 0]:8.6f}, {soft_iron[i, 1]:8.6f}, {soft_iron[i, 2]:8.6f}]")

    print(f"\nDiagnostics:")
    print(f"  Samples: {info['num_samples']}")
    print(f"  Ellipsoid center: [{info['center'][0]:.2f}, {info['center'][1]:.2f}, {info['center'][2]:.2f}]")
    print(f"  Ellipsoid radii:  [{info['radii'][0]:.2f}, {info['radii'][1]:.2f}, {info['radii'][2]:.2f}]")
    print(f"  Average radius:   {info['avg_radius']:.2f}")

    if not np.isnan(info['radii_variation']):
        print(f"  Radii variation: {info['radii_variation']*100:.2f}%")
        if info['radii_variation'] > 0.1:
            print(f"  WARNING: High variation — rotate through more orientations.")

    residuals = compute_residuals(samples, bias, soft_iron, info['avg_radius'])
    print(f"\nResiduals:")
    print(f"  RMS error:  {residuals['rms_error']:.2f}  ({residuals['rms_pct']:.2f}%)")
    print(f"  Max |error|: {residuals['max_abs_error']:.2f}  ({residuals['max_abs_pct']:.2f}%)")
    if residuals['rms_pct'] < 2.0:
        print(f"  Quality: GOOD (<2% RMS)")
    elif residuals['rms_pct'] < 5.0:
        print(f"  Quality: ACCEPTABLE (2-5% RMS)")
    else:
        print(f"  Quality: POOR (>5% RMS) — recollect with fuller sphere coverage")

    if flat_samples is not None:
        print("\n" + "="*60)
        print("HEADING ACCURACY ANALYSIS (flat device)")
        print("="*60)
        LSB_PER_UT = 68.42
        cal_flat = (soft_iron @ (flat_samples - bias).T).T
        cx_cal = (np.max(cal_flat[:, 0]) + np.min(cal_flat[:, 0])) / 2.0
        cy_cal = (np.max(cal_flat[:, 1]) + np.min(cal_flat[:, 1])) / 2.0
        r_x = (np.max(cal_flat[:, 0]) - np.min(cal_flat[:, 0])) / 2.0
        r_y = (np.max(cal_flat[:, 1]) - np.min(cal_flat[:, 1])) / 2.0
        r_avg_2d = (r_x + r_y) / 2.0
        center_offset = np.sqrt(cx_cal**2 + cy_cal**2)
        roundness = min(r_x, r_y) / max(r_x, r_y) if max(r_x, r_y) > 0 else 0.0
        heading_err_offset = (np.degrees(np.arcsin(min(1.0, center_offset / r_avg_2d)))
                              if r_avg_2d > 0 else 0.0)
        ellipticity_err = (np.degrees(np.arctan(abs(r_x - r_y) / r_avg_2d))
                           if r_avg_2d > 0 else 0.0)
        total_err = heading_err_offset + ellipticity_err
        print(f"\n  Calibrated flat circle:")
        print(f"  Center: ({cx_cal:+.1f} / {cx_cal/LSB_PER_UT:+.3f} uT,  "
              f"{cy_cal:+.1f} / {cy_cal/LSB_PER_UT:+.3f} uT)")
        print(f"  Radius: X={r_x:.1f}  Y={r_y:.1f}   Roundness={roundness:.3f}")
        print(f"\n  Heading accuracy estimate:")
        print(f"  From circle offset:  +/-{heading_err_offset:.1f} deg")
        print(f"  From ellipticity:    +/-{ellipticity_err:.1f} deg")
        print(f"  Total (worst case):  +/-{total_err:.1f} deg")
        if total_err < 2.0:
            print(f"  Rating: EXCELLENT")
        elif total_err < 5.0:
            print(f"  Rating: GOOD")
        else:
            print(f"  Rating: POOR — recollect flat data or more sphere coverage")

    print("\n" + "="*60)
    save_json_calibration(output_file, bias, soft_iron)
    print(f"\nNext steps:")
    print(f"  1. Upload {output_file} to device as /calib/mag_base.json")
    print(f"  2. Mount unit on scooter, run 'Mounted cal' from menu")
    print(f"  3. Download /mag_mounted_samples.csv, run:")
    print(f"     python mag_calibration.py --mode mounted --base {output_file} <mounted.csv>")


def run_mounted(args):
    """Handle --mode mounted: solve M_mount/b_mount correction → mag_mount.json"""
    if not args.base:
        print("ERROR: --mode mounted requires --base <mag_base.json>")
        sys.exit(1)
    if not args.input:
        print("ERROR: --mode mounted requires <mounted_samples.csv>")
        sys.exit(1)

    output_file = args.output or "mag_mount.json"

    print(f"Loading base calibration from {args.base}...")
    base_bias, base_soft_iron = load_json_calibration(args.base)
    print(f"  b_base: X={base_bias[0]:.2f}  Y={base_bias[1]:.2f}  Z={base_bias[2]:.2f}")

    print(f"\nLoading mounted cal samples from {args.input}...")
    raw_samples = load_csv(args.input)
    raw_samples = deduplicate(raw_samples)
    print(f"  {len(raw_samples)} samples after dedup")

    if len(raw_samples) < 50:
        print("WARNING: Very few samples — mounted cal may be inaccurate.")

    print("\nSolving mounted correction (diagonal soft-iron, full hard-iron)...")
    b_mount, M_mount, info = compute_mounted_correction(raw_samples, base_bias, base_soft_iron)

    print("\n" + "="*60)
    print("MOUNTED CALIBRATION CORRECTION RESULTS")
    print("="*60)
    print(f"\nMounted hard-iron offset (b_mount, in base-corrected space):")
    print(f"  X: {b_mount[0]:+8.4f}  (raw: {info['b_raw'][0]:+.4f})")
    print(f"  Y: {b_mount[1]:+8.4f}  (raw: {info['b_raw'][1]:+.4f})")
    print(f"  Z:     0.0000  (locked — insufficient vertical coverage to constrain)")
    print(f"\nMounted soft-iron correction (M_mount, diagonal):")
    for i in range(3):
        print(f"  [{M_mount[i, 0]:8.6f}, {M_mount[i, 1]:8.6f}, {M_mount[i, 2]:8.6f}]")
    LSB_PER_UT = 68.42
    print(f"\nDiagnostics:")
    print(f"  Samples: {info['n_samples']}  |  Scale fit method: {info['scale_method']}")
    print(f"  Flat circle radius (base-corrected): {info['r_flat']:.2f} counts  ({info['r_flat']/LSB_PER_UT:.2f} µT)")
    print(f"  XY semi-axes (algebraic):  rx={info['rx']:.2f}  ry={info['ry']:.2f}  ratio={info['ry']/info['rx']:.3f}")
    print(f"  Ellipticity heading error (pre-correction): ±{info['hdg_err_raw_deg']:.1f}°")
    print(f"  Per-axis scale (raw):        X={info['scale_raw'][0]:.4f}  Y={info['scale_raw'][1]:.4f}  Z=1.0000 (locked)")
    print(f"  Per-axis scale (regularized): X={info['scale'][0]:.4f}  Y={info['scale'][1]:.4f}  Z=1.0000 (locked)")
    print(f"  RMS error (after correction): {info['rms_error']:.2f}  ({info['rms_pct']:.2f}%)")

    # Sanity check: if M_mount is close to identity, mounting effect is small
    deviation = np.max(np.abs(info['scale'][:2] - 1.0))
    if deviation < 0.02:
        print(f"\n  NOTE: Scale correction < 2% — scooter's soft-iron effect is small.")
        print(f"        Mounted cal primarily provides b_mount (hard-iron) correction.")
    elif deviation > 0.15:
        hdg_e = info['hdg_err_raw_deg']
        print(f"\n  NOTE: Scale correction > 15% (ellipticity ±{hdg_e:.1f}°).")
        print(f"        This level of XY asymmetry is plausible for a DPV with strong")
        print(f"        motor/battery fields.  Verify by checking heading accuracy after upload.")

    print("\n" + "="*60)
    save_json_calibration(output_file, b_mount, M_mount)
    print(f"\nNext steps:")
    print(f"  1. Upload {output_file} to device as /calib/mag_mount.json")
    print(f"  2. Reboot device — it will apply the full base+mount correction chain")
    print(f"     corrected = M_mount * (M_base * (raw - b_base) - b_mount)")


def main():
    import argparse

    parser = argparse.ArgumentParser(
        description="Magnetometer calibration tool for DPV-Nav.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Modes:
  baseline  Full ellipsoid fit for off-scooter calibration → mag_base.json
            Usage: python mag_calibration.py --mode baseline <samples.csv> [--flat flat.csv]

  mounted   Solve correction from base-corrected residuals → mag_mount.json
            Usage: python mag_calibration.py --mode mounted --base mag_base.json <mounted.csv>

Transform chain applied on device:
  corrected = M_mount * (M_base * (raw - b_base) - b_mount)
""")
    parser.add_argument('input', nargs='?', help='Input CSV file (samples)')
    parser.add_argument('--mode', choices=['baseline', 'mounted'], default='baseline',
                        help='Calibration mode (default: baseline)')
    parser.add_argument('--flat', metavar='FLAT_CSV',
                        help='(baseline mode) Flat-rotation CSV for horizontal bias refinement')
    parser.add_argument('--base', metavar='BASE_JSON',
                        help='(mounted mode) mag_base.json from previous baseline cal')
    parser.add_argument('--output', '-o', metavar='OUTPUT_JSON',
                        help='Output JSON filename (default: mag_base.json or mag_mount.json)')

    args = parser.parse_args()

    if not args.input:
        parser.print_help()
        sys.exit(1)

    if args.mode == 'baseline':
        run_baseline(args)
    elif args.mode == 'mounted':
        run_mounted(args)


if __name__ == "__main__":
    main()
