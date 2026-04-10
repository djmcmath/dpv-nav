#include "dpvlink.h"
#include <string.h>

// ===== JSON mode ==========================================================
#if DPVLINK_USE_JSON

#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// Packet type identification from JSON "t" field
// ---------------------------------------------------------------------------
PacketType identifyPacket(const char* buf, size_t len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf, len);
    if (err) return PacketType::UNKNOWN;

    const char* t = doc["t"] | "";
    if (t[0] == 'N') return PacketType::NAV;
    if (t[0] == 'D') return PacketType::DEBUG;
    // Backward compat: packets without "t" are assumed NavPacket
    if (doc["hdg"].is<float>()) return PacketType::NAV;
    return PacketType::UNKNOWN;
}

// ---------------------------------------------------------------------------
// NavPacket
// ---------------------------------------------------------------------------
size_t navPacketToBytes(const NavPacket& pkt, char* buf, size_t bufLen) {
    JsonDocument doc;
    doc["t"]    = "N";
    doc["hdg"]  = pkt.heading_deg;
    doc["pit"]  = pkt.pitch_deg;
    doc["rol"]  = pkt.roll_deg;
    doc["spd"]  = pkt.speed_ms;
    doc["dhm"]  = pkt.distance_home_m;
    doc["bhm"]  = pkt.bearing_home_deg;
    doc["px"]   = pkt.pos_x_m;
    doc["py"]   = pkt.pos_y_m;
    doc["st"]   = pkt.system_state;
    doc["fl"]   = pkt.flags;
    doc["gq"]   = pkt.gps_fix_quality;
    doc["gs"]   = pkt.gps_satellites;
    doc["up"]   = pkt.uptime_ms;
    // Cal progress fields — only serialize when calibration is active (saves bandwidth)
    if (pkt.cal_remaining_s > 0 || pkt.cal_coverage_pct > 0 || pkt.cal_mode > 0) {
        doc["cr"] = pkt.cal_remaining_s;
        doc["cp"] = pkt.cal_coverage_pct;
        doc["cm"] = pkt.cal_mode;
    }
    // Speed cal fields — only serialize during speed calibration (cal_mode 2/3/4)
    if (pkt.cal_mode >= 2) {
        doc["sd"] = pkt.speed_cal_dist_ft;
        doc["se"] = pkt.speed_cal_elapsed_s;
        doc["sk"] = pkt.speed_cal_k_existing;
        doc["sp"] = pkt.speed_cal_k_proposed;
    }
    // Boot flags — always send (display needs them for boot status screen)
    if (pkt.boot_flags) doc["bf"] = pkt.boot_flags;

    size_t n = serializeJson(doc, buf, bufLen - 1);
    if (n == 0 || n >= bufLen - 1) return 0;
    buf[n]     = '\n';
    buf[n + 1] = '\0';
    return n + 1;
}

bool bytesToNavPacket(const char* buf, size_t len, NavPacket& out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf, len);
    if (err) return false;

    out.heading_deg      = doc["hdg"] | 0.0f;
    out.pitch_deg        = doc["pit"] | 0.0f;
    out.roll_deg         = doc["rol"] | 0.0f;
    out.speed_ms         = doc["spd"] | 0.0f;
    out.distance_home_m  = doc["dhm"] | 0.0f;
    out.bearing_home_deg = doc["bhm"] | 0.0f;
    out.pos_x_m          = doc["px"]  | 0.0f;
    out.pos_y_m          = doc["py"]  | 0.0f;
    out.system_state     = doc["st"]  | (uint8_t)0;
    out.flags            = doc["fl"]  | (uint8_t)0;
    out.gps_fix_quality  = doc["gq"]  | (uint8_t)0;
    out.gps_satellites   = doc["gs"]  | (uint8_t)0;
    out.uptime_ms        = doc["up"]  | (uint32_t)0;
    out.cal_remaining_s      = doc["cr"]  | (uint8_t)0;
    out.cal_coverage_pct     = doc["cp"]  | (uint8_t)0;
    out.cal_mode             = doc["cm"]  | (uint8_t)0;
    out.speed_cal_dist_ft    = doc["sd"]  | (uint16_t)0;
    out.speed_cal_elapsed_s  = doc["se"]  | (uint16_t)0;
    out.speed_cal_k_existing = doc["sk"]  | 0.0f;
    out.speed_cal_k_proposed = doc["sp"]  | 0.0f;
    out.boot_flags           = doc["bf"]  | (uint8_t)0;
    return true;
}

// ---------------------------------------------------------------------------
// Speed calibration start command with embedded distance
// ---------------------------------------------------------------------------
size_t displaySpeedCalStartToBytes(uint16_t dist_ft, char* buf, size_t bufLen) {
    JsonDocument doc;
    doc["cmd"]  = static_cast<uint8_t>(DisplayCmd::START_SPEED_CAL);
    doc["dist"] = dist_ft;

    size_t n = serializeJson(doc, buf, bufLen - 1);
    if (n == 0 || n >= bufLen - 1) return 0;
    buf[n]     = '\n';
    buf[n + 1] = '\0';
    return n + 1;
}

uint16_t parseSpeedCalDist(const char* buf, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, buf, len)) return 300;
    return doc["dist"] | (uint16_t)300;
}

// ---------------------------------------------------------------------------
// DebugPacket
// ---------------------------------------------------------------------------
size_t debugPacketToBytes(const DebugPacket& pkt, char* buf, size_t bufLen) {
    JsonDocument doc;
    doc["t"]  = "D";
    doc["mx"] = pkt.mag_x;
    doc["my"] = pkt.mag_y;
    doc["mz"] = pkt.mag_z;
    doc["ax"] = pkt.accel_x;
    doc["ay"] = pkt.accel_y;
    doc["az"] = pkt.accel_z;
    doc["gx"] = pkt.gyro_x;
    doc["gy"] = pkt.gyro_y;
    doc["gz"] = pkt.gyro_z;
    doc["fh"] = pkt.fused_heading_deg;
    doc["mh"] = pkt.raw_mag_heading_deg;
    doc["pi"] = pkt.pitch_deg;
    doc["ri"] = pkt.roll_deg;

    size_t n = serializeJson(doc, buf, bufLen - 1);
    if (n == 0 || n >= bufLen - 1) return 0;
    buf[n]     = '\n';
    buf[n + 1] = '\0';
    return n + 1;
}

bool bytesToDebugPacket(const char* buf, size_t len, DebugPacket& out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf, len);
    if (err) return false;

    out.mag_x              = doc["mx"] | 0.0f;
    out.mag_y              = doc["my"] | 0.0f;
    out.mag_z              = doc["mz"] | 0.0f;
    out.accel_x            = doc["ax"] | 0.0f;
    out.accel_y            = doc["ay"] | 0.0f;
    out.accel_z            = doc["az"] | 0.0f;
    out.gyro_x             = doc["gx"] | 0.0f;
    out.gyro_y             = doc["gy"] | 0.0f;
    out.gyro_z             = doc["gz"] | 0.0f;
    out.fused_heading_deg  = doc["fh"] | 0.0f;
    out.raw_mag_heading_deg = doc["mh"] | 0.0f;
    out.pitch_deg          = doc["pi"] | 0.0f;
    out.roll_deg           = doc["ri"] | 0.0f;
    return true;
}

// ---------------------------------------------------------------------------
// DisplayCmd
// ---------------------------------------------------------------------------
size_t displayCmdToBytes(DisplayCmd cmd, char* buf, size_t bufLen) {
    JsonDocument doc;
    doc["cmd"] = static_cast<uint8_t>(cmd);

    size_t n = serializeJson(doc, buf, bufLen - 1);
    if (n == 0 || n >= bufLen - 1) return 0;
    buf[n]     = '\n';
    buf[n + 1] = '\0';
    return n + 1;
}

bool bytesToDisplayCmd(const char* buf, size_t len, DisplayCmd& out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf, len);
    if (err) return false;

    uint8_t v = doc["cmd"] | (uint8_t)0;
    out = static_cast<DisplayCmd>(v);
    return true;
}

// ===== Binary mode ========================================================
#else

// Packed binary: raw struct memcpy.  No framing/CRC yet — add COBS later.
size_t navPacketToBytes(const NavPacket& pkt, char* buf, size_t bufLen) {
    if (bufLen < sizeof(NavPacket)) return 0;
    memcpy(buf, &pkt, sizeof(NavPacket));
    return sizeof(NavPacket);
}

bool bytesToNavPacket(const char* buf, size_t len, NavPacket& out) {
    if (len < sizeof(NavPacket)) return false;
    memcpy(&out, buf, sizeof(NavPacket));
    return true;
}

PacketType identifyPacket(const char* buf, size_t len) {
    (void)buf; (void)len;
    return PacketType::NAV;  // binary mode only supports NavPacket for now
}

size_t debugPacketToBytes(const DebugPacket& pkt, char* buf, size_t bufLen) {
    if (bufLen < sizeof(DebugPacket)) return 0;
    memcpy(buf, &pkt, sizeof(DebugPacket));
    return sizeof(DebugPacket);
}

bool bytesToDebugPacket(const char* buf, size_t len, DebugPacket& out) {
    if (len < sizeof(DebugPacket)) return false;
    memcpy(&out, buf, sizeof(DebugPacket));
    return true;
}

size_t displayCmdToBytes(DisplayCmd cmd, char* buf, size_t bufLen) {
    if (bufLen < 1) return 0;
    buf[0] = static_cast<char>(cmd);
    return 1;
}

bool bytesToDisplayCmd(const char* buf, size_t len, DisplayCmd& out) {
    if (len < 1) return false;
    out = static_cast<DisplayCmd>(static_cast<uint8_t>(buf[0]));
    return true;
}

#endif
