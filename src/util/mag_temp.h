#pragma once

#include "../types/types.h"

// Magnetometer thermal compensation coefficients.
//
// The LIS3MDL's zero-gauss offset drifts with die temperature; ST publish no
// coefficient for it. Measured on this unit (2026-09-13/14 heat-gun test at the
// four cardinals) as a body-fixed offset, linear over 19-69 °C die, identical
// heating and cooling. Applied in imu::readMagRaw_SensorFrame(), before any
// calibration -- see docs/mag-temperature-compensation.md.
//
// /mag_temp.json  (uploaded to nav device LittleFS):
//   {
//     "ref_temp_c": 21.0,
//     "coeff_uT_per_c": { "x": 0.537, "y": 0.417, "z": 0.277 }
//   }
//
// coeff is µT per °C in the LOGICAL frame (same frame as the mag_*_raw log
// columns and mag_base.json). ref_temp_c is the die temperature the
// calibrations were collected at; readings are normalized to it. Other keys
// (notes, provenance) are ignored. tools/mag_temp_fit.py writes this file.
//
// Absent file = no compensation (identical to firmware before this existed).

namespace mag_temp {

struct MagTempCal {
    float      ref_temp_c = 21.0f;
    imu::Vec3f coeff_uT_per_c{0.0f, 0.0f, 0.0f};
};

static constexpr const char* FILE_PATH = "/mag_temp.json";

// Sanity bounds. The measured horizontal drift is ~0.65 µT/°C; anything past
// MAX_ABS_COEFF is a typo or a units mistake (counts, gauss), and applying it
// would wreck heading far worse than no compensation.
static constexpr float MAX_ABS_COEFF_UT_PER_C = 5.0f;
static constexpr float MIN_REF_TEMP_C = -40.0f;   // LIS3MDL operating range
static constexpr float MAX_REF_TEMP_C =  85.0f;

enum class LoadResult { Ok, Absent, Invalid };

// Load and validate /mag_temp.json. On anything but Ok, cal is left at its
// defaults (zero coefficients) and the caller should clear compensation.
LoadResult load(MagTempCal& cal);

}  // namespace mag_temp
