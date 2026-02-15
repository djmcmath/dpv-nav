#!/usr/bin/env python3
"""
Magnetometer Coverage Checker
Analyzes raw magnetometer samples to identify gaps in rotation coverage.

Usage:
    python mag_coverage_check.py <mag_samples.csv>

Output:
    - Text-based spherical coverage map
    - Identifies under-sampled regions
    - Recommends specific rotations to improve coverage
"""

import numpy as np
import sys

def cart_to_spherical(x, y, z):
    """Convert Cartesian coordinates to spherical (r, theta, phi)."""
    r = np.sqrt(x*x + y*y + z*z)
    theta = np.arctan2(y, x)  # Azimuth: -pi to pi
    phi = np.arctan2(np.sqrt(x*x + y*y), z)  # Polar: 0 to pi
    return r, theta, phi

def analyze_coverage(samples, center):
    """
    Analyze rotation coverage by binning samples on sphere.

    Returns:
        coverage_map: 2D array showing sample density (theta vs phi)
        bins_theta: Theta bin edges
        bins_phi: Phi bin edges
    """
    # Center the data
    centered = samples - center

    # Convert to spherical coordinates
    x, y, z = centered[:, 0], centered[:, 1], centered[:, 2]
    r, theta, phi = cart_to_spherical(x, y, z)

    # Create bins for theta (azimuth: -180° to 180°) and phi (polar: 0° to 180°)
    n_theta_bins = 12  # 30° increments
    n_phi_bins = 6     # 30° increments

    theta_bins = np.linspace(-np.pi, np.pi, n_theta_bins + 1)
    phi_bins = np.linspace(0, np.pi, n_phi_bins + 1)

    # Bin the samples
    coverage_map, _, _ = np.histogram2d(theta, phi, bins=[theta_bins, phi_bins])

    return coverage_map, theta_bins, phi_bins

def print_coverage_map(coverage_map, theta_bins, phi_bins):
    """Print ASCII coverage map with color coding."""
    print("\n" + "="*70)
    print("SPHERICAL COVERAGE MAP")
    print("="*70)
    print("\nEach cell shows sample count in that orientation region:")
    print("  • = 0-10 samples (POOR - missing coverage)")
    print("  ░ = 10-30 samples (LOW - needs more)")
    print("  ▒ = 30-60 samples (OK)")
    print("  ▓ = 60-100 samples (GOOD)")
    print("  █ = 100+ samples (EXCELLENT)")
    print("\nRows = Polar angle (0° = North pole, 90° = Equator, 180° = South pole)")
    print("Cols = Azimuth angle (-180° to 180°, W → N → E → S → W)")
    print()

    # Theta labels (azimuth)
    theta_labels = ["-180", "-150", "-120", " -90", " -60", " -30", "   0", "  30", "  60", "  90", " 120", " 150"]
    print("      ", end="")
    for label in theta_labels:
        print(f"{label:>5}", end=" ")
    print()

    # Coverage map
    for i in range(len(phi_bins) - 1):
        phi_deg = np.degrees(phi_bins[i])
        print(f"{phi_deg:5.0f}°", end=" ")

        for j in range(len(theta_bins) - 1):
            count = int(coverage_map[j, i])

            if count == 0:
                symbol = "•"
            elif count < 10:
                symbol = "•"
            elif count < 30:
                symbol = "░"
            elif count < 60:
                symbol = "▒"
            elif count < 100:
                symbol = "▓"
            else:
                symbol = "█"

            print(f"{symbol:>5}", end=" ")
        print()

    print("\n" + "="*70)

def identify_gaps(coverage_map, theta_bins, phi_bins, threshold=10):
    """Identify under-sampled regions and recommend rotations."""
    gaps = []

    for i in range(len(phi_bins) - 1):
        for j in range(len(theta_bins) - 1):
            if coverage_map[j, i] < threshold:
                theta_mid = (theta_bins[j] + theta_bins[j+1]) / 2.0
                phi_mid = (phi_bins[i] + phi_bins[i+1]) / 2.0

                theta_deg = np.degrees(theta_mid)
                phi_deg = np.degrees(phi_mid)

                gaps.append((theta_deg, phi_deg, int(coverage_map[j, i])))

    return gaps

def orientation_description(theta_deg, phi_deg):
    """Convert spherical angles to human-readable orientation."""
    # Phi (polar angle): 0° = up, 90° = horizontal, 180° = down
    if phi_deg < 30:
        tilt = "nose UP (steep climb)"
    elif phi_deg < 60:
        tilt = "nose up (moderate climb)"
    elif phi_deg < 120:
        tilt = "level"
    elif phi_deg < 150:
        tilt = "nose down (moderate dive)"
    else:
        tilt = "nose DOWN (steep dive)"

    # Theta (azimuth): 0° = North, 90° = East, -90° = West, 180° = South
    if -22.5 <= theta_deg < 22.5:
        heading = "North"
    elif 22.5 <= theta_deg < 67.5:
        heading = "Northeast"
    elif 67.5 <= theta_deg < 112.5:
        heading = "East"
    elif 112.5 <= theta_deg < 157.5:
        heading = "Southeast"
    elif 157.5 <= theta_deg <= 180 or -180 <= theta_deg < -157.5:
        heading = "South"
    elif -157.5 <= theta_deg < -112.5:
        heading = "Southwest"
    elif -112.5 <= theta_deg < -67.5:
        heading = "West"
    else:
        heading = "Northwest"

    return f"{heading}, {tilt}"

def main():
    if len(sys.argv) < 2:
        print("Usage: python mag_coverage_check.py <mag_samples.csv>")
        sys.exit(1)

    filename = sys.argv[1]

    # Load data
    print(f"Loading samples from {filename}...")
    try:
        data = np.loadtxt(filename, delimiter=',', skiprows=1)
    except:
        data = np.loadtxt(filename, delimiter=',')

    samples = data[:, :3]
    print(f"Loaded {len(samples)} samples")

    # Compute approximate center
    center = np.mean(samples, axis=0)
    print(f"Estimated center: [{center[0]:.1f}, {center[1]:.1f}, {center[2]:.1f}]")

    # Analyze coverage
    print("\nAnalyzing rotation coverage...")
    coverage_map, theta_bins, phi_bins = analyze_coverage(samples, center)

    # Print coverage map
    print_coverage_map(coverage_map, theta_bins, phi_bins)

    # Identify gaps
    gaps = identify_gaps(coverage_map, theta_bins, phi_bins, threshold=20)

    if gaps:
        print("\nUNDER-SAMPLED ORIENTATIONS (< 20 samples):")
        print("="*70)
        gaps_sorted = sorted(gaps, key=lambda x: x[2])  # Sort by sample count

        for theta_deg, phi_deg, count in gaps_sorted[:10]:  # Show worst 10
            desc = orientation_description(theta_deg, phi_deg)
            print(f"  [{count:3d} samples] {desc}")

        if len(gaps) > 10:
            print(f"\n  ... and {len(gaps) - 10} more under-sampled regions")

        print("\n" + "="*70)
        print("RECOMMENDATIONS:")
        print("  1. Re-run calibration with emphasis on under-sampled orientations above")
        print("  2. Rotate SLOWLY through all pitch angles (0° to 180°)")
        print("  3. At each pitch, do a complete 360° rotation (all headings)")
        print("  4. Aim for 50+ samples in every cell of the coverage map")
        print("="*70)
    else:
        print("\nCOVERAGE LOOKS GOOD!")
        print("All orientation bins have sufficient samples.")
        print("If you're still getting poor calibration, the issue is likely:")
        print("  - Strong magnetic distortion (soft-iron)")
        print("  - Noisy sensors")
        print("  - Time-varying magnetic fields (motors, electronics)")

    # Statistics
    total_bins = coverage_map.size
    empty_bins = np.sum(coverage_map == 0)
    low_bins = np.sum((coverage_map > 0) & (coverage_map < 20))
    good_bins = np.sum(coverage_map >= 50)

    print(f"\nCOVERAGE STATISTICS:")
    print(f"  Total orientation bins: {total_bins}")
    print(f"  Empty bins (0 samples): {empty_bins} ({empty_bins/total_bins*100:.1f}%)")
    print(f"  Low coverage bins (< 20): {low_bins} ({low_bins/total_bins*100:.1f}%)")
    print(f"  Good coverage bins (≥ 50): {good_bins} ({good_bins/total_bins*100:.1f}%)")
    print(f"  Coverage quality: {100 - (empty_bins + low_bins)/total_bins*100:.1f}%")

if __name__ == "__main__":
    main()
