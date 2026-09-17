#include "logging.h"
#include "log_names.h"
#include "../config.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <cstdio>
#include <cstring>
#include <time.h>

namespace logging {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool     gReady    = false;   // init() succeeded

// Two levels, because selecting one and acting on it are deliberately separated
// by LOG_COMMIT_DELAY_MS (see config.h). gLevel is what the diver picked and
// what the UI reports; gActiveLevel is what the open file — if any — actually
// is, and it alone decides the CSV schema.
static LogLevel gLevel       = LogLevel::LEVEL_OFF;
static LogLevel gActiveLevel = LogLevel::LEVEL_OFF;
static uint32_t gLevelSetMs  = 0;    // millis() of the last cycleLevel()

static File     gLogFile;
static char     gLogPath[LOG_PATH_MAX] = "";

static constexpr size_t FREE_SPACE_THRESHOLD = 32768;  // 32 KB minimum free
static constexpr const char* LOG_DIR = "/logs";

// An unsynced ESP32 sits near epoch 0; any time >= Nov 2023 came from a real
// source (GPS or NTP). Used both for the date prefix below and for the
// local_time column.
static constexpr time_t CLOCK_VALID_EPOCH = 1700000000L;

// Log files are named /logs/YYYYMMDD-NNN.csv -- see util/log_names.h for the
// grammar and why it is dated. Sequence numbers are resolved per file at open
// time from what is on disk (openNextFile), not from a counter, so deleting a
// log can never hand its name to a later one.
static constexpr uint16_t LOG_SEQ_MAX = 999;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Today's date as YYYYMMDD, or "" when no clock has been set yet.
static void todayPrefix(char* out, size_t len) {
    time_t now_t = time(nullptr);
    if (now_t < CLOCK_VALID_EPOCH) {
        out[0] = '\0';
        return;
    }
    struct tm tm_info{};
    localtime_r(&now_t, &tm_info);
    strftime(out, len, "%Y%m%d", &tm_info);
}

// One pass over /logs.
struct LogScan {
    char     oldest[LOG_NAME_MAX] = "";  // oldest name by log_names::olderThan ("" = empty dir)
    char     newestPrefix[log_names::PREFIX_LEN + 1] = "";  // highest date prefix present ("" = no dated logs)
    uint16_t highestSeq           = 0;   // highest NNN under `seqPrefix` (0 = none)
};

// `seqPrefix` may be nullptr when the caller only wants oldest/newestPrefix.
static void scanLogDir(LogScan& out, const char* seqPrefix) {
    File dir = LittleFS.open(LOG_DIR);
    if (!dir || !dir.isDirectory()) return;

    File f = dir.openNextFile();
    while (f) {
        const char* name = f.name();  // e.g. "20260908-001.csv"
        if (out.oldest[0] == '\0' || log_names::olderThan(name, out.oldest)) {
            snprintf(out.oldest, sizeof(out.oldest), "%s", name);
        }
        char     prefix[log_names::PREFIX_LEN + 1];
        uint16_t seq;
        if (log_names::parse(name, prefix, seq)) {
            if (strcmp(prefix, out.newestPrefix) > 0) strcpy(out.newestPrefix, prefix);
            if (seqPrefix && strcmp(prefix, seqPrefix) == 0 && seq > out.highestSeq) {
                out.highestSeq = seq;
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
}

// Delete oldest log files until free space >= threshold. "Oldest" is decided
// by name alone (log_names::olderThan) -- LittleFS timestamps are only as good
// as the clock was when the file was written, and on this unit that clock is
// often unset. See util/log_names.h.
static void cleanupOldLogs() {
    size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    while (freeBytes < FREE_SPACE_THRESHOLD) {
        LogScan scan;
        scanLogDir(scan, nullptr);
        if (scan.oldest[0] == '\0') break;  // no log files left

        char path[LOG_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", LOG_DIR, scan.oldest);
        // Never prune the file we are actively writing -- it is the one log
        // that cannot be re-collected, and on a nearly full unit it can be the
        // only file left to consider.
        if (strcmp(path, gLogPath) == 0) break;
        if (LittleFS.remove(path)) {
            Serial.printf("[LOG] Deleted old log %s (free space: %u)\n", path,
                          (unsigned)(LittleFS.totalBytes() - LittleFS.usedBytes()));
        } else {
            break;  // can't delete, stop trying
        }
        freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    }
}

// Write CSV header appropriate for the current level.
static void writeHeader() {
    if (!gLogFile) return;
    if (gActiveLevel == LogLevel::LEVEL_LOW) {
        gLogFile.print("timestamp_ms,local_time,heading_deg,speed_ms,speed_src,"
                       "pos_x_m,pos_y_m,lat,lon,pos_src,"
                       "gps_satellites,gps_hdop,depth_m,water_temp_c,mag_temp_c\n");
    } else if (gActiveLevel == LogLevel::LEVEL_MID) {
        gLogFile.print("timestamp_ms,local_time,heading_deg,speed_ms,speed_src,"
                       "pos_x_m,pos_y_m,lat,lon,pos_src,"
                       "gps_satellites,gps_hdop,depth_m,water_temp_c,"
                       "mag_x_cal,mag_y_cal,mag_z_cal,"
                       "pitch_deg,roll_deg,mag_temp_c\n");
    } else if (gActiveLevel == LogLevel::LEVEL_HIGH) {
        gLogFile.print("timestamp_ms,local_time,heading_deg,speed_ms,speed_src,"
                       "pos_x_m,pos_y_m,lat,lon,pos_src,"
                       "gps_satellites,gps_hdop,depth_m,water_temp_c,"
                       "mag_x_raw,mag_y_raw,mag_z_raw,"
                       "accel_x_raw,accel_y_raw,accel_z_raw,"
                       "gyro_x_raw,gyro_y_raw,gyro_z_raw,"
                       "mag_x_cal,mag_y_cal,mag_z_cal,"
                       "accel_x_cal,accel_y_cal,accel_z_cal,"
                       "gyro_x_cal,gyro_y_cal,gyro_z_cal,"
                       "pitch_deg,roll_deg,mag_temp_c\n");
    }
}

static const char* levelName(LogLevel l);  // defined below, beside cycleLevel()

// Open a new log file, named for today's date plus the next free sequence
// number under it (see the naming note at the top of this file).
static bool openNextFile() {
    char prefix[log_names::PREFIX_LEN + 1];
    todayPrefix(prefix, sizeof(prefix));
    if (prefix[0] == '\0') {
        // No clock yet -- GPS hasn't fixed and WiFi hasn't NTP'd. Continue
        // under the newest date already on disk rather than inventing one, so
        // this file still sorts after every earlier log; fall back to all
        // zeros only on a unit that has never had a clock at all.
        LogScan scan;
        scanLogDir(scan, nullptr);
        strcpy(prefix, scan.newestPrefix[0] ? scan.newestPrefix : "00000000");
    }

    LogScan scan;
    scanLogDir(scan, prefix);
    if (scan.highestSeq >= LOG_SEQ_MAX) {
        Serial.printf("[LOG] Error: %s is full (%u logs today)\n", prefix, LOG_SEQ_MAX);
        return false;
    }
    snprintf(gLogPath, sizeof(gLogPath), "%s/%s-%03u.csv", LOG_DIR, prefix,
             (unsigned)(scan.highestSeq + 1));

    gLogFile = LittleFS.open(gLogPath, FILE_WRITE);
    if (!gLogFile) {
        Serial.printf("[LOG] Error: could not open %s\n", gLogPath);
        gLogPath[0] = '\0';
        return false;
    }
    Serial.printf("[LOG] Opened %s (level %s)\n", gLogPath, levelName(gActiveLevel));
    writeHeader();
    return true;
}

// Close current file if open.
static void closeFile() {
    if (gLogFile) {
        gLogFile.flush();
        gLogFile.close();
        Serial.printf("[LOG] Closed %s\n", gLogPath);
        gLogPath[0] = '\0';
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool init() {
    if (gReady) return true;

    // LittleFS is already mounted by nav_main.cpp setup().
    // Ensure /logs directory exists.
    LittleFS.mkdir(LOG_DIR);

    // Clean up old logs if space is low. Names are resolved per file at
    // open time now (openNextFile), so there is no counter to seed here.
    cleanupOldLogs();

    gLevel       = LogLevel::LEVEL_OFF;
    gActiveLevel = LogLevel::LEVEL_OFF;
    gLevelSetMs  = millis();
    gReady       = true;

    size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    Serial.printf("[LOG] Init OK, free: %u bytes\n", (unsigned)freeBytes);
    return true;
}

void shutdown() {
    closeFile();
    gReady       = false;
    gLevel       = LogLevel::LEVEL_OFF;
    gActiveLevel = LogLevel::LEVEL_OFF;
}

LogLevel getLevel() {
    return gLevel;
}

LogLevel getActiveLevel() {
    return gActiveLevel;
}

// Close whatever is open and, unless the target is OFF, start a fresh file at
// the new level. The only place gActiveLevel moves.
static void applyLevel(LogLevel level) {
    closeFile();
    gActiveLevel = level;
    if (gActiveLevel != LogLevel::LEVEL_OFF) {
        openNextFile();
    }
}

static const char* levelName(LogLevel l) {
    switch (l) {
        case LogLevel::LEVEL_LOW:  return "LOW";
        case LogLevel::LEVEL_MID:  return "MID";
        case LogLevel::LEVEL_HIGH: return "HIGH";
        default:                   return "OFF";
    }
}

void cycleLevel() {
    if (!gReady) return;

    LogLevel prev = gLevel;

    switch (gLevel) {
        case LogLevel::LEVEL_OFF:  gLevel = LogLevel::LEVEL_LOW;  break;
        case LogLevel::LEVEL_LOW:  gLevel = LogLevel::LEVEL_MID;  break;
        case LogLevel::LEVEL_MID:  gLevel = LogLevel::LEVEL_HIGH; break;
        case LogLevel::LEVEL_HIGH: gLevel = LogLevel::LEVEL_OFF;  break;
    }
    gLevelSetMs = millis();

    // Stopping is never deferred. The delay exists to stop a level the diver
    // only passed through from creating a file; OFF creates nothing, and
    // closing now is what makes the file available to the auto-upload. It also
    // means the file open on the way in gets closed rather than replaced: a
    // LOW -> HIGH -> OFF sweep ends with the LOW file intact and no HIGH file
    // at all.
    if (gLevel == LogLevel::LEVEL_OFF) {
        Serial.printf("[LOG] Level: %s -> OFF (immediate)\n", levelName(prev));
        applyLevel(gLevel);
        return;
    }

    // LOW/HIGH are a selection only — tick() opens the file once the diver has
    // stopped pressing for LOG_COMMIT_DELAY_MS.
    Serial.printf("[LOG] Level: %s -> %s (pending, %lu ms)\n",
                  levelName(prev), levelName(gLevel),
                  (unsigned long)LOG_COMMIT_DELAY_MS);
}

void setLevel(LogLevel level) {
    if (!gReady) return;
    gLevel      = level;
    gLevelSetMs = millis();
    if (gActiveLevel == level) return;
    applyLevel(level);
}

void tick() {
    if (!gReady) return;
    // Only ever LOW or HIGH: cycleLevel() applies OFF on the spot, so a pending
    // change here always ends in a file being opened.
    if (gLevel == gActiveLevel) return;                        // nothing pending
    if (millis() - gLevelSetMs < LOG_COMMIT_DELAY_MS) return;  // still settling

    Serial.printf("[LOG] Level committed: %s -> %s\n",
                  levelName(gActiveLevel), levelName(gLevel));
    applyLevel(gLevel);
}

const char* getLogPath() {
    return gLogPath;
}

const char* currentPath() {
    return gLogPath;
}

bool isLogging() {
    return gReady && gActiveLevel != LogLevel::LEVEL_OFF;
}

// Columns beyond LOW's common set, per level. Shared by log() and logImmediate()
// so the two can't drift apart on schema — they already duplicated the common
// set, and a third copy of the per-level tail is how a header stops matching its
// rows.
static void writeLevelColumns(const LogData& d) {
    if (gActiveLevel == LogLevel::LEVEL_LOW) {
        // Die temperature at every level: the mag readings are thermally
        // compensated with it, so a dive log without it can't be re-examined.
        gLogFile.printf(",%.2f", d.mag_temp_c);
    } else if (gActiveLevel == LogLevel::LEVEL_MID) {
        gLogFile.printf(",%.3f,%.3f,%.3f,%.2f,%.2f,%.2f",
                        d.mag_cal.x, d.mag_cal.y, d.mag_cal.z,
                        d.pitch_deg, d.roll_deg, d.mag_temp_c);
    } else if (gActiveLevel == LogLevel::LEVEL_HIGH) {
        gLogFile.printf(",%.3f,%.3f,%.3f"
                        ",%.3f,%.3f,%.3f"
                        ",%.3f,%.3f,%.3f"
                        ",%.3f,%.3f,%.3f"
                        ",%.3f,%.3f,%.3f"
                        ",%.3f,%.3f,%.3f"
                        ",%.2f,%.2f,%.2f",
                        d.mag_raw.x, d.mag_raw.y, d.mag_raw.z,
                        d.accel_raw.x, d.accel_raw.y, d.accel_raw.z,
                        d.gyro_raw.x, d.gyro_raw.y, d.gyro_raw.z,
                        d.mag_cal.x, d.mag_cal.y, d.mag_cal.z,
                        d.accel_cal.x, d.accel_cal.y, d.accel_cal.z,
                        d.gyro_cal.x, d.gyro_cal.y, d.gyro_cal.z,
                        d.pitch_deg, d.roll_deg, d.mag_temp_c);
    }
}

void log(const LogData& d) {
    if (!gReady || gActiveLevel == LogLevel::LEVEL_OFF || !gLogFile) return;

    // Throttle log rate per level.
    {
        static uint32_t lastLowLogMs  = 0;
        static uint32_t lastMidLogMs  = 0;
        static uint32_t lastHighLogMs = 0;
        uint32_t now = millis();
        if (gActiveLevel == LogLevel::LEVEL_LOW) {
            if (now - lastLowLogMs < LOG_LOW_INTERVAL_MS) return;
            lastLowLogMs = now;
        } else if (gActiveLevel == LogLevel::LEVEL_MID) {
            if (now - lastMidLogMs < LOG_MID_INTERVAL_MS) return;
            lastMidLogMs = now;
        } else if (gActiveLevel == LogLevel::LEVEL_HIGH) {
            if (now - lastHighLogMs < LOG_HIGH_INTERVAL_MS) return;
            lastHighLogMs = now;
        }
    }

    // Format local time if the system clock has been set (GPS or NTP).
    char localTimeBuf[24] = "";  // empty = no valid time source
    time_t now_t = time(nullptr);
    if (now_t >= CLOCK_VALID_EPOCH) {
        struct tm tm_info{};
        localtime_r(&now_t, &tm_info);
        strftime(localTimeBuf, sizeof(localTimeBuf), "%Y-%m-%dT%H:%M:%S", &tm_info);
    }

    // Common columns (LOW and HIGH)
    gLogFile.printf("%lu,%s,%.2f,%.3f,%c,%.2f,%.2f,%.8f,%.8f,%c",
                    d.timestamp_ms,
                    localTimeBuf,
                    d.heading_deg,
                    d.speed_ms,
                    d.gpsSpeed ? 'G' : 'F',
                    d.pos_x_m,
                    d.pos_y_m,
                    d.lat,
                    d.lon,
                    d.pos_src);

    if (d.pos_src == 'G') {
        gLogFile.printf(",%u,%.1f", d.gps_satellites, d.gps_hdop);
    } else {
        gLogFile.print(",,");
    }

    gLogFile.printf(",%.2f,%.2f", d.depth_m, d.water_temp_c);

    writeLevelColumns(d);

    gLogFile.print('\n');

    // Time-based flush to avoid SPI stalls from frequent flash erases.
    static uint32_t lastFlushMs = 0;
    uint32_t now = millis();
    if (now - lastFlushMs >= 30000) {
        lastFlushMs = now;
        gLogFile.flush();
    }
}

void logImmediate(const LogData& d) {
    if (!gReady || gActiveLevel == LogLevel::LEVEL_OFF || !gLogFile) return;

    char localTimeBuf[24] = "";
    time_t now_t = time(nullptr);
    if (now_t >= CLOCK_VALID_EPOCH) {
        struct tm tm_info{};
        localtime_r(&now_t, &tm_info);
        strftime(localTimeBuf, sizeof(localTimeBuf), "%Y-%m-%dT%H:%M:%S", &tm_info);
    }

    gLogFile.printf("%lu,%s,%.2f,%.3f,%c,%.2f,%.2f,%.8f,%.8f,%c",
                    d.timestamp_ms,
                    localTimeBuf,
                    d.heading_deg,
                    d.speed_ms,
                    d.gpsSpeed ? 'G' : 'F',
                    d.pos_x_m,
                    d.pos_y_m,
                    d.lat,
                    d.lon,
                    d.pos_src);

    if (d.pos_src == 'G') {
        gLogFile.printf(",%u,%.1f", d.gps_satellites, d.gps_hdop);
    } else {
        gLogFile.print(",,");
    }

    gLogFile.printf(",%.2f,%.2f", d.depth_m, d.water_temp_c);

    writeLevelColumns(d);

    gLogFile.print('\n');
    gLogFile.flush();
}

}  // namespace logging
