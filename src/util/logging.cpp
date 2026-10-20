#include "logging.h"
#include "log_names.h"
#include "log_idle.h"
#include "boot_log.h"
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
// is, and it alone decides the file format and schema.
static LogLevel gLevel       = LogLevel::LEVEL_OFF;
static LogLevel gActiveLevel = LogLevel::LEVEL_OFF;
static uint32_t gLevelSetMs  = 0;    // millis() of the last cycleLevel()

static File     gLogFile;
static char     gLogPath[LOG_PATH_MAX] = "";

// timestamp_ms of the last 'G' row written to the open file, 0 = none yet.
// Backs gpsRowWrittenSince() -- the nav board's solid-green status LED.
static uint32_t gLastGpsRowMs = 0;

static constexpr size_t FREE_SPACE_THRESHOLD = 32768;  // 32 KB minimum free
static constexpr const char* LOG_DIR = "/logs";

// Rolling-log cleanup (cleanupOldLogs() below) prefers to delete a log that
// never recorded a dive -- just a GPS-only track, e.g. left logging after the
// diver forgot to stop -- over one that did, even when the dive log is older.
// depth_m is column 13 (0-based 12) in every CSV schema this firmware has
// written, and at LOG_RECORD_DEPTH_OFFSET in a binary record. A unit with no depth sensor logs depth_m as 0.0
// for every row regardless of level (see nav_main.cpp's depth::isPresent()
// checks), so every log on it reads as non-dive and cleanup quietly falls
// back to today's plain oldest-first order -- not a regression, just no
// signal to prioritize on.
static constexpr float NON_DIVE_DEPTH_THRESHOLD_M = 2.0f;
static constexpr int   DEPTH_COLUMN_INDEX         = 12;

// An unsynced ESP32 sits near epoch 0; any time >= Nov 2023 came from a real
// source (GPS or NTP). Used both for the date prefix below and for the
// local_time column.
static constexpr time_t CLOCK_VALID_EPOCH = 1700000000L;

// Log files are named /logs/YYYYMMDD-NNN.csv (LOW) or .bin (MID/HIGH) -- see
// util/log_names.h for the grammar and why it is dated. Sequence numbers are resolved per file at open
// time from what is on disk (openNextFile), not from a counter, so deleting a
// log can never hand its name to a later one.
static constexpr uint16_t LOG_SEQ_MAX = 999;

// MID and HIGH write fixed binary records (log_record.h); LOW writes CSV.
static bool levelIsBinary(LogLevel l) {
    return l == LogLevel::LEVEL_MID || l == LogLevel::LEVEL_HIGH;
}

// LOW and MID thin out to a heartbeat while the unit is idle on the surface;
// HIGH never does. See util/log_idle.h.
static bool levelIsGated(LogLevel l) {
    return l == LogLevel::LEVEL_LOW || l == LogLevel::LEVEL_MID;
}

static log_idle::Gate  gIdleGate;
static log_idle::Pacer gPacer;
static LogData         gHeldSample{};   // newest idle sample not written, see log_idle::Pacer

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
    char     newestPrefix[log_names::PREFIX_LEN + 1] = "";  // highest date prefix present ("" = no dated logs)
    uint16_t highestSeq           = 0;   // highest NNN under `seqPrefix` (0 = none)
};

// `seqPrefix` may be nullptr when the caller only wants newestPrefix.
static void scanLogDir(LogScan& out, const char* seqPrefix) {
    File dir = LittleFS.open(LOG_DIR);
    if (!dir || !dir.isDirectory()) return;

    File f = dir.openNextFile();
    while (f) {
        const char* name = f.name();  // e.g. "20260908-001.csv"
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

// True if `path` never recorded a depth past NON_DIVE_DEPTH_THRESHOLD_M --
// i.e. is a GPS-only track with no dive in it. Stops at the first qualifying
// sample rather than reading the whole file. An unparsable or empty file
// (no header, no rows) has no depth evidence either way, so it also reads as
// non-dive -- there's nothing in it worth protecting.
static bool isNonDiveLog(const char* path) {
    File f = LittleFS.open(path, FILE_READ);
    if (!f) return true;

    if (log_names::isBinary(path)) {
        // Sizes come from the file's own header, not this build's structs, so
        // a log written by a later record version is still read correctly.
        LogFileHeader h{};
        bool ok = f.read((uint8_t*)&h, sizeof(h)) == sizeof(h) &&
                  memcmp(h.magic, LOG_BIN_MAGIC, sizeof(h.magic)) == 0 &&
                  h.header_size >= sizeof(h) &&
                  h.record_size >= LOG_RECORD_DEPTH_OFFSET + sizeof(int16_t);
        const int16_t threshold = (int16_t)(NON_DIVE_DEPTH_THRESHOLD_M / LOG_SCALE_DEPTH);
        for (uint32_t pos = h.header_size; ok; pos += h.record_size) {
            int16_t depth;
            if (!f.seek(pos + LOG_RECORD_DEPTH_OFFSET) ||
                f.read((uint8_t*)&depth, sizeof(depth)) != sizeof(depth)) break;
            if (depth >= threshold) {   // LOG_NAN_I16 is the lowest value, so never
                f.close();
                return false;
            }
        }
        f.close();
        return true;
    }

    f.readStringUntil('\n');  // header
    while (f.available()) {
        String line = f.readStringUntil('\n');
        int col = 0, start = 0;
        for (int i = 0; i <= (int)line.length(); i++) {
            if (i < (int)line.length() && line[i] != ',') continue;
            if (col == DEPTH_COLUMN_INDEX) {
                if (line.substring(start, i).toFloat() >= NON_DIVE_DEPTH_THRESHOLD_M) {
                    f.close();
                    return false;
                }
                break;
            }
            col++;
            start = i + 1;
        }
    }
    f.close();
    return true;
}

// Find the oldest log basename under LOG_DIR into `out` ("" if nothing
// qualifies), skipping the file currently open for writing -- it is the one
// log that cannot be re-collected, and on a nearly full unit it can be the
// only file left to consider. Also skips boot_log::protectedLogPath(): on a
// fresh boot gLogPath is always "" (no file has been reopened yet -- that
// happens later, once NVS restores the log level), so without this second
// check a brownout mid-dive leaves the half-written log looking exactly like
// any other stale file the moment this runs. When `nonDiveOnly` is set, only
// logs that fail isNonDiveLog's test are eligible. "Oldest" is decided by
// name alone (log_names::olderThan) -- LittleFS timestamps are only as good
// as the clock was when the file was written, and on this unit that clock is
// often unset. See util/log_names.h.
static void findOldestLog(char* out, size_t outLen, bool nonDiveOnly) {
    out[0] = '\0';
    const char* protectedPath = boot_log::protectedLogPath();
    File dir = LittleFS.open(LOG_DIR);
    if (!dir || !dir.isDirectory()) return;

    File f = dir.openNextFile();
    while (f) {
        const char* name = f.name();  // e.g. "20260908-001.csv"
        char path[LOG_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", LOG_DIR, name);
        // Age check before the (expensive, file-content) non-dive check so a
        // candidate that can't possibly beat the current best never pays for it.
        if (strcmp(path, gLogPath) != 0 &&
            strcmp(path, protectedPath) != 0 &&
            (out[0] == '\0' || log_names::olderThan(name, out)) &&
            (!nonDiveOnly || isNonDiveLog(path))) {
            snprintf(out, outLen, "%s", name);
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
}

// Delete log files until free space >= threshold, preferring non-dive logs
// (see isNonDiveLog) over dive logs regardless of age, and within each of
// those two groups, oldest first.
static void cleanupOldLogs() {
    size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    while (freeBytes < FREE_SPACE_THRESHOLD) {
        char victim[LOG_NAME_MAX];
        findOldestLog(victim, sizeof(victim), /*nonDiveOnly=*/true);
        if (victim[0] == '\0') {
            findOldestLog(victim, sizeof(victim), /*nonDiveOnly=*/false);
        }
        if (victim[0] == '\0') break;  // no log files left (other than the active one)

        char path[LOG_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", LOG_DIR, victim);

        uint32_t victimSize = 0;
        {
            File vf = LittleFS.open(path, FILE_READ);
            if (vf) { victimSize = vf.size(); vf.close(); }
        }

        if (LittleFS.remove(path)) {
            Serial.printf("[LOG] Deleted old log %s (free space: %u)\n", path,
                          (unsigned)(LittleFS.totalBytes() - LittleFS.usedBytes()));
            boot_log::recordCleanupDeletion(path, victimSize, /*wasProtected=*/false);
        } else {
            break;  // can't delete, stop trying
        }
        freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    }
}

// Write the file header for the current level: the binary preamble for
// MID/HIGH, the CSV column names for LOW.
static void writeHeader() {
    if (!gLogFile) return;
    if (levelIsBinary(gActiveLevel)) {
        LogFileHeader h = makeLogFileHeader(static_cast<uint8_t>(gActiveLevel));
        gLogFile.write((const uint8_t*)&h, sizeof(h));
    } else {
        gLogFile.print("timestamp_ms,local_time,heading_deg,speed_ms,speed_src,"
                       "pos_x_m,pos_y_m,lat,lon,pos_src,"
                       "gps_satellites,gps_hdop,depth_m,water_temp_c,mag_temp_c\n");
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
    snprintf(gLogPath, sizeof(gLogPath), "%s/%s-%03u%s", LOG_DIR, prefix,
             (unsigned)(scan.highestSeq + 1),
             levelIsBinary(gActiveLevel) ? log_names::EXT_BIN : log_names::EXT_CSV);

    gLogFile = LittleFS.open(gLogPath, FILE_WRITE);
    if (!gLogFile) {
        Serial.printf("[LOG] Error: could not open %s\n", gLogPath);
        gLogPath[0] = '\0';
        return false;
    }
    Serial.printf("[LOG] Opened %s (level %s)\n", gLogPath, levelName(gActiveLevel));
    boot_log::setActiveLogPath(gLogPath);
    writeHeader();
    // A new file always starts at full rate; idle has to be earned again.
    gIdleGate.reset();
    gPacer.reset();
    gLastGpsRowMs = 0;  // and has no GPS fix in it until one is written
    return true;
}

// Close current file if open. A deliberate close (OFF, a level change, or
// POWER_OFF) means the file is complete, so it no longer needs crash
// protection on the next boot's cleanup pass.
static void closeFile() {
    if (gLogFile) {
        gLogFile.flush();
        gLogFile.close();
        Serial.printf("[LOG] Closed %s\n", gLogPath);
        boot_log::clearActiveLogPath();
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

    // LOW/MID/HIGH are a selection only — tick() opens the file once the diver has
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
    // Never OFF: cycleLevel() applies OFF on the spot, so a pending
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

// Local wall-clock time, or tm_year == 0 when no clock has been set (GPS or NTP).
static bool localNow(struct tm& out) {
    time_t now_t = time(nullptr);
    if (now_t < CLOCK_VALID_EPOCH) return false;
    localtime_r(&now_t, &out);
    return true;
}

// Seconds since 1970 of a broken-down *local* time, i.e. timegm() of it. The
// binary record stores this rather than the UTC epoch so the decoder can print
// the same local_time the CSV carries without knowing the unit's time zone.
static uint32_t localSeconds(const struct tm& t) {
    // Days from 1970-01-01 to the civil date (Howard Hinnant's algorithm).
    int y = t.tm_year + 1900, m = t.tm_mon + 1, d = t.tm_mday;
    y -= m <= 2;
    int      era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    return (uint32_t)(days * 86400L + t.tm_hour * 3600L + t.tm_min * 60L + t.tm_sec);
}

// Write one row in the open file's format. Shared by log() and logImmediate()
// so the two can't drift apart on schema.
static void writeRowUnflushed(const LogData& d);

// The first 'G' row after a gap -- the first fix in a file, or the first after
// surfacing -- is flushed at once rather than on the 30 s timer, so the status
// LED's "fix logged" means on flash, not in a buffer a brownout would lose.
// One flush per acquisition; rows inside a run of fixes take the timer.
static void writeRow(const LogData& d) {
    writeRowUnflushed(d);
    if (d.pos_src != 'G') return;
    const bool afterGap = gLastGpsRowMs == 0 || d.timestamp_ms - gLastGpsRowMs > GPS_FIX_STALE_MS;
    gLastGpsRowMs = d.timestamp_ms;
    if (afterGap) gLogFile.flush();
}

static void writeRowUnflushed(const LogData& d) {
    struct tm tm_info{};
    bool haveClock = localNow(tm_info);

    if (levelIsBinary(gActiveLevel)) {
        LogRecord r = packLogRecord(d, haveClock ? localSeconds(tm_info) : 0);
        gLogFile.write((const uint8_t*)&r, sizeof(r));
        return;
    }

    char localTimeBuf[24] = "";  // empty = no valid time source
    if (haveClock) strftime(localTimeBuf, sizeof(localTimeBuf), "%Y-%m-%dT%H:%M:%S", &tm_info);

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

    // Whenever there is a fix, not only on 'G' rows: a fix the confidence gate
    // rejected is an 'E' row, and its HDOP is the record of why. The binary
    // record always carries these; dive-map reads neither.
    if (d.gps_satellites > 0) {
        gLogFile.printf(",%u,%.1f", d.gps_satellites, d.gps_hdop);
    } else {
        gLogFile.print(",,");
    }

    // Die temperature at every level: the mag readings are thermally
    // compensated with it, so a dive log without it can't be re-examined.
    gLogFile.printf(",%.2f,%.2f,%.2f\n", d.depth_m, d.water_temp_c, d.mag_temp_c);
}

void log(const LogData& d) {
    if (!gReady || gActiveLevel == LogLevel::LEVEL_OFF || !gLogFile) return;

    // The gate sees every sample, not just the ones that get written -- that
    // is what lets the first sample that breaks idle be logged at once rather
    // than up to a heartbeat late.
    bool idle = false;
    if (levelIsGated(gActiveLevel)) {
        static constexpr log_idle::Params kIdleParams{
            LOG_IDLE_MAX_DEPTH_M, LOG_IDLE_MAX_SPEED_MS, LOG_IDLE_MAX_DRIFT_M, LOG_IDLE_ENTER_MS};
        idle = gIdleGate.update({d.timestamp_ms, d.depth_m, d.speed_ms, d.pos_src == 'G',
                                 d.pos_x_m, d.pos_y_m}, kIdleParams);
    }

    // Throttle log rate per level.
    uint32_t interval = gActiveLevel == LogLevel::LEVEL_LOW ? LOG_LOW_INTERVAL_MS
                      : gActiveLevel == LogLevel::LEVEL_MID ? LOG_MID_INTERVAL_MS
                      : LOG_HIGH_INTERVAL_MS;
    log_idle::Pacer::Plan plan = gPacer.step(d.timestamp_ms, idle, interval, LOG_IDLE_HEARTBEAT_MS);
    if (plan.flushHeld) writeRow(gHeldSample);
    if (plan.hold) gHeldSample = d;
    if (!plan.write) return;

    writeRow(d);

    // Time-based flush to avoid SPI stalls from frequent flash erases.
    static uint32_t lastFlushMs = 0;
    uint32_t now = millis();
    if (now - lastFlushMs >= 30000) {
        lastFlushMs = now;
        gLogFile.flush();
    }
}

bool gpsRowWrittenSince(uint32_t sinceMs) {
    return gLogFile && gLastGpsRowMs != 0 && (int32_t)(gLastGpsRowMs - sinceMs) >= 0;
}

void logImmediate(const LogData& d) {
    if (!gReady || gActiveLevel == LogLevel::LEVEL_OFF || !gLogFile) return;
    writeRow(d);
    gLogFile.flush();
}

}  // namespace logging
