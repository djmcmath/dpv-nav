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
    uint8_t  gps_fix_quality;   // 0=none, 1=GPS, 2=DGPS  (GGA quality field)
    uint8_t  gps_satellites;    // number of satellites in fix
    uint8_t  gps_signal_bars;   // 0–4 signal quality bars (computed by nav device)
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

    float    heading_raw_deg;      // heading before hdg_cal correction (same as heading_deg if no cal loaded)

    uint16_t batt_mv;             // battery voltage in millivolts (0 = unknown / not read yet)

    // Boot status flags — set once during nav device setup(), sent in every packet.
    // Display uses these to show the boot results screen after first link contact.
    uint8_t boot_flags;  // see BOOT_* constants below

    uint8_t gps_hdop_x10;  // HDOP × 10 (0=no fix/unknown; 12 = HDOP 1.2)
    uint8_t gps_antenna;   // 0=unknown, 1=error/shorted, 2=internal, 3=active external
    uint8_t flags2;        // see FLAG2_* constants below

    float   depth_m;       // depth below surface, meters (0 if sensor absent, see FLAG2_DEPTH_PRESENT)
    float   water_temp_c;  // water temperature, degrees C (0 if sensor absent)
};

// NavPacket.boot_flags bit definitions
constexpr uint8_t BOOT_IMU_OK       = 0x01;  // IMU init succeeded
constexpr uint8_t BOOT_GPS_OK       = 0x02;  // GPS init succeeded
constexpr uint8_t BOOT_MAG_CAL_OK   = 0x04;  // mag calibration loaded from flash
constexpr uint8_t BOOT_GYRO_CAL_OK  = 0x08;  // gyro calibration loaded from flash
constexpr uint8_t BOOT_ACCEL_CAL_OK = 0x10;  // accel calibration loaded from flash
constexpr uint8_t BOOT_HDG_CAL_OK   = 0x20;  // Fourier heading calibration loaded from flash
constexpr uint8_t BOOT_DEPTH_OK     = 0x40;  // depth sensor (MS5837) detected and probed OK
constexpr uint8_t BOOT_DISPLAY_LINK_OK = 0x80;  // display echoed a boot ping back — Serial1 confirmed both ways

// NavPacket.flags bit definitions
constexpr uint8_t FLAG_TRUE_HEADING    = 0x01;  // 1 = true heading, 0 = magnetic
constexpr uint8_t FLAG_GPS_SPEED       = 0x02;  // 1 = GPS speed, 0 = flow sensor
constexpr uint8_t FLAG_HAS_HOME        = 0x04;  // 1 = home position set
constexpr uint8_t FLAG_GPS_ENABLED     = 0x08;  // 1 = GPS usage enabled (position + speed)
constexpr uint8_t FLAG_WIFI_ENABLED    = 0x10;  // 1 = WiFi radio enabled
// 0x20 retired (was FLAG_GPS_SPD_ENABLED — GPS position/speed toggles merged into one)
constexpr uint8_t FLAG_LOG_LEVEL_MASK  = 0xC0;  // bits 7:6 — log level (0=OFF, 1=LOW, 2=HIGH)
constexpr uint8_t FLAG_LOG_LEVEL_SHIFT = 6;

// NavPacket.flags2 bit definitions
constexpr uint8_t FLAG2_WIFI_CLIENT   = 0x01;  // 1 = connected to stored AP (client), 0 = own AP mode
constexpr uint8_t FLAG2_DEPTH_PRESENT = 0x02;  // 1 = depth sensor detected; depth_m/water_temp_c valid
constexpr uint8_t FLAG2_SALT_WATER    = 0x04;  // 1 = salt water density selected, 0 = fresh water
constexpr uint8_t FLAG2_DIVE_MODE     = 0x08;  // 1 = dive mode active (GPS+WiFi off), 0 = surface mode.
                                                // Authoritative source of truth — dive mode can now be
                                                // triggered automatically by depth, not just the menu.

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
// Calibration progress packet  (sent at ~2 Hz during active mag cal)
// ---------------------------------------------------------------------------

// Which calibration type is running
enum class CalType : uint8_t {
    BASELINE = 0,  // full sphere, unit off-scooter
    MOUNTED  = 1,  // limited range, unit on scooter
    HDG      = 2,  // 12-point Fourier heading-correction fit -- only used for
                    // CalCloudResultPacket.cal_type (never CalProgressPacket;
                    // HDG has no bin grid).
};

// Named phases. Baseline cal stays in ROUGH_SCAN for its whole session — no
// grid, just axis-range bars + fit stats; the diver ends it themselves
// (FINISH_BASELINE_COLLECTION) and the CSV is graded server-side. Mounted cal
// always runs in COLLECT (the guided grid) and auto-completes when bins are
// green. A two-pass ROUGH_SCAN->COLLECT handoff was tried and retired for
// baseline — see docs/baseline-cal-two-pass.md for why.
enum class CalPhase : uint8_t {
    COLLECT    = 0,  // mounted only: collect samples until bins are green
    ROUGH_SCAN = 1,  // baseline only: no grid, just axis-range bars + fit stats
};

struct CalProgressPacket {
    uint8_t  cal_type;          // CalType enum value
    uint8_t  phase;             // CalPhase enum value
    uint8_t  bins_green;        // count of bins at green threshold
    uint8_t  bins_total;        // total bins (60 baseline, 36 mounted)
    bool     complete;          // true when all bins green (CSV being dumped)
    uint8_t  bin_counts[60];    // sample count per bin, capped at 255 (baseline uses all 60; mounted uses first 36)
    int8_t   current_bin;       // bin index of current device orientation (-1 if unmappable)
    float    cur_pitch_deg;     // actual AHRS pitch at packet-send time (for orientation readout)
    float    cur_hdg_deg;       // actual heading at packet-send time (for orientation readout)

    // ROUGH_SCAN only: per-axis raw-range coverage (0-100%, vs an expected
    // range), and total accepted sample count. No bin/grid meaning in this
    // phase — bins_green/bins_total/bin_counts/current_bin/complete are unused.
    uint8_t  cov_x, cov_y, cov_z;
    uint16_t sample_count;

    // Incremental 2-D ellipse fit quality (updated each GetProgress call, ~2 Hz)
    // Reflects circularity of XY plane data in calibrated space — the quantity
    // that directly determines heading accuracy. Populated in both phases.
    bool  fit_valid;        // true once ≥8 samples and a valid ellipse solution exists
    float fit_hdg_err_deg;  // estimated heading error from XY ellipticity (degrees);
                            // converges toward the expected error of the resulting cal
    float fit_delta;        // centre shift since last solve, in µT;
                            // converges toward 0 as data stabilises (solution converged)
};

// ---------------------------------------------------------------------------
// Cloud calibration result packet  (sent once, nav -> display, after a bin-
// coverage cal finishes and the nav device uploads it for fitting)
// See docs/cloud-calibration-plan.md.
// ---------------------------------------------------------------------------

enum class CalCloudStage : uint8_t {
    OFFLINE = 0,  // no WiFi connection; upload was skipped
    FAILED  = 1,  // upload or fit failed; see `error`
    DONE    = 2,  // fit succeeded; quality/rms_pct/recommendation are valid
};

enum class CalCloudQuality : uint8_t { GOOD = 0, WARN = 1, BAD = 2 };

struct CalCloudResultPacket {
    uint8_t cal_type;             // CalType enum value (baseline/mounted)
    uint8_t stage;                // CalCloudStage enum value
    uint8_t quality;              // CalCloudQuality enum value (stage == DONE only)
    float   rms_pct;              // stage == DONE only
    char    recommendation[96];   // shown to the diver as-is; stage == DONE only
    char    error[64];            // shown to the diver as-is; stage == FAILED only
    char    calibration_id[40];   // UUID string; needed to accept/reject
    int16_t coverage_gaps;        // stage == DONE only; baseline only. -1 = not
                                   // applicable (mounted, or pre-9-axis-firmware CSV).
                                   // See divemap's baseline-cal-coverage-feedback-plan.md.
};

// ---------------------------------------------------------------------------
// Cloud account-link result packet  (nav -> display), device-auth bootstrap
// (RFC 8628) for the divemap.diverdaniel.com cloud link.
// See docs/cloud-calibration-plan.md and net/cloud_client.h.
// ---------------------------------------------------------------------------

enum class CloudLinkStage : uint8_t {
    CODE_READY = 0,  // device/user code obtained; poll for approval in progress
    DONE       = 1,  // approved; bearer token saved on the nav device
    FAILED     = 2,  // denied, expired, or a network/server error; see `error`
    OFFLINE    = 3,  // no WiFi connection; flow was not attempted
};

struct CloudLinkResultPacket {
    uint8_t stage;                  // CloudLinkStage enum value
    char    user_code[16];          // stage == CODE_READY only
    char    error[64];              // stage == FAILED only
};

// ---------------------------------------------------------------------------
// Waypoint list packet  (sent from nav device at ~1 Hz)
// ---------------------------------------------------------------------------
constexpr int WP_PACKET_MAX = 50;  // max waypoints in one WaypointListPacket

struct WaypointEntry {
    uint8_t idx;
    char    name[13];   // 12 display chars + null
    float   lat;
    float   lon;
};

struct WaypointListPacket {
    uint8_t      count;         // entries in this packet
    uint8_t      total_count;   // total waypoints in full list (may exceed count)
    WaypointEntry waypoints[WP_PACKET_MAX];
};

// ---------------------------------------------------------------------------
// Packet type discriminator (JSON "t" field)
// ---------------------------------------------------------------------------
enum class PacketType : uint8_t { UNKNOWN = 0, NAV, DEBUG, CAL_PROGRESS, WAYPOINT_LIST, CAL_CLOUD_RESULT, CLOUD_LINK_RESULT, BOOT_PING };

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
    START_FULL_CAL    = 13,  // (legacy) start 120s mag cal data collection
    START_BASELINE_CAL = 23, // start baseline (off-scooter) mag cal data collection
    START_MOUNTED_CAL  = 24, // start mounted (on-scooter) mag cal data collection
    START_SPEED_CAL      = 14, // start speed calibration (with embedded dist_ft field)
    TOGGLE_GPS           = 15, // toggle GPS usage (position + speed together)
    // 16 retired (was TOGGLE_GPS_SPD — split GPS pos/speed toggles merged into one)
    TOGGLE_WIFI          = 17, // toggle WiFi on/off
    CYCLE_LOG_LEVEL      = 18, // cycle logging level
    TOGGLE_OP_MODE       = 19, // toggle dive/surface mode (GPS + WiFi)
    SPEED_CAL_ACCEPT_RESET = 20, // accept result and reset history to single measurement
    SPEED_CAL_ACCEPT       = 21, // accept result and add to rolling history
    SPEED_CAL_REJECT       = 22, // reject result, discard measurement
    POWER_OFF              = 25, // save state and enter deep sleep
    START_HDG_FOURIER_CAL  = 26, // begin Fourier heading cal data collection (resets sample buffer)
    CAPTURE_HDG_POINT      = 27, // capture one (target, indicated) pair (carries float "tgt" field)
    FINALIZE_HDG_CAL       = 28, // end collection, save /hdg_samples.csv on nav device
    SELECT_WAYPOINT        = 29, // select waypoint as navigation target (carries uint8 "idx" field)
    ARRIVE_WAYPOINT        = 30, // snap position to waypoint (carries uint8 "idx" field)
    ACCEPT_CLOUD_CAL       = 31, // accept a cloud calibration result (carries "cid" calibration id)
    REJECT_CLOUD_CAL       = 32, // reject a cloud calibration result (carries "cid" calibration id)
    LINK_ACCOUNT           = 33, // begin device-auth cloud account link (RFC 8628)
    CANCEL_LINK            = 34, // cancel an in-progress account-link poll (BTN2 during wait)
    FINISH_BASELINE_COLLECTION = 35, // diver declares baseline collection done -> dump CSV, upload for grading
    TOGGLE_WATER_DENSITY   = 36, // toggle salt/fresh water density used for depth calculation
    LINK_HELLO             = 37, // reply to a BOOT_PING — proves the display->nav direction is alive
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

// Boot-time display-link round-trip check: nav sends this repeatedly for a
// few seconds during self-test; display replies with DisplayCmd::LINK_HELLO
// the instant it sees one. No payload — identifyPacket() on the "t" tag is
// all a receiver needs.
size_t bootPingToBytes(char* buf, size_t bufLen);

size_t calProgressPacketToBytes(const CalProgressPacket& pkt, char* buf, size_t bufLen);
bool   bytesToCalProgressPacket(const char* buf, size_t len, CalProgressPacket& out);

size_t calCloudResultPacketToBytes(const CalCloudResultPacket& pkt, char* buf, size_t bufLen);
bool   bytesToCalCloudResultPacket(const char* buf, size_t len, CalCloudResultPacket& out);

size_t cloudLinkResultPacketToBytes(const CloudLinkResultPacket& pkt, char* buf, size_t bufLen);
bool   bytesToCloudLinkResultPacket(const char* buf, size_t len, CloudLinkResultPacket& out);

// Serialize ACCEPT_CLOUD_CAL / REJECT_CLOUD_CAL with the calibration id embedded.
size_t displayCloudCalRespondToBytes(DisplayCmd cmd, const char* calibrationId,
                                      char* buf, size_t bufLen);

// Extract the calibration id from an ACCEPT_CLOUD_CAL / REJECT_CLOUD_CAL buffer.
// idOut is left empty ("") if the "cid" field is absent or malformed.
void parseCloudCalId(const char* buf, size_t len, char* idOut, size_t idOutLen);

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

// Serialize CAPTURE_HDG_POINT with the target heading (degrees).
size_t displayCaptureHdgPointToBytes(float target_deg, char* buf, size_t bufLen);

// Extract the target heading from a CAPTURE_HDG_POINT command buffer.
// Returns 0.0 if the "tgt" field is absent.
float parseCaptureHdgPoint(const char* buf, size_t len);

// Serialize SELECT_WAYPOINT or ARRIVE_WAYPOINT with the waypoint index.
size_t displaySelectWaypointToBytes(uint8_t idx, char* buf, size_t bufLen);
size_t displayArriveWaypointToBytes(uint8_t idx, char* buf, size_t bufLen);

// Extract the waypoint index from a SELECT_WAYPOINT or ARRIVE_WAYPOINT buffer.
// Returns 0 if the "idx" field is absent.
uint8_t parseWaypointIndex(const char* buf, size_t len);

// WaypointListPacket serialize / parse.
size_t waypointListPacketToBytes(const WaypointListPacket& pkt, char* buf, size_t bufLen);
bool   bytesToWaypointListPacket(const char* buf, size_t len, WaypointListPacket& out);
