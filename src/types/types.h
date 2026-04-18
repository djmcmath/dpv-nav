#pragma once

#include <cstdint>

#ifndef mag_reading_h
#define mag_reading_h

struct mag_reading {
    int16_t x;
    int16_t y;
    int16_t z;
};

#endif // mag_reading_h

namespace imu {

struct Vec3i16 { int16_t x, y, z; };
struct Vec3f   { float   x, y, z; };

struct ImuConfig {
  // Full-scale ranges (you’ll map to sensor register values in .cpp)
  float accel_g_fullscale;     // e.g. 2, 4, 8, 16
  float gyro_dps_fullscale;    // e.g. 250, 500, 1000, 2000
  float mag_uT_fullscale;      // depends on sensor; ok to ignore initially
  uint16_t sample_hz;          // e.g. 100
};

struct AxisMap {
  // maps logical X/Y/Z to sensor axes with sign
  // Example: +X = sensor Y, +Y = -sensor X, +Z = sensor Z
  int8_t x_axis; // +1=+X, -1=-X, +2=+Y, -2=-Y, +3=+Z, -3=-Z
  int8_t y_axis;
  int8_t z_axis;
};

}


struct Calib3 {
  imu::Vec3f bias;     // subtract
  imu::Vec3f scale;    // multiply (1,1,1 if unused)
};

struct MagCalib {
  imu::Vec3f bias;       // hard-iron offset
  float softIron[3][3]; // 3x3 (identity if unused)
};

struct GpsFix {
  float lat;            // degrees (negative = South)
  float lon;            // degrees (negative = West)
  float altitude_m;     // meters above MSL
  float speed_knots;    // ground speed
  float course_deg;     // course over ground (degrees)
  float hdop;           // horizontal dilution of precision
  uint8_t satellites;   // number of satellites tracked
  uint8_t fix_quality;  // 0=none, 1=GPS, 2=DGPS  (from GGA)
  uint8_t fix_type_3d;  // 1=no fix, 2=2D, 3=3D  (from GSA)
  bool has_fix;         // convenience flag
  uint32_t fix_age_ms;  // millis() at last valid fix
  // UTC time from GPS (populated from NMEA RMC sentence)
  uint8_t utc_year;     // 2-digit year (e.g. 25 for 2025); 0 if not yet parsed
  uint8_t utc_month;    // 1-12
  uint8_t utc_day;      // 1-31
  uint8_t utc_hour;     // 0-23
  uint8_t utc_minute;   // 0-59
  uint8_t utc_second;   // 0-59
  bool    has_time;     // true once GPS has parsed a valid date/time sentence
};