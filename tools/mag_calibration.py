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

    Accepts two formats:
    1. Simple format: X,Y,Z raw counts (one sample per line, optional header)
    2. Nav logger format: full log CSV with many columns. Detected by the presence
       of "mag_x_raw" in the header. Extracts mag_x/y/z_raw (µT), converts to raw
       counts (×68.42 LSB/µT), and undoes the axis map (negate Y, since magMap.y=-2)
       to put data in the same frame as simple-format calibration CSVs.
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
        mag_ut = data  # µT, raw sensor values (no axis map applied in logger)
        # Convert µT → raw sensor counts (same frame as mag_cal_collect.cpp readMagRaw)
        LSB_PER_UT = 68.42
        counts = mag_ut * LSB_PER_UT
        print(f"  (nav-log format detected: extracted mag_x/y/z_raw, converted to raw counts)")
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
    Remove consecutive and exact duplicate samples.

    The ESP32 logs at 100 Hz while the device may be stationary, producing
    runs of 8-10 identical rows per orientation. These duplicates bias the
    ellipsoid fit toward heavily-sampled orientations without adding geometric
    information. Removing them gives each distinct orientation equal weight.
    """
    unique = np.unique(samples, axis=0)
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


def main():
    if len(sys.argv) < 2:
        print("Usage: python mag_calibration.py <spherical.csv> [flat_rotation.csv]")
        print("\nspherical.csv  — samples from full 3D rotation (all orientations)")
        print("flat_rotation.csv — optional: samples from flat 360° yaw rotation only")
        print("                    when provided, refines horizontal-plane bias offset")
        print("\nCSV format: X,Y,Z (raw magnetometer readings, one sample per line)")
        sys.exit(1)

    input_file = sys.argv[1]
    flat_file = sys.argv[2] if len(sys.argv) >= 3 else None
    output_file = "mag_cal.json"

    print(f"Loading samples from {input_file}...")
    samples = load_csv(input_file)
    print(f"Loaded {len(samples)} samples")

    samples_dedup = deduplicate(samples)
    n_removed = len(samples) - len(samples_dedup)
    if n_removed > 0:
        print(f"De-duplicated: removed {n_removed} exact duplicate rows "
              f"({len(samples_dedup)} unique locations remain)")
        samples = samples_dedup

    if len(samples) < 100:
        print("WARNING: Less than 100 samples. Recommend at least 500 samples for accurate calibration.")

    flat_samples = None
    if flat_file is not None:
        print(f"\nLoading flat-rotation samples from {flat_file}...")
        flat_samples = load_csv(flat_file)
        flat_dedup = deduplicate(flat_samples)
        n_flat_removed = len(flat_samples) - len(flat_dedup)
        if n_flat_removed > 0:
            print(f"De-duplicated: removed {n_flat_removed} rows "
                  f"({len(flat_dedup)} unique locations remain)")
            flat_samples = flat_dedup
        print(f"Loaded {len(flat_samples)} flat-rotation samples")

    print("\nFitting ellipsoid...")
    bias, soft_iron, info = compute_calibration(samples, flat_samples=flat_samples)

    print("\n" + "="*60)
    print("MAGNETOMETER CALIBRATION RESULTS")
    print("="*60)

    print(f"\nHard-Iron Offset (bias):")
    print(f"  X: {bias[0]:8.2f}")
    print(f"  Y: {bias[1]:8.2f}")
    print(f"  Z: {bias[2]:8.2f}")

    print(f"\nSoft-Iron Correction Matrix:")
    for i in range(3):
        print(f"  [{soft_iron[i, 0]:8.6f}, {soft_iron[i, 1]:8.6f}, {soft_iron[i, 2]:8.6f}]")

    print(f"\nDiagnostics:")
    print(f"  Number of samples: {info['num_samples']}")
    print(f"  Ellipsoid center: [{info['center'][0]:.2f}, {info['center'][1]:.2f}, {info['center'][2]:.2f}]")
    print(f"  Ellipsoid radii: [{info['radii'][0]:.2f}, {info['radii'][1]:.2f}, {info['radii'][2]:.2f}]")
    print(f"  Average radius: {info['avg_radius']:.2f}")

    if np.isnan(info['radii_variation']):
        print(f"  Radii variation: N/A (calculation failed)")
    else:
        print(f"  Radii variation: {info['radii_variation']*100:.2f}%")

    if not np.isnan(info['radii_variation']) and info['radii_variation'] > 0.1:
        print(f"\n  WARNING: High radii variation ({info['radii_variation']*100:.1f}%) suggests poor sample distribution.")
        print(f"           Rotate device through ALL orientations during calibration.")

    # Residual analysis
    residuals = compute_residuals(samples, bias, soft_iron, info['avg_radius'])
    print(f"\nResiduals (calibrated point distance from reference sphere):")
    print(f"  Reference radius:  {info['avg_radius']:.2f}")
    print(f"  Mean magnitude:    {residuals['mean_magnitude']:.2f}")
    print(f"  Std of magnitude:  {residuals['std_magnitude']:.2f}")
    print(f"  RMS error:         {residuals['rms_error']:.2f}  ({residuals['rms_pct']:.2f}%)")
    print(f"  Max |error|:       {residuals['max_abs_error']:.2f}  ({residuals['max_abs_pct']:.2f}%)")
    if residuals['rms_pct'] < 2.0:
        print(f"  Quality:           GOOD (<2% RMS)")
    elif residuals['rms_pct'] < 5.0:
        print(f"  Quality:           ACCEPTABLE (2-5% RMS) — usable but more coverage helps")
    else:
        print(f"  Quality:           POOR (>5% RMS) — recollect with fuller sphere coverage")

    # Heading accuracy analysis (requires flat rotation data)
    if flat_samples is not None:
        print("\n" + "="*60)
        print("HEADING ACCURACY ANALYSIS (flat device)")
        print("="*60)

        LSB_PER_UT = 68.42

        # Azimuth coverage of flat data (12 x 30-degree sectors)
        centered_flat = flat_samples[:, :2] - np.array([bias[0], bias[1]])
        azimuths = np.degrees(np.arctan2(centered_flat[:, 1], centered_flat[:, 0])) % 360.0
        n_bins = 12
        bin_counts = np.zeros(n_bins, dtype=int)
        for az in azimuths:
            bin_counts[int(az / (360.0 / n_bins)) % n_bins] += 1
        covered_bins = np.sum(bin_counts > 0)
        print(f"\n  Flat rotation azimuth coverage: {covered_bins}/{n_bins} sectors "
              f"({100 * covered_bins // n_bins}%)")
        if covered_bins < n_bins:
            missing = [f"{i*30}-{i*30+30}deg" for i in range(n_bins) if bin_counts[i] == 0]
            print(f"  Missing sectors: {', '.join(missing)}")

        # Apply calibration to flat samples and measure calibrated circle
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

        print(f"\n  Calibrated flat circle (counts / uT):")
        print(f"  Center: ({cx_cal:+.1f} / {cx_cal/LSB_PER_UT:+.3f} uT,  "
              f"{cy_cal:+.1f} / {cy_cal/LSB_PER_UT:+.3f} uT)  (ideal: 0, 0)")
        print(f"  Radius: X={r_x:.1f} ({r_x/LSB_PER_UT:.2f} uT)  "
              f"Y={r_y:.1f} ({r_y/LSB_PER_UT:.2f} uT)")
        print(f"  Roundness: {roundness:.3f}  (ideal: 1.000)")

        print(f"\n  Heading accuracy estimate:")
        print(f"  From circle offset:  +/-{heading_err_offset:.1f} deg max")
        print(f"  From ellipticity:    +/-{ellipticity_err:.1f} deg max")
        print(f"  Total (worst case):  +/-{total_err:.1f} deg")
        if total_err < 2.0:
            print(f"  Rating: EXCELLENT (<2 deg expected error)")
        elif total_err < 5.0:
            print(f"  Rating: GOOD (2-5 deg expected error)")
        else:
            print(f"  Rating: POOR (>5 deg) -- recollect flat data or more sphere coverage")

    print("\n" + "="*60)

    # Save to JSON
    save_json_calibration(output_file, bias, soft_iron)

    print(f"\nNext steps:")
    print(f"  1. Copy {output_file} to data/ in the project root")
    print(f"  2. Run: pio run -e nav -t uploadfs")
    print(f"     WARNING: uploadfs reformats LittleFS — gyro/accel cal files will be wiped.")
    print(f"     Re-run accel/gyro calibration after uploading, or copy those files into data/ first.")
    print(f"  3. Reboot ESP32 — it will load /mag_cal.json automatically")


if __name__ == "__main__":
    main()
