#pragma once

// Motor-on heading correction.
//
// The Fourier heading calibration is collected with the motor off (bench cal).
// When the motor runs, it adds a roughly constant magnetic bias that shifts
// the heading by a fixed amount regardless of heading angle or motor speed.
//
// /motor_cal.json  (uploaded to nav device LittleFS):
//   { "heading_offset_deg": -3.0 }
//
// Positive offset = compass reads low (add to get true heading).
// Negative offset = compass reads high (subtract from indicated to get true).
//
// Future evolution: extend to a speed-indexed table once enough calibration
// runs exist to characterize the speed dependence.

namespace motor_cal {

struct MotorCal {
    float heading_offset_deg = 0.0f;
};

static constexpr const char* FILE_PATH = "/motor_cal.json";

// Load /motor_cal.json.  Returns false if absent or invalid; cal stays at defaults (no correction).
bool load(MotorCal& cal);

}  // namespace motor_cal
