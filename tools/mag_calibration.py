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

    # Step 7: For soft-iron, compute covariance to get rotation
    cov = np.cov(X_centered.T)
    eigenvalues, eigenvectors = linalg.eigh(cov)

    # Eigenvectors give rotation matrix
    rotation = eigenvectors

    # Radii from eigenvalues of covariance (proportional to variance)
    radii_from_cov = np.sqrt(eigenvalues) * 2.0  # *2 for full axis length

    return center, radii_from_cov, rotation


def fit_ellipsoid_algebraic(X):
    """
    Algebraic ellipsoid fitting with robustness checks.

    Returns:
        center: (3,) hard-iron offset
        radii: (3,) ellipsoid radii
        rotation: (3, 3) rotation matrix
    """
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

    if np.any(eigvals_A <= 0):
        print(f"\nWARNING: Algebraic fit produced non-ellipsoid (eigenvalues: {eigvals_A})")
        print("Falling back to simple axis-aligned method...")
        return fit_ellipsoid_simple(X)

    # Solve for center: A * center = -b
    try:
        center = -linalg.solve(A, b)
    except linalg.LinAlgError:
        print("\nWARNING: Singular matrix in center calculation")
        print("Falling back to simple axis-aligned method...")
        return fit_ellipsoid_simple(X)

    # Translate to center
    # Ellipsoid equation at center: (x-c)^T A (x-c) + constant = 0
    # constant = c^T A c - b^T c + d
    const = center.T @ A @ center - b.T @ center + d

    # Normalize: (x-c)^T A (x-c) = -constant
    # For proper ellipsoid, -constant must be positive
    if -const <= 0:
        print(f"\nWARNING: Invalid ellipsoid constant: {const}")
        print("Falling back to simple axis-aligned method...")
        return fit_ellipsoid_simple(X)

    A_normalized = A / (-const)

    # Eigendecomposition to get radii and rotation
    eigenvalues, eigenvectors = linalg.eigh(A_normalized)

    # Check eigenvalues are all positive
    if np.any(eigenvalues <= 0):
        print(f"\nWARNING: Non-positive eigenvalues: {eigenvalues}")
        print("Falling back to simple axis-aligned method...")
        return fit_ellipsoid_simple(X)

    radii = 1.0 / np.sqrt(eigenvalues)
    rotation = eigenvectors

    return center, radii, rotation


def compute_calibration(samples):
    """
    Compute magnetometer calibration from raw samples.

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

    # Try algebraic fit first
    try:
        center, radii, rotation = fit_ellipsoid_algebraic(samples)
    except Exception as e:
        print(f"\nAlgebraic fit failed: {e}")
        print("Using simple axis-aligned method instead...")
        center, radii, rotation = fit_ellipsoid_simple(samples)

    # Hard-iron offset is the ellipsoid center
    bias = center

    # Soft-iron correction matrix transforms ellipsoid to sphere
    # Scale each principal axis to have the same radius (average)
    avg_radius = np.mean(radii)

    # Build correction matrix: M = R * S * R^T
    # where S is diagonal scaling matrix, R is rotation to principal axes
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


def load_csv(filename):
    """Load magnetometer samples from CSV file."""
    try:
        data = np.loadtxt(filename, delimiter=',', skiprows=1)
    except Exception as e:
        print(f"Error loading CSV: {e}")
        print("Trying without header row...")
        data = np.loadtxt(filename, delimiter=',')

    if data.shape[1] < 3:
        raise ValueError(f"CSV must have at least 3 columns (X,Y,Z), got {data.shape[1]}")

    return data[:, :3]  # X, Y, Z columns


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
        print("Usage: python mag_calibration.py <mag_samples.csv>")
        print("\nCSV format: X,Y,Z (raw magnetometer readings, one sample per line)")
        sys.exit(1)

    input_file = sys.argv[1]
    output_file = "calib_mag_cal.json"

    print(f"Loading samples from {input_file}...")
    samples = load_csv(input_file)
    print(f"Loaded {len(samples)} samples")

    if len(samples) < 100:
        print("WARNING: Less than 100 samples. Recommend at least 500 samples for accurate calibration.")

    print("\nFitting ellipsoid...")
    bias, soft_iron, info = compute_calibration(samples)

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

    print("\n" + "="*60)

    # Save to JSON
    save_json_calibration(output_file, bias, soft_iron)

    print(f"\nNext steps:")
    print(f"  1. Upload {output_file} to ESP32 SPIFFS as /calib_mag_cal.json")
    print(f"  2. Or copy-paste the values into firmware/src/main.cpp")
    print(f"  3. Reboot ESP32 to load new calibration")


if __name__ == "__main__":
    main()
