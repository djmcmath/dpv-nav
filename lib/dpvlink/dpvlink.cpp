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
    if (t[0] == 'D' && t[1] == '\0') return PacketType::DEBUG;
    if (t[0] == 'C') return PacketType::CAL_PROGRESS;
    if (t[0] == 'W') return PacketType::WAYPOINT_LIST;
    if (t[0] == 'R') return PacketType::CAL_CLOUD_RESULT;
    if (t[0] == 'L') return PacketType::CLOUD_LINK_RESULT;
    if (t[0] == 'P') return PacketType::BOOT_PING;
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
    doc["gb"]   = pkt.gps_signal_bars;
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
    // Raw heading (before hdg_cal correction) — only include when it differs from heading_deg
    if (pkt.heading_raw_deg != pkt.heading_deg) doc["hr"] = pkt.heading_raw_deg;
    // Battery voltage — only include when we have a reading
    if (pkt.batt_mv > 0) doc["bv"] = pkt.batt_mv;
    // GPS detail fields — only include when populated
    if (pkt.gps_hdop_x10 > 0) doc["gh"] = pkt.gps_hdop_x10;
    if (pkt.gps_antenna  > 0) doc["ga"] = pkt.gps_antenna;
    if (pkt.flags2       > 0) doc["f2"] = pkt.flags2;
    // Depth fields — only include when the sensor is present (saves bandwidth otherwise)
    if (pkt.flags2 & FLAG2_DEPTH_PRESENT) {
        doc["dp"] = pkt.depth_m;
        doc["wt"] = pkt.water_temp_c;
    }

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
    out.gps_signal_bars  = doc["gb"]  | (uint8_t)0;
    out.uptime_ms        = doc["up"]  | (uint32_t)0;
    out.cal_remaining_s      = doc["cr"]  | (uint8_t)0;
    out.cal_coverage_pct     = doc["cp"]  | (uint8_t)0;
    out.cal_mode             = doc["cm"]  | (uint8_t)0;
    out.speed_cal_dist_ft    = doc["sd"]  | (uint16_t)0;
    out.speed_cal_elapsed_s  = doc["se"]  | (uint16_t)0;
    out.speed_cal_k_existing = doc["sk"]  | 0.0f;
    out.speed_cal_k_proposed = doc["sp"]  | 0.0f;
    out.boot_flags           = doc["bf"]  | (uint8_t)0;
    // heading_raw_deg: fall back to heading_deg if not present (no hdg_cal active)
    out.heading_raw_deg      = doc["hr"]  | out.heading_deg;
    out.batt_mv              = doc["bv"]  | (uint16_t)0;
    out.gps_hdop_x10         = doc["gh"]  | (uint8_t)0;
    out.gps_antenna          = doc["ga"]  | (uint8_t)0;
    out.flags2               = doc["f2"]  | (uint8_t)0;
    out.depth_m              = doc["dp"]  | 0.0f;
    out.water_temp_c         = doc["wt"]  | 0.0f;
    return true;
}

// ---------------------------------------------------------------------------
// Boot-time display-link round-trip check
// ---------------------------------------------------------------------------
size_t bootPingToBytes(char* buf, size_t bufLen) {
    JsonDocument doc;
    doc["t"] = "P";
    size_t n = serializeJson(doc, buf, bufLen - 1);
    if (n == 0 || n >= bufLen - 1) return 0;
    buf[n]     = '\n';
    buf[n + 1] = '\0';
    return n + 1;
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
// Fourier heading calibration commands
// ---------------------------------------------------------------------------
size_t displayCaptureHdgPointToBytes(float target_deg, char* buf, size_t bufLen) {
    JsonDocument doc;
    doc["cmd"] = static_cast<uint8_t>(DisplayCmd::CAPTURE_HDG_POINT);
    doc["tgt"] = target_deg;

    size_t n = serializeJson(doc, buf, bufLen - 1);
    if (n == 0 || n >= bufLen - 1) return 0;
    buf[n]     = '\n';
    buf[n + 1] = '\0';
    return n + 1;
}

float parseCaptureHdgPoint(const char* buf, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, buf, len)) return 0.0f;
    return doc["tgt"] | 0.0f;
}

// ---------------------------------------------------------------------------
// Waypoint commands
// ---------------------------------------------------------------------------
size_t displaySelectWaypointToBytes(uint8_t idx, char* buf, size_t bufLen) {
    JsonDocument doc;
    doc["cmd"] = static_cast<uint8_t>(DisplayCmd::SELECT_WAYPOINT);
    doc["idx"] = idx;

    size_t n = serializeJson(doc, buf, bufLen - 1);
    if (n == 0 || n >= bufLen - 1) return 0;
    buf[n]     = '\n';
    buf[n + 1] = '\0';
    return n + 1;
}

size_t displayArriveWaypointToBytes(uint8_t idx, char* buf, size_t bufLen) {
    JsonDocument doc;
    doc["cmd"] = static_cast<uint8_t>(DisplayCmd::ARRIVE_WAYPOINT);
    doc["idx"] = idx;

    size_t n = serializeJson(doc, buf, bufLen - 1);
    if (n == 0 || n >= bufLen - 1) return 0;
    buf[n]     = '\n';
    buf[n + 1] = '\0';
    return n + 1;
}

uint8_t parseWaypointIndex(const char* buf, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, buf, len)) return 0;
    return doc["idx"] | (uint8_t)0;
}

// ---------------------------------------------------------------------------
// WaypointListPacket
// ---------------------------------------------------------------------------
size_t waypointListPacketToBytes(const WaypointListPacket& pkt, char* buf, size_t bufLen) {
    JsonDocument doc;
    doc["t"]  = "W";
    doc["c"]  = pkt.count;
    doc["tc"] = pkt.total_count;

    JsonArray wps = doc["w"].to<JsonArray>();
    for (int i = 0; i < pkt.count; i++) {
        JsonObject e = wps.add<JsonObject>();
        e["i"]  = pkt.waypoints[i].idx;
        e["n"]  = pkt.waypoints[i].name;
        e["la"] = pkt.waypoints[i].lat;
        e["lo"] = pkt.waypoints[i].lon;
    }

    size_t n = serializeJson(doc, buf, bufLen - 1);
    if (n == 0 || n >= bufLen - 1) return 0;
    buf[n]     = '\n';
    buf[n + 1] = '\0';
    return n + 1;
}

bool bytesToWaypointListPacket(const char* buf, size_t len, WaypointListPacket& out) {
    JsonDocument doc;
    if (deserializeJson(doc, buf, len)) return false;

    out.count       = doc["c"]  | (uint8_t)0;
    out.total_count = doc["tc"] | (uint8_t)0;

    JsonArray wps = doc["w"];
    int n = (int)out.count;
    if (n > WP_PACKET_MAX) n = WP_PACKET_MAX;
    for (int i = 0; i < n; i++) {
        JsonObject e = wps[i];
        out.waypoints[i].idx = e["i"] | (uint8_t)0;
        const char* name = e["n"] | "";
        strncpy(out.waypoints[i].name, name, 12);
        out.waypoints[i].name[12] = '\0';
        out.waypoints[i].lat = e["la"] | 0.0f;
        out.waypoints[i].lon = e["lo"] | 0.0f;
    }
    return true;
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
// CalProgressPacket
// ---------------------------------------------------------------------------
size_t calProgressPacketToBytes(const CalProgressPacket& pkt, char* buf, size_t bufLen) {
    JsonDocument doc;
    doc["t"]   = "C";
    doc["ct"]  = pkt.cal_type;
    doc["ph"]  = pkt.phase;
    doc["bg"]  = pkt.bins_green;
    doc["bt"]  = pkt.bins_total;
    doc["ok"]  = pkt.complete;
    doc["cb"]  = pkt.current_bin;
    doc["pp"]  = pkt.cur_pitch_deg;
    doc["hh"]  = pkt.cur_hdg_deg;
    doc["sc"]  = pkt.sample_count;

    // Encode bin counts as a JSON array
    JsonArray bins = doc["bc"].to<JsonArray>();
    for (int i = 0; i < pkt.bins_total; i++) {
        bins.add(pkt.bin_counts[i]);
    }

    // ROUGH_SCAN only — omit otherwise to save bandwidth, same convention as fv/fe/fd below
    if (pkt.phase == (uint8_t)CalPhase::ROUGH_SCAN) {
        doc["cx"] = pkt.cov_x;
        doc["cy"] = pkt.cov_y;
        doc["cz"] = pkt.cov_z;
        if (pkt.max_samples_reached) doc["mx"] = true;
    }

    // GAP_FILL only: the server's per-cell target statuses, packed 2 bits per
    // cell into 15 bytes and hex-encoded (30 chars) rather than sent as a
    // second 60-element JSON array (~120 bytes). They're constant for the
    // whole session, so a one-shot packet at session start would be cheaper
    // still -- but then a display reboot or a single dropped packet leaves the
    // grid permanently blank with no way to ask for it again. 30 chars on a
    // packet that already carries the counts array buys statelessness, and the
    // caller already treats an over-long packet as "skip this frame" (this
    // function returns 0), so the failure mode is a missed 2 Hz update, not
    // corruption.
    if (pkt.phase == (uint8_t)CalPhase::GAP_FILL && pkt.has_targets) {
        char tg[31];
        for (int b = 0; b < 15; b++) {
            uint8_t packed = 0;
            for (int k = 0; k < 4; k++) {
                packed |= (uint8_t)((pkt.targets[b * 4 + k] & 0x03) << (k * 2));
            }
            static const char kHex[] = "0123456789abcdef";
            tg[b * 2]     = kHex[packed >> 4];
            tg[b * 2 + 1] = kHex[packed & 0x0F];
        }
        tg[30] = '\0';
        doc["tg"] = tg;

        // Roll breakdown for the currently-highlighted cell only (4 counts +
        // 4 statuses + 1 sector index) -- cheap because it's one cell, not
        // the full 60-cell grid. Same guard as "tg" above: no target map, no
        // point sending a breakdown for a cell nothing has flagged.
        JsonArray rc = doc["rc"].to<JsonArray>();
        JsonArray rt = doc["rt"].to<JsonArray>();
        for (int k = 0; k < 4; k++) {
            rc.add(pkt.current_bin_roll_counts[k]);
            rt.add(pkt.current_bin_roll_targeted[k]);
        }
        doc["rs"] = pkt.current_roll_sector;
    }

    // Fit quality — only include when valid to save bandwidth
    if (pkt.fit_valid) {
        doc["fv"] = pkt.fit_valid;
        doc["fe"] = pkt.fit_hdg_err_deg;
        doc["fd"] = pkt.fit_delta;
    }

    size_t n = serializeJson(doc, buf, bufLen - 1);
    if (n == 0 || n >= bufLen - 1) return 0;
    buf[n]     = '\n';
    buf[n + 1] = '\0';
    return n + 1;
}

bool bytesToCalProgressPacket(const char* buf, size_t len, CalProgressPacket& out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf, len);
    if (err) return false;

    out.cal_type   = doc["ct"]  | (uint8_t)0;
    out.phase      = doc["ph"]  | (uint8_t)0;
    out.bins_green = doc["bg"]  | (uint8_t)0;
    out.bins_total = doc["bt"]  | (uint8_t)0;
    out.complete      = doc["ok"]  | false;
    out.current_bin   = doc["cb"]  | (int8_t)-1;
    out.cur_pitch_deg = doc["pp"]  | 0.0f;
    out.cur_hdg_deg   = doc["hh"]  | 0.0f;
    out.sample_count  = doc["sc"]  | (uint16_t)0;

    JsonArray bins = doc["bc"];
    int count = (int)out.bins_total;
    if (count > 60) count = 60;
    for (int i = 0; i < count; i++) {
        out.bin_counts[i] = bins[i] | (uint8_t)0;
    }

    out.cov_x = doc["cx"] | (uint8_t)0;
    out.cov_y = doc["cy"] | (uint8_t)0;
    out.cov_z = doc["cz"] | (uint8_t)0;
    out.max_samples_reached = doc["mx"] | false;

    // Unpack the 2-bits-per-cell target statuses (see the encoder above).
    // Absent "tg" -> has_targets stays false and the array stays zeroed, which
    // renders as "no targets known" rather than "every cell is fine" -- the
    // display checks has_targets, not the contents.
    memset(out.targets, 0, sizeof(out.targets));
    out.has_targets = false;
    const char* tg = doc["tg"] | (const char*)nullptr;
    if (tg && strlen(tg) == 30) {
        bool ok = true;
        for (int b = 0; b < 15 && ok; b++) {
            uint8_t packed = 0;
            for (int h = 0; h < 2; h++) {
                char ch = tg[b * 2 + h];
                uint8_t nib;
                if (ch >= '0' && ch <= '9')      nib = (uint8_t)(ch - '0');
                else if (ch >= 'a' && ch <= 'f') nib = (uint8_t)(ch - 'a' + 10);
                else if (ch >= 'A' && ch <= 'F') nib = (uint8_t)(ch - 'A' + 10);
                else { ok = false; break; }
                packed = (uint8_t)((packed << 4) | nib);
            }
            if (!ok) break;
            for (int k = 0; k < 4; k++) {
                out.targets[b * 4 + k] = (uint8_t)((packed >> (k * 2)) & 0x03);
            }
        }
        if (ok) {
            out.has_targets = true;
        } else {
            // Partially decoded garbage is worse than none: a half-filled
            // target map would send the diver to cells nobody flagged.
            memset(out.targets, 0, sizeof(out.targets));
        }
    }

    // Roll breakdown for the currently-highlighted cell (see encoder above).
    // Defaults to all-zero/untargeted and sector -1 when absent -- same
    // "renders as unknown, not as satisfied" convention as `targets[]`.
    memset(out.current_bin_roll_counts, 0, sizeof(out.current_bin_roll_counts));
    memset(out.current_bin_roll_targeted, 0, sizeof(out.current_bin_roll_targeted));
    out.current_roll_sector = -1;
    if (out.has_targets) {
        JsonArray rc = doc["rc"];
        JsonArray rt = doc["rt"];
        for (int k = 0; k < 4; k++) {
            out.current_bin_roll_counts[k]   = rc[k]   | (uint8_t)0;
            out.current_bin_roll_targeted[k] = rt[k]   | (uint8_t)0;
        }
        out.current_roll_sector = doc["rs"] | (int8_t)-1;
    }

    out.fit_valid       = doc["fv"]  | false;
    out.fit_hdg_err_deg = doc["fe"]  | 0.0f;
    out.fit_delta       = doc["fd"]  | 0.0f;
    return true;
}

// ---------------------------------------------------------------------------
// CalCloudResultPacket
// ---------------------------------------------------------------------------
size_t calCloudResultPacketToBytes(const CalCloudResultPacket& pkt, char* buf, size_t bufLen) {
    JsonDocument doc;
    doc["t"]  = "R";
    doc["ct"] = pkt.cal_type;
    doc["cs"] = pkt.stage;
    if (pkt.stage == (uint8_t)CalCloudStage::DONE) {
        doc["cq"]  = pkt.quality;
        doc["crp"] = pkt.rms_pct;
        doc["crc"] = pkt.recommendation;
        doc["cid"] = pkt.calibration_id;
        doc["ccg"] = pkt.coverage_gaps;
        // Omit when true (the common case) to save bandwidth, same convention
        // as the other DONE-only fields; decoder defaults absent -> true.
        if (!pkt.installable) doc["in"] = false;
    } else if (pkt.stage == (uint8_t)CalCloudStage::FAILED) {
        doc["cer"] = pkt.error;
    }

    size_t n = serializeJson(doc, buf, bufLen - 1);
    if (n == 0 || n >= bufLen - 1) return 0;
    buf[n]     = '\n';
    buf[n + 1] = '\0';
    return n + 1;
}

bool bytesToCalCloudResultPacket(const char* buf, size_t len, CalCloudResultPacket& out) {
    JsonDocument doc;
    if (deserializeJson(doc, buf, len)) return false;

    out.cal_type = doc["ct"]  | (uint8_t)0;
    out.stage    = doc["cs"]  | (uint8_t)0;
    out.quality  = doc["cq"]  | (uint8_t)0;
    out.rms_pct  = doc["crp"] | 0.0f;
    out.coverage_gaps = doc["ccg"] | (int16_t)-1;

    const char* rec = doc["crc"] | "";
    strncpy(out.recommendation, rec, sizeof(out.recommendation) - 1);
    out.recommendation[sizeof(out.recommendation) - 1] = '\0';

    const char* err = doc["cer"] | "";
    strncpy(out.error, err, sizeof(out.error) - 1);
    out.error[sizeof(out.error) - 1] = '\0';

    const char* cid = doc["cid"] | "";
    strncpy(out.calibration_id, cid, sizeof(out.calibration_id) - 1);
    out.calibration_id[sizeof(out.calibration_id) - 1] = '\0';

    out.installable = doc["in"] | true;
    return true;
}

size_t displayCloudCalRespondToBytes(DisplayCmd cmd, const char* calibrationId,
                                      char* buf, size_t bufLen) {
    JsonDocument doc;
    doc["cmd"] = static_cast<uint8_t>(cmd);
    doc["cid"] = calibrationId;

    size_t n = serializeJson(doc, buf, bufLen - 1);
    if (n == 0 || n >= bufLen - 1) return 0;
    buf[n]     = '\n';
    buf[n + 1] = '\0';
    return n + 1;
}

void parseCloudCalId(const char* buf, size_t len, char* idOut, size_t idOutLen) {
    if (idOutLen == 0) return;
    idOut[0] = '\0';
    JsonDocument doc;
    if (deserializeJson(doc, buf, len)) return;
    const char* cid = doc["cid"] | "";
    strncpy(idOut, cid, idOutLen - 1);
    idOut[idOutLen - 1] = '\0';
}

// ---------------------------------------------------------------------------
// CloudLinkResultPacket
// ---------------------------------------------------------------------------
size_t cloudLinkResultPacketToBytes(const CloudLinkResultPacket& pkt, char* buf, size_t bufLen) {
    JsonDocument doc;
    doc["t"]  = "L";
    doc["ls"] = pkt.stage;
    if (pkt.stage == (uint8_t)CloudLinkStage::CODE_READY) {
        doc["luc"] = pkt.user_code;
    } else if (pkt.stage == (uint8_t)CloudLinkStage::FAILED) {
        doc["ler"] = pkt.error;
    }

    size_t n = serializeJson(doc, buf, bufLen - 1);
    if (n == 0 || n >= bufLen - 1) return 0;
    buf[n]     = '\n';
    buf[n + 1] = '\0';
    return n + 1;
}

bool bytesToCloudLinkResultPacket(const char* buf, size_t len, CloudLinkResultPacket& out) {
    JsonDocument doc;
    if (deserializeJson(doc, buf, len)) return false;

    out.stage = doc["ls"] | (uint8_t)0;

    const char* uc = doc["luc"] | "";
    strncpy(out.user_code, uc, sizeof(out.user_code) - 1);
    out.user_code[sizeof(out.user_code) - 1] = '\0';

    const char* err = doc["ler"] | "";
    strncpy(out.error, err, sizeof(out.error) - 1);
    out.error[sizeof(out.error) - 1] = '\0';
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
