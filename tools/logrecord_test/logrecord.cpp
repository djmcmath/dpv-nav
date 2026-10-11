// Host-side test for the binary log record. Compiles the REAL
// src/util/log_record.h, packs records with it, and writes two files:
//   <bin>  the log exactly as the firmware would write it
//   <csv>  what tools/decode_binlog.py must produce from it
// run.sh then runs the decoder and diffs. The expected CSV is derived here
// from the packed integers and the scales written out below as literals, so
// a scale changed in log_record.h without the decoder following fails either
// the scale checks in this file or the diff.
//   see tools/logrecord_test/run.sh
#include "../../src/util/log_record.h"

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

using namespace logging;

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { printf("  FAIL: %s\n", what); failures++; }
}
static void near(double got, double want, double tol, const char* what) {
    if (!(fabs(got - want) <= tol)) {
        printf("  FAIL: %s: got %.6f, want %.6f (+/- %.6f)\n", what, got, want, tol);
        failures++;
    }
}

static std::string num(long raw, double scale, int places, long nan) {
    if (raw == nan) return "nan";
    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f", places, raw * scale);
    return buf;
}

static std::string expectedRow(const LogRecord& r) {
    std::string s = std::to_string(r.timestamp_ms) + ",";
    if (r.local_s) {
        time_t t = r.local_s;
        struct tm tm_info{};
        gmtime_r(&t, &tm_info);
        char buf[24];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_info);
        s += buf;
    }
    uint8_t srcIdx = (r.flags & LOG_FLAG_POS_SRC_MASK) >> LOG_FLAG_POS_SRC_SHIFT;
    char src = srcIdx < strlen(LOG_POS_SRC_CHARS) ? LOG_POS_SRC_CHARS[srcIdx] : '?';
    s += "," + num(r.heading, 0.01, 2, LOG_NAN_U16) + "," + num(r.speed, 0.001, 3, LOG_NAN_U16);
    s += (r.flags & LOG_FLAG_GPS_SPEED) ? ",G" : ",F";
    s += "," + num(r.pos_x, 0.01, 2, LOG_NAN_I32) + "," + num(r.pos_y, 0.01, 2, LOG_NAN_I32);
    s += "," + num(r.lat, 1e-7, 8, LOG_NAN_I32) + "," + num(r.lon, 1e-7, 8, LOG_NAN_I32);
    s += std::string(",") + src;
    if (src == 'G') {
        char buf[32];
        snprintf(buf, sizeof(buf), ",%u,%.1f", r.gps_satellites, r.gps_hdop * 0.1);
        s += buf;
    } else {
        s += ",,";
    }
    s += "," + num(r.depth, 0.01, 2, LOG_NAN_I16) + "," + num(r.water_temp, 0.01, 2, LOG_NAN_I16);
    struct { const int16_t* v; double scale; } vecs[] = {
        {r.mag_raw, 0.01}, {r.accel_raw, 0.0005}, {r.gyro_raw, 0.001},
        {r.mag_cal, 0.01}, {r.accel_cal, 0.0005}, {r.gyro_cal, 0.001}};
    for (const auto& v : vecs)
        for (int i = 0; i < 3; i++) s += "," + num(v.v[i], v.scale, 3, LOG_NAN_I16);
    s += "," + num(r.pitch, 0.01, 2, LOG_NAN_I16) + "," + num(r.roll, 0.01, 2, LOG_NAN_I16);
    s += "," + num(r.mag_temp, 0.01, 2, LOG_NAN_I16);
    return s;
}

// A row with the magnitudes a real dive log holds (values from a 2026-09-07 HIGH log).
static LogData typical() {
    LogData d{};
    d.timestamp_ms = 836582;
    d.heading_deg = 207.57f;  d.speed_ms = 0.612f;  d.gpsSpeed = false;
    d.pos_x_m = 1830.13f;     d.pos_y_m = -7619.96f;
    d.lat = 47.52045059f;     d.lon = -122.25962067f;
    d.pos_src = 'G';          d.gps_satellites = 11;  d.gps_hdop = 0.9f;
    d.depth_m = -0.77f;       d.water_temp_c = 20.20f;  d.mag_temp_c = 21.43f;
    d.mag_raw   = {-33.119f, -24.847f, 85.267f};
    d.accel_raw = {0.003f, -0.272f, -0.950f};
    d.gyro_raw  = {0.055f, -0.083f, -0.038f};
    d.mag_cal   = {-10.892f, -12.204f, 53.912f};
    d.accel_cal = {-0.003f, -0.270f, -0.963f};
    d.gyro_cal  = {-0.000f, 0.002f, 0.006f};
    d.pitch_deg = -0.19f;     d.roll_deg = 15.17f;
    return d;
}

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: logrecord <out.bin> <expected.csv>\n"); return 2; }

    printf("layout\n");
    check(offsetof(LogRecord, depth) == LOG_RECORD_DEPTH_OFFSET, "LOG_RECORD_DEPTH_OFFSET points at depth");
    LogFileHeader h = makeLogFileHeader(3);
    check(memcmp(h.magic, "TLOG", 4) == 0 && h.version == 1 && h.level == 3, "header identifies the file");
    check(h.header_size == 16 && h.record_size == 77, "header carries its own sizes");

    printf("scales match the decoder's\n");
    near(LOG_SCALE_DEG, 0.01, 1e-9, "LOG_SCALE_DEG");      near(LOG_SCALE_SPEED, 0.001, 1e-9, "LOG_SCALE_SPEED");
    near(LOG_SCALE_POS, 0.01, 1e-9, "LOG_SCALE_POS");      near(LOG_SCALE_LATLON, 1e-7, 1e-15, "LOG_SCALE_LATLON");
    near(LOG_SCALE_HDOP, 0.1, 1e-8, "LOG_SCALE_HDOP");     near(LOG_SCALE_DEPTH, 0.01, 1e-9, "LOG_SCALE_DEPTH");
    near(LOG_SCALE_TEMP, 0.01, 1e-9, "LOG_SCALE_TEMP");    near(LOG_SCALE_MAG, 0.01, 1e-9, "LOG_SCALE_MAG");
    near(LOG_SCALE_ACCEL, 0.0005, 1e-10, "LOG_SCALE_ACCEL"); near(LOG_SCALE_GYRO, 0.001, 1e-9, "LOG_SCALE_GYRO");

    printf("a typical row survives to within half a step\n");
    LogData d = typical();
    LogRecord r = packLogRecord(d, 1757244248u);
    near(r.heading * 0.01, d.heading_deg, 0.005, "heading");
    near(r.speed * 0.001, d.speed_ms, 0.0005, "speed");
    near(r.pos_x * 0.01, d.pos_x_m, 0.005, "pos_x");
    near(r.pos_y * 0.01, d.pos_y_m, 0.005, "pos_y");
    near(r.lat * 1e-7, d.lat, 5e-8, "lat");
    near(r.lon * 1e-7, d.lon, 5e-8, "lon");
    near(r.gps_hdop * 0.1, d.gps_hdop, 0.05, "hdop");
    near(r.depth * 0.01, d.depth_m, 0.005, "depth");
    near(r.water_temp * 0.01, d.water_temp_c, 0.005, "water temp");
    near(r.mag_temp * 0.01, d.mag_temp_c, 0.005, "mag temp");
    near(r.mag_raw[2] * 0.01, d.mag_raw.z, 0.005, "mag_raw.z");
    near(r.mag_cal[0] * 0.01, d.mag_cal.x, 0.005, "mag_cal.x");
    // Accel and gyro are stored at (a divisor of) the step the CSV printed, so
    // nothing the CSV carried is lost.
    near(r.accel_raw[1] * 0.0005, d.accel_raw.y, 1e-6, "accel_raw.y exact to 3 places");
    near(r.accel_cal[2] * 0.0005, d.accel_cal.z, 1e-6, "accel_cal.z exact to 3 places");
    near(r.gyro_raw[1] * 0.001, d.gyro_raw.y, 1e-6, "gyro_raw.y exact to 3 places");
    near(r.pitch * 0.01, d.pitch_deg, 0.005, "pitch");
    near(r.roll * 0.01, d.roll_deg, 0.005, "roll");
    check(r.gps_satellites == 11, "satellites");
    check(!(r.flags & LOG_FLAG_GPS_SPEED), "flow speed flag");

    printf("edge cases\n");
    std::vector<LogRecord> recs = {r};
    {
        LogData e = typical();          // every pos_src, GPS speed, no clock
        for (char src : {'W', 'E', 'M', 'C'}) {
            e.pos_src = src; e.gpsSpeed = (src == 'W'); e.timestamp_ms += 1000;
            LogRecord x = packLogRecord(e, 0);
            check(LOG_POS_SRC_CHARS[(x.flags & LOG_FLAG_POS_SRC_MASK) >> LOG_FLAG_POS_SRC_SHIFT] == src,
                  "pos_src round-trips");
            recs.push_back(x);
        }
        // A source the table lacks must not decode as a real one: stored as
        // 'E' it would be integrated as a step of the track.
        for (char bad : {'Z', '\0'}) {
            e.pos_src = bad; e.timestamp_ms += 1000;
            LogRecord x = packLogRecord(e, 0);
            check(((x.flags & LOG_FLAG_POS_SRC_MASK) >> LOG_FLAG_POS_SRC_SHIFT) == LOG_POS_SRC_UNKNOWN,
                  "unknown pos_src is stored as unknown");
            recs.push_back(x);
        }
    }
    {
        LogData e = typical();          // NaN everywhere a sensor can fail
        e.timestamp_ms += 5000;
        e.mag_temp_c = NAN; e.heading_deg = NAN; e.speed_ms = NAN; e.pos_x_m = NAN; e.lat = NAN;
        e.mag_raw.x = NAN; e.pitch_deg = INFINITY;
        LogRecord x = packLogRecord(e, 1757244253u);
        check(x.mag_temp == LOG_NAN_I16 && x.heading == LOG_NAN_U16 && x.speed == LOG_NAN_U16 &&
              x.pos_x == LOG_NAN_I32 && x.lat == LOG_NAN_I32 && x.mag_raw[0] == LOG_NAN_I16 &&
              x.pitch == LOG_NAN_I16, "non-finite values become the NaN marker");
        recs.push_back(x);
    }
    {
        LogData e = typical();          // saturation never lands on a NaN marker
        e.timestamp_ms = 0xFFFFFFF0u;
        e.mag_raw = {400.0f, -400.0f, 0.0f};  e.gyro_cal = {35.0f, -35.0f, 0.0f};
        e.heading_deg = 359.99f; e.speed_ms = 99.0f; e.gps_hdop = 99.9f; e.gps_satellites = 255;
        e.depth_m = 150.0f; e.roll_deg = -180.0f; e.pitch_deg = 90.0f;
        e.lat = -89.9999999f; e.lon = 179.9999999f; e.pos_x_m = -3.0e9f;
        LogRecord x = packLogRecord(e, 4102444799u);
        check(x.mag_raw[0] == 32767 && x.mag_raw[1] == -32767, "mag saturates short of the NaN marker");
        check(x.gyro_cal[0] == 32767 && x.gyro_cal[1] == -32767, "gyro saturates short of the NaN marker");
        check(x.speed == 65534, "speed saturates short of the NaN marker");
        check(x.pos_x == -INT32_MAX, "position saturates short of the NaN marker");
        check(x.gps_hdop == 255, "hdop saturates");
        check(x.heading == 35999, "heading just under 360");
        check(x.roll == -18000 && x.pitch == 9000, "full attitude range fits");
        check(x.depth == 15000, "150 m fits");
        recs.push_back(x);
    }

    FILE* bin = fopen(argv[1], "wb");
    FILE* csv = fopen(argv[2], "w");
    if (!bin || !csv) { perror("open"); return 2; }
    fwrite(&h, sizeof(h), 1, bin);
    fputs("timestamp_ms,local_time,heading_deg,speed_ms,speed_src,pos_x_m,pos_y_m,lat,lon,pos_src,"
          "gps_satellites,gps_hdop,depth_m,water_temp_c,"
          "mag_x_raw,mag_y_raw,mag_z_raw,accel_x_raw,accel_y_raw,accel_z_raw,"
          "gyro_x_raw,gyro_y_raw,gyro_z_raw,mag_x_cal,mag_y_cal,mag_z_cal,"
          "accel_x_cal,accel_y_cal,accel_z_cal,gyro_x_cal,gyro_y_cal,gyro_z_cal,"
          "pitch_deg,roll_deg,mag_temp_c\n", csv);
    for (const auto& x : recs) {
        fwrite(&x, sizeof(x), 1, bin);
        fprintf(csv, "%s\n", expectedRow(x).c_str());
    }
    // A row cut short by power loss: the decoder must stop at the last whole one.
    fwrite(&recs[0], 40, 1, bin);
    fclose(bin);
    fclose(csv);

    if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
    printf("\nAll log_record tests passed (%zu records written).\n", recs.size());
    return 0;
}
