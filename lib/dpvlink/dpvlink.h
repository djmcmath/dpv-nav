#pragma once

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Wire format selector: set to 1 for JSON (human-readable, easy debug),
// set to 0 for packed binary (lower overhead, future).
// ---------------------------------------------------------------------------
#ifndef DPVLINK_USE_JSON
#define DPVLINK_USE_JSON 1
#endif

// ---------------------------------------------------------------------------
// Nav -> Display packet  (sent at ~10 Hz)
// ---------------------------------------------------------------------------
struct NavPacket {
    float    heading_deg;       // 0-360
    float    pitch_deg;         // -90 to +90
    float    roll_deg;          // -180 to +180
    float    speed_ms;          // m/s (flow or GPS)
    float    distance_home_m;   // dead-reckoning distance to home
    float    bearing_home_deg;  // bearing to home (0-360)
    float    pos_x_m;           // meters east of home (or start)
    float    pos_y_m;           // meters north of home (or start)
    uint8_t  system_state;      // SystemState enum value
    uint8_t  flags;             // see FLAG_* constants below
    uint8_t  gps_fix_quality;   // 0=none, 1=GPS, 2=DGPS
    uint8_t  gps_satellites;    // number of satellites in fix
    uint32_t uptime_ms;
    uint8_t  cal_remaining_s;   // seconds remaining in active calibration (0 otherwise)
    uint8_t  cal_coverage_pct;  // calibration coverage/completeness 0-100
    // cal_mode values:
    //   0 = quick mag cal (hard-iron sweep)
    //   1 = full mag cal  (soft-iron data collection)
    //   2 = speed cal — waiting for flow to start
    //   3 = speed cal — run in progress (timer running)
    //   4 = speed cal — result ready, awaiting accept/reject
    uint8_t  cal_mode;

    // Speed calibration result fields (only populated when cal_mode == 2/3/4)
    uint16_t speed_cal_dist_ft;      // selected calibration distance (feet)
    uint16_t speed_cal_elapsed_s;    // elapsed run time (seconds)
    float    speed_cal_k_existing;   // k-factor before this calibration run
    float    speed_cal_k_proposed;   // computed k-factor from this run

    // Boot status flags — set once during nav device setup(), sent in every packet.
    // Display uses these to show the boot results screen after first link contact.
    uint8_t boot_flags;  // see BOOT_* constants below
};

// NavPacket.boot_flags bit definitions
constexpr uint8_t BOOT_IMU_OK       = 0x01;  // IMU init succeeded
constexpr uint8_t BOOT_GPS_OK       = 0x02;  // GPS init succeeded
constexpr uint8_t BOOT_MAG_CAL_OK   = 0x04;  // mag calibration loaded from flash
constexpr uint8_t BOOT_GYRO_CAL_OK  = 0x08;  // gyro calibration loaded from flash
constexpr uint8_t BOOT_ACCEL_CAL_OK = 0x10;  // accel calibration loaded from flash

// NavPacket.flags bit definitions
constexpr uint8_t FLAG_TRUE_HEADING    = 0x01;  // 1 = true heading, 0 = magnetic
constexpr uint8_t FLAG_GPS_SPEED       = 0x02;  // 1 = GPS speed, 0 = flow sensor
constexpr uint8_t FLAG_HAS_HOME        = 0x04;  // 1 = home position set
constexpr uint8_t FLAG_GPS_POS_ENABLED = 0x08;  // 1 = GPS position usage enabled
constexpr uint8_t FLAG_WIFI_ENABLED    = 0x10;  // 1 = WiFi radio enabled
constexpr uint8_t FLAG_GPS_SPD_ENABLED = 0x20;  // 1 = GPS speed usage enabled
constexpr uint8_t FLAG_LOG_LEVEL_MASK  = 0xC0;  // bits 7:6 — log level (0=OFF, 1=LOW, 2=HIGH)
constexpr uint8_t FLAG_LOG_LEVEL_SHIFT = 6;

// ---------------------------------------------------------------------------
// Debug packet  (sent alongside NavPacket when debug mode enabled)
// ---------------------------------------------------------------------------
struct DebugPacket {
    float mag_x, mag_y, mag_z;         // calibrated magnetometer (uT)
    float accel_x, accel_y, accel_z;   // calibrated accelerometer (g)
    float gyro_x, gyro_y, gyro_z;      // calibrated gyroscope (rad/s)
    float fused_heading_deg;            // AHRS heading (0-360)
    float raw_mag_heading_deg;          // atan2(mag_y, mag_x), no tilt comp
    float pitch_deg;                    // AHRS pitch
    float roll_deg;                     // AHRS roll
};

// ---------------------------------------------------------------------------
// Packet type discriminator (JSON "t" field)
// ---------------------------------------------------------------------------
enum class PacketType : uint8_t { UNKNOWN = 0, NAV, DEBUG };

PacketType identifyPacket(const char* buf, size_t len);

// ---------------------------------------------------------------------------
// Display -> Nav commands
// ---------------------------------------------------------------------------
enum class DisplayCmd : uint8_t {
    NONE = 0,
    SET_HOME,           // 1
    CLEAR_HOME,         // 2
    START_MAG_CAL,      // 3
    START_GYRO_CAL,     // 4
    RESET,              // 5
    // Menu-driven commands (10+)
    NAV_OUTBOUND   = 10,  // select outbound waypoint as destination
    NAV_HOME       = 11,  // select power-on position as home/destination
    MARK_POSITION  = 12,  // mark current position in logs
    START_FULL_CAL = 13,  // start 120s mag cal data collection
    START_SPEED_CAL      = 14, // start speed calibration (with embedded dist_ft field)
    TOGGLE_GPS_POS       = 15, // toggle GPS position usage
    TOGGLE_GPS_SPD       = 16, // toggle GPS speed usage
    TOGGLE_WIFI          = 17, // toggle WiFi on/off
    CYCLE_LOG_LEVEL      = 18, // cycle logging level
    TOGGLE_OP_MODE       = 19, // toggle dive/surface mode (GPS + WiFi)
    SPEED_CAL_ACCEPT_RESET = 20, // accept result and reset history to single measurement
    SPEED_CAL_ACCEPT       = 21, // accept result and add to rolling history
    SPEED_CAL_REJECT       = 22, // reject result, discard measurement
};

// ---------------------------------------------------------------------------
// Serialize / parse API
//
// All functions write into caller-provided buffers.
// JSON mode: output is a single line terminated by '\n'.
// Binary mode: output is raw bytes with a 2-byte sync header + CRC (future).
//
// Returns number of bytes written, or 0 on error.
// Parse functions return true on success.
// ---------------------------------------------------------------------------

size_t navPacketToBytes(const NavPacket& pkt, char* buf, size_t bufLen);
bool   bytesToNavPacket(const char* buf, size_t len, NavPacket& out);

size_t debugPacketToBytes(const DebugPacket& pkt, char* buf, size_t bufLen);
bool   bytesToDebugPacket(const char* buf, size_t len, DebugPacket& out);

size_t displayCmdToBytes(DisplayCmd cmd, char* buf, size_t bufLen);
bool   bytesToDisplayCmd(const char* buf, size_t len, DisplayCmd& out);

// Serialize START_SPEED_CAL with the selected distance embedded as "dist" field.
// Use this instead of displayCmdToBytes() when initiating a speed calibration run.
size_t displaySpeedCalStartToBytes(uint16_t dist_ft, char* buf, size_t bufLen);

// Extract the "dist" field from a parsed START_SPEED_CAL command buffer.
// Returns 300 (default) if the field is absent.
uint16_t parseSpeedCalDist(const char* buf, size_t len);
