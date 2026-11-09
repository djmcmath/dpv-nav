// Host tests for the baseline live grid's bootstrap (src/util/baseline_boot.h).
// Compiles the real source, not a copy -- see run.sh.
//
// Two halves:
//   * Real sessions. Each fixture is a real baseline upload, replayed in
//     collection order through the same Session the firmware runs. It must
//     match dive-map's Python model (baseline_bootstrap_replay.py --emit)
//     sample for sample, and the cells it shows must agree with the cells
//     the server's accepted fit puts those samples in.
//   * Synthetic. A perfect sphere fits exactly; a single-axis spin never
//     locks, about a vertical axis (caught by the radius gate) or a
//     horizontal one (whose 0.92x ring only the spread gate catches); a real
//     tumble does.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/util/baseline_boot.h"

using baseline_boot::Event;
using baseline_boot::Session;
using mag_orient::SampleOrient;

static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) { printf("  FAIL: %s\n", what); failures++; }
}

static constexpr float kLsbPerUt  = 68.42f;  // LIS3MDL +-4 gauss; the replay's default
static constexpr int   kMaxStored = 2160;    // config.h MAG_CAL_BIN_MAX_SAMPLES

struct Row { int16_t m[3]; float a[3]; };

class VecSource : public baseline_boot::SampleSource {
public:
    std::vector<Row> rows;
    int count() const override { return (int)rows.size(); }
    void get(int i, int16_t mag[3], float accel[3]) const override {
        memcpy(mag, rows[i].m, sizeof(rows[i].m));
        memcpy(accel, rows[i].a, sizeof(rows[i].a));
    }
};

// The firmware's per-sample flow (imu.cpp, baseline sub-phases), minus the
// duplicate check the fixtures' collector already applied.
struct Outcome {
    std::vector<int> devBin, devRoll;  // -1 pre-lock, -2 dropped by the ceiling
    Session s;
    int stored = 0;
};

static Outcome runSession(const std::vector<Row>& offered) {
    Outcome out;
    out.s.reset();
    VecSource src;
    for (const Row& r : offered) {
        if (src.count() >= kMaxStored) break;
        SampleOrient o;
        const bool have = out.s.classify(r.m, r.a, o);
        if (have && !out.s.accepts(o)) {
            out.devBin.push_back(-2); out.devRoll.push_back(-2);
            continue;
        }
        out.devBin.push_back(have ? o.bin : -1);
        out.devRoll.push_back(have ? o.rollSector : -1);
        src.rows.push_back(r);
        out.s.onStored(src, have ? &o : nullptr, kLsbPerUt);
    }
    out.stored = src.count();
    return out;
}

// ---------------------------------------------------------------- fixtures --

static std::vector<Row> loadCsv(const std::string& path) {
    std::vector<Row> rows;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return rows;
    char line[512];
    fgets(line, sizeof(line), f);  // header
    while (fgets(line, sizeof(line), f)) {
        double v[6];
        if (sscanf(line, "%lf,%lf,%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
            continue;
        Row r;
        for (int k = 0; k < 3; k++) { r.m[k] = (int16_t)lround(v[k]); r.a[k] = (float)v[3 + k]; }
        rows.push_back(r);
    }
    fclose(f);
    return rows;
}

// {"bias": [3], "soft_iron": [[3],[3],[3]]}: twelve numbers, in that order.
static bool loadFit(const std::string& path, float bias[3], float si[3][3]) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    std::string s;
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    double v[12];
    int got = 0;
    for (size_t i = 0; i < s.size() && got < 12;) {
        const char c = s[i];
        if ((c >= '0' && c <= '9') || c == '-' || c == '.') {
            char* end;
            v[got++] = strtod(s.c_str() + i, &end);
            i = end - s.c_str();
        } else {
            i++;
        }
    }
    if (got != 12) return false;
    for (int k = 0; k < 3; k++) bias[k] = (float)v[k];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) si[r][c] = (float)v[3 + r * 3 + c];
    return true;
}

struct Expected {
    int lockN = -1, rebins = -1, stored = -1;
    float binBias[3] = {};
    std::vector<int> devBin, devRoll;
};

static bool loadExpected(const std::string& path, Expected& e) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    char line[256];
    if (!fgets(line, sizeof(line), f) ||
        sscanf(line, "# lock_n=%d rebins=%d stored=%d bin_bias=%f,%f,%f", &e.lockN, &e.rebins,
               &e.stored, &e.binBias[0], &e.binBias[1], &e.binBias[2]) != 6) {
        fclose(f);
        return false;
    }
    fgets(line, sizeof(line), f);  // column header
    int b, r;
    while (fgets(line, sizeof(line), f) && sscanf(line, "%d,%d", &b, &r) == 2) {
        e.devBin.push_back(b);
        e.devRoll.push_back(r);
    }
    fclose(f);
    return true;
}

static void testFixture(const char* id, bool goodRun) {
    printf("fixture %s%s\n", id, goodRun ? "" : " (graded bad, looser bounds)");
    const std::string base = std::string("fixtures/") + id;
    const std::vector<Row> rows = loadCsv(base + ".csv");
    float tb[3], tsi[3][3];
    Expected ex;
    check(!rows.empty(), "fixture CSV loads");
    check(loadFit(base + ".json", tb, tsi), "fixture fit loads");
    check(loadExpected(base + ".expected.csv", ex), "replay reference loads");
    if (rows.empty() || ex.devBin.empty()) return;

    Outcome o = runSession(rows);

    // --- parity with the Python model --------------------------------------
    char msg[160];
    snprintf(msg, sizeof(msg), "lock at %d, replay says %d", o.s.lockN(), ex.lockN);
    check(o.s.lockN() == ex.lockN, msg);
    snprintf(msg, sizeof(msg), "%d re-bins, replay says %d", o.s.rebins(), ex.rebins);
    check(o.s.rebins() == ex.rebins, msg);
    snprintf(msg, sizeof(msg), "stored %d, replay says %d", o.stored, ex.stored);
    check(o.stored == ex.stored, msg);
    float dBias = 0;
    for (int k = 0; k < 3; k++) dBias = fmaxf(dBias, fabsf(o.s.binBias()[k] - ex.binBias[k]));
    snprintf(msg, sizeof(msg), "binning bias within 0.1 uT of the replay's (off %.2f uT)",
             dBias / kLsbPerUt);
    check(dBias / kLsbPerUt < 0.1f, msg);

    const size_t n = o.devBin.size() < ex.devBin.size() ? o.devBin.size() : ex.devBin.size();
    check(o.devBin.size() == ex.devBin.size(), "same number of samples offered as the replay");
    int differ = 0;
    for (size_t i = 0; i < n; i++)
        if (o.devBin[i] != ex.devBin[i] || o.devRoll[i] != ex.devRoll[i]) differ++;
    // Float firmware vs double Python can flip a sample sitting on a 30 deg
    // boundary; nothing else should differ.
    snprintf(msg, sizeof(msg), "per-sample cells match the replay (%d of %zu differ)", differ, n);
    check(differ <= (int)(n / 200), msg);

    // --- against the server's own binning -----------------------------------
    int shown = 0, same = 0, further = 0;
    for (size_t i = 0; i < n; i++) {
        if (o.devBin[i] < 0) continue;
        const Row& r = rows[i];
        float c[3];
        for (int k = 0; k < 3; k++)
            c[k] = tsi[k][0] * (r.m[0] - tb[0]) + tsi[k][1] * (r.m[1] - tb[1]) + tsi[k][2] * (r.m[2] - tb[2]);
        const SampleOrient t = mag_orient::orientSample(c[0], c[1], c[2], r.a[0], r.a[1], r.a[2]);
        shown++;
        const int de = abs(o.devBin[i] / 12 - t.bin / 12);
        int dh = abs(o.devBin[i] % 12 - t.bin % 12);
        if (dh > 6) dh = 12 - dh;
        if (o.devBin[i] == t.bin) same++;
        else if (de > 1 || dh > 1) further++;
    }
    const float pSame = shown ? (float)same / shown : 0, pFurther = shown ? (float)further / shown : 1;
    printf("  lock %d, re-bins %d, stored %d; live %.0f%% same cell, %.1f%% non-adjacent\n",
           o.s.lockN(), o.s.rebins(), o.stored, 100 * pSame, 100 * pFurther);
    check(o.s.lockN() >= 25 && o.s.lockN() <= 100, "locks within the first 25-100 samples");
    check(pSame >= (goodRun ? 0.84f : 0.75f), "live cell matches the server's often enough");
    check(pFurther <= (goodRun ? 0.01f : 0.02f), "live cell is almost never further than adjacent");
}

// --------------------------------------------------------------- synthetic --

static uint32_t rngState = 12345;
static float noise(float sigma) {  // deterministic, roughly normal
    float s = 0;
    for (int i = 0; i < 4; i++) {
        rngState = rngState * 1664525u + 1013904223u;
        s += (float)(rngState >> 8) / 16777216.0f - 0.5f;
    }
    return s * sigma * 1.7320508f;
}

static const float kTrueBias[3] = { 1430.0f, 3092.0f, 6363.0f };
static const float kFieldCounts = 50.1f * kLsbPerUt;

// Field in the body frame for a body rotated by R (body = R^T world), 67 deg
// dip, plus hard iron and sensor noise. Accel is set to a plausible level
// value -- the lock gate never reads it.
static Row bodySample(const float R[3][3]) {
    const float dip = 67.0f * 0.017453292f;
    const float w[3] = { kFieldCounts * cosf(dip), 0.0f, kFieldCounts * sinf(dip) };
    Row r;
    for (int k = 0; k < 3; k++) {
        const float b = R[0][k] * w[0] + R[1][k] * w[1] + R[2][k] * w[2];
        r.m[k] = (int16_t)lroundf(kTrueBias[k] + b + noise(15.0f));
    }
    r.a[0] = 0; r.a[1] = 0; r.a[2] = -1;
    return r;
}

static void rotZ(float t, float R[3][3]) {
    const float c = cosf(t), s = sinf(t);
    const float M[3][3] = { { c, -s, 0 }, { s, c, 0 }, { 0, 0, 1 } };
    memcpy(R, M, sizeof(M));
}
static void rotX(float t, float R[3][3]) {
    const float c = cosf(t), s = sinf(t);
    const float M[3][3] = { { 1, 0, 0 }, { 0, c, -s }, { 0, s, c } };
    memcpy(R, M, sizeof(M));
}

static void testSyntheticSphere() {
    printf("synthetic: exact sphere\n");
    VecSource src;
    for (int i = 0; i < 400; i++) {
        // Golden-angle points on the sphere, no noise.
        const float z = 1.0f - 2.0f * (i + 0.5f) / 400.0f, rho = sqrtf(1 - z * z);
        const float phi = 2.39996323f * i;
        Row r;
        const float u[3] = { rho * cosf(phi), rho * sinf(phi), z };
        for (int k = 0; k < 3; k++) r.m[k] = (int16_t)lroundf(kTrueBias[k] + kFieldCounts * u[k]);
        r.a[0] = r.a[1] = 0; r.a[2] = -1;
        src.rows.push_back(r);
    }
    const baseline_boot::SphereFit f = baseline_boot::fitSphere(src);
    float err = 0;
    for (int k = 0; k < 3; k++) err = fmaxf(err, fabsf(f.bias[k] - kTrueBias[k]));
    check(f.valid, "fit is valid");
    check(err < 1.0f, "centre recovered to within one count");
    check(fabsf(f.radius - kFieldCounts) < 1.0f, "radius recovered to within one count");
}

static void testSpinNeverLocks(const char* name, bool vertical) {
    printf("synthetic: single-axis spin about the %s axis\n", name);
    std::vector<Row> rows;
    for (int i = 0; i < 600; i++) {
        float R[3][3];
        const float t = 4.0f * 3.14159265f * i / 600.0f;
        if (vertical) rotZ(t, R); else rotX(t, R);
        rows.push_back(bodySample(R));
    }
    Outcome o = runSession(rows);
    check(!o.s.locked(), "a ring of samples never locks");
    if (o.s.lastFit().valid) {
        printf("  last fit r/expected %.2f, spread %.4f\n", o.s.lastFit().radius / kFieldCounts,
               o.s.lastFit().spread);
    }
}

static void testTumbleLocks() {
    printf("synthetic: tumble\n");
    std::vector<Row> rows;
    for (int i = 0; i < 300; i++) {
        float A[3][3], B[3][3], R[3][3];
        rotZ(0.37f * i, A);
        rotX(0.23f * i, B);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                R[r][c] = A[r][0] * B[0][c] + A[r][1] * B[1][c] + A[r][2] * B[2][c];
        rows.push_back(bodySample(R));
    }
    Outcome o = runSession(rows);
    check(o.s.locked(), "locks");
    check(o.s.lockN() <= 75, "locks by 75 samples");
    float err = 0;
    for (int k = 0; k < 3; k++) err = fmaxf(err, fabsf(o.s.binBias()[k] - kTrueBias[k]));
    check(err / kLsbPerUt < 1.0f, "binning bias within 1 uT of the truth");
}

int main() {
    testFixture("9471b121", true);
    testFixture("b423759c", true);
    testFixture("6858d148", true);
    testFixture("e58990ba", true);
    testFixture("76bf0382", false);
    testSyntheticSphere();
    testSpinNeverLocks("vertical", true);
    testSpinNeverLocks("horizontal", false);
    testTumbleLocks();
    if (failures) {
        printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("\nall baseline bootstrap checks passed\n");
    return 0;
}
