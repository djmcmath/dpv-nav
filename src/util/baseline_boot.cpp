#include "baseline_boot.h"

#include <math.h>
#include <string.h>

namespace baseline_boot {

namespace {

// Normalise to ~unit range before squaring, same idea as imu.cpp's FIT_NORM.
constexpr double kNorm = 4096.0;

float dist3(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Solves the 4x4 system M x = rhs in place (Gauss-Jordan, partial pivoting).
bool solve4(double M[4][5]) {
    for (int col = 0; col < 4; col++) {
        int pivot = col;
        for (int row = col + 1; row < 4; row++)
            if (fabs(M[row][col]) > fabs(M[pivot][col])) pivot = row;
        if (fabs(M[pivot][col]) < 1e-12) return false;
        if (pivot != col)
            for (int j = 0; j < 5; j++) { const double t = M[col][j]; M[col][j] = M[pivot][j]; M[pivot][j] = t; }
        const double inv = 1.0 / M[col][col];
        for (int j = col; j < 5; j++) M[col][j] *= inv;
        for (int row = 0; row < 4; row++) {
            if (row == col) continue;
            const double f = M[row][col];
            for (int j = col; j < 5; j++) M[row][j] -= f * M[col][j];
        }
    }
    return true;
}

// Smallest eigenvalue of a symmetric 3x3 matrix, closed form (trigonometric
// solution of the characteristic cubic).
double minEigSym3(const double A[3][3]) {
    const double p1 = A[0][1] * A[0][1] + A[0][2] * A[0][2] + A[1][2] * A[1][2];
    const double q = (A[0][0] + A[1][1] + A[2][2]) / 3.0;
    if (p1 < 1e-18) {
        double m = A[0][0];
        if (A[1][1] < m) m = A[1][1];
        if (A[2][2] < m) m = A[2][2];
        return m;
    }
    const double p2 = (A[0][0] - q) * (A[0][0] - q) + (A[1][1] - q) * (A[1][1] - q) +
                      (A[2][2] - q) * (A[2][2] - q) + 2.0 * p1;
    const double p = sqrt(p2 / 6.0);
    double B[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) B[r][c] = (A[r][c] - (r == c ? q : 0.0)) / p;
    const double detB = B[0][0] * (B[1][1] * B[2][2] - B[1][2] * B[2][1]) -
                        B[0][1] * (B[1][0] * B[2][2] - B[1][2] * B[2][0]) +
                        B[0][2] * (B[1][0] * B[2][1] - B[1][1] * B[2][0]);
    double r = detB / 2.0;
    if (r < -1.0) r = -1.0;
    if (r > 1.0) r = 1.0;
    const double phi = acos(r) / 3.0;
    return q + 2.0 * p * cos(phi + 2.0943951023931953);  // + 2 pi / 3: the smallest root
}

}  // namespace

SphereFit fitSphere(const SampleSource& src) {
    SphereFit out = {};
    const int n = src.count();
    if (n < 4) return out;

    int16_t m[3];
    float a[3];
    double mean[3] = {0, 0, 0};
    for (int i = 0; i < n; i++) {
        src.get(i, m, a);
        for (int k = 0; k < 3; k++) mean[k] += m[k];
    }
    for (int k = 0; k < 3; k++) mean[k] /= n;

    // Rows [2u, 1], target |u|^2, for u = (m - mean) / kNorm. The fit is
    // translation- and scale-equivariant, so this is the same solution as
    // fitting raw counts, just well-conditioned.
    double M[4][5] = {};
    for (int i = 0; i < n; i++) {
        src.get(i, m, a);
        const double row[4] = { 2.0 * (m[0] - mean[0]) / kNorm, 2.0 * (m[1] - mean[1]) / kNorm,
                                2.0 * (m[2] - mean[2]) / kNorm, 1.0 };
        const double y = (row[0] * row[0] + row[1] * row[1] + row[2] * row[2]) / 4.0;
        for (int r = 0; r < 4; r++) {
            for (int c = r; c < 4; c++) M[r][c] += row[r] * row[c];
            M[r][4] += row[r] * y;
        }
    }
    for (int r = 1; r < 4; r++)
        for (int c = 0; c < r; c++) M[r][c] = M[c][r];
    if (!solve4(M)) return out;

    const double b[3] = { M[0][4], M[1][4], M[2][4] };
    const double r2 = M[3][4] + b[0] * b[0] + b[1] * b[1] + b[2] * b[2];
    if (!(r2 > 0.0)) return out;
    double centre[3];
    for (int k = 0; k < 3; k++) centre[k] = mean[k] + b[k] * kNorm;

    double S[3][3] = {};
    int used = 0;
    for (int i = 0; i < n; i++) {
        src.get(i, m, a);
        const double d[3] = { m[0] - centre[0], m[1] - centre[1], m[2] - centre[2] };
        const double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len <= 0.0) continue;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) S[r][c] += d[r] * d[c] / (len * len);
        used++;
    }
    if (used == 0) return out;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) S[r][c] /= used;

    for (int k = 0; k < 3; k++) out.bias[k] = (float)centre[k];
    out.radius = (float)(sqrt(r2) * kNorm);
    out.spread = (float)minEigSym3(S);
    out.valid  = true;
    return out;
}

void Session::reset() {
    *this = Session();
}

bool Session::classify(const int16_t mag[3], const float accel[3],
                       mag_orient::SampleOrient& out) const {
    if (!locked_) return false;
    out = mag_orient::orientSample(mag[0] - binBias_[0], mag[1] - binBias_[1],
                                   mag[2] - binBias_[2], accel[0], accel[1], accel[2]);
    return out.bin >= 0 && out.bin < MAG_CAL_BASELINE_BINS;
}

uint16_t Session::ceiling(int rollSector) {
    const int cap = (rollSector == 0) ? MAG_CAL_GAPFILL_TARGET_CAP
                                      : MAG_CAL_GAPFILL_TARGET_CAP_OTHER_ROLL;
    return (uint16_t)(MAG_CAL_BOOT_CEILING_MULT * cap);
}

bool Session::accepts(const mag_orient::SampleOrient& o) const {
    if (!locked_ || MAG_CAL_BOOT_CEILING_MULT <= 0) return true;
    return counts_[o.bin][o.rollSector] < ceiling(o.rollSector);
}

uint16_t Session::count(int bin, int rollSector) const {
    if (bin < 0 || bin >= MAG_CAL_BASELINE_BINS || rollSector < 0 || rollSector >= MAG_CAL_ROLL_SECTORS)
        return 0;
    return counts_[bin][rollSector];
}

void Session::rebinAll(const SampleSource& src) {
    memset(counts_, 0, sizeof(counts_));
    int16_t m[3];
    float a[3];
    mag_orient::SampleOrient o;
    for (int i = 0; i < src.count(); i++) {
        src.get(i, m, a);
        if (classify(m, a, o) && counts_[o.bin][o.rollSector] < UINT16_MAX)
            counts_[o.bin][o.rollSector]++;
    }
}

Event Session::onStored(const SampleSource& src, const mag_orient::SampleOrient* o, float lsbPerUt) {
    if (o && locked_ && counts_[o->bin][o->rollSector] < UINT16_MAX)
        counts_[o->bin][o->rollSector]++;

    const int n = src.count();
    if (n <= 0 || n % MAG_CAL_BOOT_REFIT_EVERY != 0) return Event::NONE;

    lastFit_ = fitSphere(src);
    if (!lastFit_.valid) return Event::NONE;

    if (!locked_) {
        const float rExpected = MAG_CAL_EXPECTED_FIELD_UT * lsbPerUt;
        const bool ok = n >= MAG_CAL_BOOT_MIN_SAMPLES &&
                        fabsf(lastFit_.radius / rExpected - 1.0f) < MAG_CAL_BOOT_RADIUS_TOL &&
                        lastFit_.spread >= MAG_CAL_BOOT_MIN_SPREAD &&
                        hasPrev_ &&
                        dist3(lastFit_.bias, prevBias_) / lsbPerUt < MAG_CAL_BOOT_STABLE_UT;
        memcpy(prevBias_, lastFit_.bias, sizeof(prevBias_));
        hasPrev_ = true;
        if (!ok) return Event::NONE;
        locked_ = true;
        lockN_  = n;
        memcpy(binBias_, lastFit_.bias, sizeof(binBias_));
        rebinAll(src);
        return Event::LOCKED;
    }

    if (dist3(lastFit_.bias, binBias_) / lsbPerUt <= MAG_CAL_BOOT_REBIN_UT) return Event::NONE;
    memcpy(binBias_, lastFit_.bias, sizeof(binBias_));
    rebins_++;
    rebinAll(src);
    return Event::REBIN;
}

}  // namespace baseline_boot
