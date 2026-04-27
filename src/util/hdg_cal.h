#pragma once

// 4-point heading calibration: records indicated vs actual heading at N/E/S/W,
// then applies circular linear interpolation to correct heading at runtime.
//
// Storage: /hdg_cal.json on LittleFS (nav device only)
// Wire:    SET_HDG_CAL DisplayCmd from display device after collecting 4 points

namespace hdg_cal {

// Indicated AHRS heading (pre-correction, with declination applied) when the
// device was aligned to each of the four cardinal headings, in order: N/E/S/W.
struct HdgCal {
    float indicated[4];  // [0]=North(0°) [1]=East(90°) [2]=South(180°) [3]=West(270°)
};

static constexpr const char* FILE_PATH = "/hdg_cal.json";

bool load(HdgCal& cal);
bool save(const HdgCal& cal);

// Apply heading correction via circular linear interpolation between the 4 points.
// headingDeg is the raw indicated heading (0-360); returns the corrected heading.
// Returns headingDeg unchanged if cal data is degenerate.
float apply(float headingDeg, const HdgCal& cal);

// Compute the 4 correction values (actual - indicated) for display/assessment.
// out_corrections[i] = actual[i] - indicated[i], normalized to [-180, 180].
void corrections(const HdgCal& cal, float out_corrections[4]);

}  // namespace hdg_cal
