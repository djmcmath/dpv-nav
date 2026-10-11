#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include "../types/types.h"

// What one log row holds, and the binary form MID and HIGH write it in. Pure
// C++ (no Arduino, no LittleFS) so tools/logrecord_test/ can pack real records
// on the host and check tools/decode_binlog.py against them.
//
// LOW stays CSV: it is the level divers run and the one that uploads itself.
// MID and HIGH are diagnostics. As CSV their rows cost ~240 bytes, which filled
// the 768 KiB partition in under an hour; as the fixed record below they cost
// 77, so a two-hour dive is ~540 KiB. Decoding is deliberately not done on the
// unit -- pull the .bin off tern.local and run tools/decode_binlog.py, which
// writes the same columns the CSV had.
namespace logging {

// All data needed for a log entry. Everything past mag_temp_c is ignored at LOW.
struct LogData {
    uint32_t timestamp_ms;
    float heading_deg;
    float speed_ms;
    bool  gpsSpeed;       // true = GPS, false = flowmeter
    float pos_x_m;
    float pos_y_m;
    float lat;            // current latitude (GPS or DR-estimated)
    float lon;            // current longitude (GPS or DR-estimated)
    char  pos_src;        // 'G' = GPS, 'W' = waypoint snap, 'E' = DR estimate, 'M' = diver mark, 'C' = current hold
    uint8_t gps_satellites; // satellites tracked (0 if no GPS fix)
    float   gps_hdop;       // HDOP (0.0 if no GPS fix)
    float   depth_m;        // depth below surface, meters (0 if sensor absent)
    float   water_temp_c;   // water temperature, degrees C (0 if sensor absent)
    // All levels: LIS3MDL die temperature, degrees C. NaN (logged as "nan")
    // when the read fails, rather than 0, which would read as a real value.
    float   mag_temp_c;
    // MID/HIGH fields
    imu::Vec3f mag_raw, accel_raw, gyro_raw;
    imu::Vec3f mag_cal, accel_cal, gyro_cal;
    float pitch_deg, roll_deg;
};

// ---------------------------------------------------------------------------
// Binary file layout: one LogFileHeader, then LogRecords until end of file.
// Little-endian, packed. A file cut short by a brownout is valid up to its last
// whole record. The decoder reads header_size and record_size from the header,
// so a later version may grow either without breaking it.
// ---------------------------------------------------------------------------

static constexpr char    LOG_BIN_MAGIC[4] = {'T', 'L', 'O', 'G'};
static constexpr uint8_t LOG_BIN_VERSION  = 1;

// Scale of each stored integer (value = integer * scale). The accel and gyro
// steps equal what the CSV printed; the mag step is finer than the LIS3MDL's
// own 0.0146 uT. Out-of-range values saturate.
static constexpr float LOG_SCALE_DEG    = 0.01f;    // heading, pitch, roll
static constexpr float LOG_SCALE_SPEED  = 0.001f;   // m/s
static constexpr float LOG_SCALE_POS    = 0.01f;    // m
static constexpr double LOG_SCALE_LATLON = 1e-7;    // degrees
static constexpr float LOG_SCALE_HDOP   = 0.1f;
static constexpr float LOG_SCALE_DEPTH  = 0.01f;    // m
static constexpr float LOG_SCALE_TEMP   = 0.01f;    // deg C
static constexpr float LOG_SCALE_MAG    = 0.01f;    // uT
static constexpr float LOG_SCALE_ACCEL  = 0.0005f;  // g
static constexpr float LOG_SCALE_GYRO   = 0.001f;   // rad/s

// A non-finite value is stored as the type's lowest (signed) or highest
// (unsigned) value, which the decoder writes back out as "nan".
static constexpr int16_t  LOG_NAN_I16 = INT16_MIN;
static constexpr int32_t  LOG_NAN_I32 = INT32_MIN;
static constexpr uint16_t LOG_NAN_U16 = UINT16_MAX;

// flags: bit 0 = speed from GPS; bits 3:1 = pos_src as an index into
// LOG_POS_SRC_CHARS. Append new sources (room for 8) -- never reorder, and add
// them to tools/decode_binlog.py too. A source missing from here is stored as
// '?', which the track processor rejects loudly rather than integrating a mark
// or current row as a step of the track.
static constexpr uint8_t LOG_FLAG_GPS_SPEED     = 0x01;
static constexpr uint8_t LOG_FLAG_POS_SRC_SHIFT = 1;
static constexpr uint8_t LOG_FLAG_POS_SRC_MASK  = 0x0E;
static constexpr char    LOG_POS_SRC_CHARS[]    = "GWEMC";
static constexpr uint8_t LOG_POS_SRC_UNKNOWN    = 7;

#pragma pack(push, 1)
struct LogFileHeader {
    char     magic[4];      // LOG_BIN_MAGIC
    uint8_t  version;       // LOG_BIN_VERSION
    uint8_t  level;         // LogLevel the file was recorded at
    uint16_t header_size;   // bytes before the first record
    uint16_t record_size;   // bytes per record
    uint8_t  reserved[6];
};

struct LogRecord {
    uint32_t timestamp_ms;
    uint32_t local_s;       // local wall-clock time as seconds since 1970; 0 = clock not set
    uint16_t heading;       // LOG_SCALE_DEG
    uint16_t speed;         // LOG_SCALE_SPEED
    int32_t  pos_x;         // LOG_SCALE_POS
    int32_t  pos_y;
    int32_t  lat;           // LOG_SCALE_LATLON
    int32_t  lon;
    uint8_t  flags;
    uint8_t  gps_satellites;
    uint8_t  gps_hdop;      // LOG_SCALE_HDOP
    int16_t  depth;         // LOG_SCALE_DEPTH
    int16_t  water_temp;    // LOG_SCALE_TEMP
    int16_t  mag_raw[3];    // LOG_SCALE_MAG
    int16_t  accel_raw[3];  // LOG_SCALE_ACCEL
    int16_t  gyro_raw[3];   // LOG_SCALE_GYRO
    int16_t  mag_cal[3];
    int16_t  accel_cal[3];
    int16_t  gyro_cal[3];
    int16_t  pitch;         // LOG_SCALE_DEG
    int16_t  roll;
    int16_t  mag_temp;      // LOG_SCALE_TEMP
};
#pragma pack(pop)

static_assert(sizeof(LogFileHeader) == 16, "LogFileHeader layout changed -- bump LOG_BIN_VERSION");
static_assert(sizeof(LogRecord) == 77, "LogRecord layout changed -- bump LOG_BIN_VERSION");

// Offset of `depth` in a record, for logging.cpp's dive/non-dive check.
static constexpr size_t LOG_RECORD_DEPTH_OFFSET = 31;

inline int16_t logQuantI16(float v, float scale) {
    if (!std::isfinite(v)) return LOG_NAN_I16;
    float q = roundf(v / scale);
    if (q > 32767.0f)  return 32767;
    if (q < -32767.0f) return -32767;   // INT16_MIN is the NaN marker
    return (int16_t)q;
}

inline uint16_t logQuantU16(float v, float scale) {
    if (!std::isfinite(v)) return LOG_NAN_U16;
    float q = roundf(v / scale);
    if (q > 65534.0f) return 65534;     // UINT16_MAX is the NaN marker
    if (q < 0.0f)     return 0;
    return (uint16_t)q;
}

inline int32_t logQuantI32(double v, double scale) {
    if (!std::isfinite(v)) return LOG_NAN_I32;
    double q = round(v / scale);
    if (q > 2147483647.0)  return INT32_MAX;
    if (q < -2147483647.0) return -INT32_MAX;
    return (int32_t)q;
}

inline void logQuantVec(const imu::Vec3f& v, float scale, int16_t out[3]) {
    out[0] = logQuantI16(v.x, scale);
    out[1] = logQuantI16(v.y, scale);
    out[2] = logQuantI16(v.z, scale);
}

inline LogFileHeader makeLogFileHeader(uint8_t level) {
    LogFileHeader h{};
    memcpy(h.magic, LOG_BIN_MAGIC, sizeof(h.magic));
    h.version     = LOG_BIN_VERSION;
    h.level       = level;
    h.header_size = sizeof(LogFileHeader);
    h.record_size = sizeof(LogRecord);
    return h;
}

// `local_s` is the caller's clock (0 when it has none) -- kept out of here so
// this stays free of time().
inline LogRecord packLogRecord(const LogData& d, uint32_t local_s) {
    LogRecord r{};
    r.timestamp_ms = d.timestamp_ms;
    r.local_s      = local_s;
    r.heading      = logQuantU16(d.heading_deg, LOG_SCALE_DEG);
    r.speed        = logQuantU16(d.speed_ms, LOG_SCALE_SPEED);
    r.pos_x        = logQuantI32(d.pos_x_m, LOG_SCALE_POS);
    r.pos_y        = logQuantI32(d.pos_y_m, LOG_SCALE_POS);
    r.lat          = logQuantI32(d.lat, LOG_SCALE_LATLON);
    r.lon          = logQuantI32(d.lon, LOG_SCALE_LATLON);

    const char* src = strchr(LOG_POS_SRC_CHARS, d.pos_src);
    uint8_t srcIdx  = (src && d.pos_src) ? (uint8_t)(src - LOG_POS_SRC_CHARS) : LOG_POS_SRC_UNKNOWN;
    r.flags = (d.gpsSpeed ? LOG_FLAG_GPS_SPEED : 0) |
              (uint8_t)((srcIdx << LOG_FLAG_POS_SRC_SHIFT) & LOG_FLAG_POS_SRC_MASK);

    r.gps_satellites = d.gps_satellites;
    float hdop = std::isfinite(d.gps_hdop) ? roundf(d.gps_hdop / LOG_SCALE_HDOP) : 255.0f;
    r.gps_hdop = (uint8_t)(hdop > 255.0f ? 255.0f : (hdop < 0.0f ? 0.0f : hdop));

    r.depth      = logQuantI16(d.depth_m, LOG_SCALE_DEPTH);
    r.water_temp = logQuantI16(d.water_temp_c, LOG_SCALE_TEMP);
    logQuantVec(d.mag_raw,   LOG_SCALE_MAG,   r.mag_raw);
    logQuantVec(d.accel_raw, LOG_SCALE_ACCEL, r.accel_raw);
    logQuantVec(d.gyro_raw,  LOG_SCALE_GYRO,  r.gyro_raw);
    logQuantVec(d.mag_cal,   LOG_SCALE_MAG,   r.mag_cal);
    logQuantVec(d.accel_cal, LOG_SCALE_ACCEL, r.accel_cal);
    logQuantVec(d.gyro_cal,  LOG_SCALE_GYRO,  r.gyro_cal);
    r.pitch    = logQuantI16(d.pitch_deg, LOG_SCALE_DEG);
    r.roll     = logQuantI16(d.roll_deg, LOG_SCALE_DEG);
    r.mag_temp = logQuantI16(d.mag_temp_c, LOG_SCALE_TEMP);
    return r;
}

}  // namespace logging
