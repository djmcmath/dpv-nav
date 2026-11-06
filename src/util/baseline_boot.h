#pragma once

#include <stdint.h>

#include "../config.h"
#include "mag_cal_orient.h"

// Baseline live grid: the in-session bootstrap that lets a *first* baseline
// cal show gap-fill's grid. See docs/baseline-live-grid-plan.md.
//
// Every live-feedback attempt before this one failed on the same
// chicken-and-egg: binning needs heading, heading needs a calibration, and
// baseline is collecting the data for the calibration. Gap-fill escapes it by
// running only after a real fit exists. This escapes it by fitting as it
// goes: a hard-iron-only sphere through every sample stored so far, refit
// every MAG_CAL_BOOT_REFIT_EVERY samples. Off the scooter, soft iron is within
// a few percent of identity, so hard iron alone puts heading within ~2-3 deg
// of the real fit -- far inside a 30 deg sector. Min/max (what ROUGH_SCAN's
// bars track) does not: it is 15-25 uT off until the last third of a tumble.
//
// Once the fit passes the lock gate, each sample is binned through
// mag_orient::orientSample -- the same per-sample, filter-free path gap-fill
// uses -- against the locked bias with identity soft iron.
//
// Pure C++ with no Arduino dependency, so tools/baseline_boot_test/ runs this
// exact code against real baseline uploads and the Python model in dive-map's
// calibration-processor/tools/baseline_bootstrap_replay.py. The two must
// agree sample for sample; change one, change the other.
namespace baseline_boot {

// Read access to the samples the caller has stored, in collection order.
// imu.cpp's buffer stores accel scaled to int16; the host test reads a CSV.
class SampleSource {
public:
    virtual ~SampleSource() {}
    virtual int count() const = 0;
    virtual void get(int i, int16_t mag[3], float accel[3]) const = 0;
};

struct SphereFit {
    float bias[3];   // raw counts
    float radius;    // raw counts
    float spread;    // smallest eigenvalue of the mean outer product of the
                     // samples' unit directions from `bias`; see MAG_CAL_BOOT_MIN_SPREAD
    bool  valid;
};

// Linear least-squares sphere |m|^2 = 2 m.b + c through every stored sample,
// r^2 = c + |b|^2. Mean-centred and accumulated in double: float sums of
// |m|^2 * x lose precision at these magnitudes. Also measures how fully the
// samples surround the fitted centre (`spread`), which a ring cannot fake.
SphereFit fitSphere(const SampleSource& src);

enum class Event : uint8_t {
    NONE,
    LOCKED,  // the gate passed: the grid can be shown, everything stored is binned
    REBIN,   // post-lock refit moved past MAG_CAL_BOOT_REBIN_UT: everything re-binned
};

// One baseline session's bootstrap state: the lock gate, the bias the grid is
// binned with, and the per-(cell, roll sector) counts it produced.
class Session {
public:
    void reset();

    bool locked() const { return locked_; }

    // The sample's orientation against the binning bias. False before lock,
    // when there is nothing to bin with.
    bool classify(const int16_t mag[3], const float accel[3],
                  mag_orient::SampleOrient& out) const;

    // Post-lock acceptance: false once the sample's (cell, roll sector) is at
    // its ceiling. Every sample is accepted before lock.
    bool accepts(const mag_orient::SampleOrient& o) const;

    // The caller has just stored a sample, now src.count() - 1. `o` is its
    // classify() result, or null before lock. Counts it, refits when due,
    // and may lock or re-bin. lsbPerUt converts the uT thresholds to counts.
    Event onStored(const SampleSource& src, const mag_orient::SampleOrient* o, float lsbPerUt);

    uint16_t count(int bin, int rollSector) const;
    const float* binBias() const { return binBias_; }
    const SphereFit& lastFit() const { return lastFit_; }
    int lockN() const { return lockN_; }
    int rebins() const { return rebins_; }

    // Ceiling for one roll sector's count in a cell; see MAG_CAL_BOOT_CEILING_MULT.
    static uint16_t ceiling(int rollSector);

private:
    void rebinAll(const SampleSource& src);

    bool      locked_ = false;
    bool      hasPrev_ = false;
    float     prevBias_[3] = {};
    float     binBias_[3] = {};
    SphereFit lastFit_ = {};
    int       lockN_ = 0;
    int       rebins_ = 0;
    uint16_t  counts_[MAG_CAL_BASELINE_BINS][MAG_CAL_ROLL_SECTORS] = {};
};

}  // namespace baseline_boot
