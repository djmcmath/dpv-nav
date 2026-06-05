#include "logging.h"
#include "../config.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <cstdio>
#include <time.h>

namespace logging {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool     gReady    = false;   // init() succeeded
static LogLevel gLevel    = LogLevel::LEVEL_OFF;
static File     gLogFile;
static char     gLogPath[32] = "";
static uint16_t gNextNum  = 1;       // next sequential file number

static constexpr size_t FREE_SPACE_THRESHOLD = 32768;  // 32 KB minimum free
static constexpr const char* LOG_DIR = "/logs";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build path for a given sequence number: /logs/001.csv
static void buildPath(char* buf, size_t bufLen, uint16_t num) {
    snprintf(buf, bufLen, "%s/%03u.csv", LOG_DIR, num);
}

// Scan /logs/ for the highest existing sequence number and set gNextNum.
// Also returns the lowest number found (for cleanup). Returns 0 if no files.
static uint16_t scanLogDir(uint16_t& lowestOut) {
    uint16_t highest = 0;
    uint16_t lowest  = 0xFFFF;
    File dir = LittleFS.open(LOG_DIR);
    if (!dir || !dir.isDirectory()) {
        lowestOut = 0;
        return 0;
    }
    File f = dir.openNextFile();
    while (f) {
        const char* name = f.name();  // e.g. "001.csv"
        // Parse leading digits
        uint16_t num = 0;
        const char* p = name;
        while (*p >= '0' && *p <= '9') {
            num = num * 10 + (*p - '0');
            p++;
        }
        if (num > 0) {
            if (num > highest) highest = num;
            if (num < lowest)  lowest  = num;
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
    lowestOut = (lowest == 0xFFFF) ? 0 : lowest;
    return highest;
}

// Delete lowest-numbered log files until free space >= threshold.
static void cleanupOldLogs() {
    size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    while (freeBytes < FREE_SPACE_THRESHOLD) {
        uint16_t lowest;
        uint16_t highest = scanLogDir(lowest);
        if (lowest == 0 || highest == 0) break;  // no log files left

        char path[32];
        buildPath(path, sizeof(path), lowest);
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
    if (gLevel == LogLevel::LEVEL_LOW) {
        gLogFile.print("timestamp_ms,local_time,heading_deg,speed_ms,speed_src,"
                       "pos_x_m,pos_y_m,lat,lon,pos_src\n");
    } else if (gLevel == LogLevel::LEVEL_HIGH) {
        gLogFile.print("timestamp_ms,local_time,heading_deg,speed_ms,speed_src,"
                       "pos_x_m,pos_y_m,lat,lon,pos_src,"
                       "mag_x_raw,mag_y_raw,mag_z_raw,"
                       "accel_x_raw,accel_y_raw,accel_z_raw,"
                       "gyro_x_raw,gyro_y_raw,gyro_z_raw,"
                       "mag_x_cal,mag_y_cal,mag_z_cal,"
                       "accel_x_cal,accel_y_cal,accel_z_cal,"
                       "gyro_x_cal,gyro_y_cal,gyro_z_cal,"
                       "pitch_deg,roll_deg\n");
    }
}

// Open a new log file with the next sequential number.
static bool openNextFile() {
    buildPath(gLogPath, sizeof(gLogPath), gNextNum);
    gLogFile = LittleFS.open(gLogPath, FILE_WRITE);
    if (!gLogFile) {
        Serial.printf("[LOG] Error: could not open %s\n", gLogPath);
        return false;
    }
    Serial.printf("[LOG] Opened %s (level %s)\n", gLogPath,
                  gLevel == LogLevel::LEVEL_LOW ? "LOW" : "HIGH");
    gNextNum++;
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

    // Determine next file number from existing files.
    uint16_t lowest;
    uint16_t highest = scanLogDir(lowest);
    gNextNum = highest + 1;

    // Clean up old logs if space is low.
    cleanupOldLogs();

    gLevel = LogLevel::LEVEL_OFF;
    gReady = true;

    size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    Serial.printf("[LOG] Init OK, next file: %s/%03u.csv, free: %u bytes\n",
                  LOG_DIR, gNextNum, (unsigned)freeBytes);
    return true;
}

void shutdown() {
    closeFile();
    gReady = false;
    gLevel = LogLevel::LEVEL_OFF;
}

LogLevel getLevel() {
    return gLevel;
}

void cycleLevel() {
    if (!gReady) return;

    // Close existing file when transitioning to OFF or changing level
    // (different level = different CSV schema = new file).
    LogLevel prev = gLevel;

    switch (gLevel) {
        case LogLevel::LEVEL_OFF:  gLevel = LogLevel::LEVEL_LOW;  break;
        case LogLevel::LEVEL_LOW:  gLevel = LogLevel::LEVEL_HIGH; break;
        case LogLevel::LEVEL_HIGH: gLevel = LogLevel::LEVEL_OFF;  break;
    }

    Serial.printf("[LOG] Level: %s -> %s\n",
                  prev == LogLevel::LEVEL_OFF ? "OFF" : (prev == LogLevel::LEVEL_LOW ? "LOW" : "HIGH"),
                  gLevel == LogLevel::LEVEL_OFF ? "OFF" : (gLevel == LogLevel::LEVEL_LOW ? "LOW" : "HIGH"));

    // Close previous file on any transition (schema changes between LOW/HIGH).
    closeFile();

    // Open new file if moving to an active level.
    if (gLevel != LogLevel::LEVEL_OFF) {
        openNextFile();
    }
}

void setLevel(LogLevel level) {
    if (!gReady || gLevel == level) return;
    closeFile();
    gLevel = level;
    if (gLevel != LogLevel::LEVEL_OFF) {
        openNextFile();
    }
}

bool isLogging() {
    return gReady && gLevel != LogLevel::LEVEL_OFF;
}

void log(const LogData& d) {
    if (!gReady || gLevel == LogLevel::LEVEL_OFF || !gLogFile) return;

    // Throttle log rate per level.
    {
        static uint32_t lastLowLogMs  = 0;
        static uint32_t lastHighLogMs = 0;
        uint32_t now = millis();
        if (gLevel == LogLevel::LEVEL_LOW) {
            if (now - lastLowLogMs < LOG_LOW_INTERVAL_MS) return;
            lastLowLogMs = now;
        } else if (gLevel == LogLevel::LEVEL_HIGH) {
            if (now - lastHighLogMs < LOG_HIGH_INTERVAL_MS) return;
            lastHighLogMs = now;
        }
    }

    // Format local time if the system clock has been set (GPS or NTP).
    // An unsynced ESP32 sits near epoch 0; any time >= Nov 2023 is real.
    char localTimeBuf[24] = "";  // empty = no valid time source
    time_t now_t = time(nullptr);
    if (now_t >= 1700000000L) {
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

    if (gLevel == LogLevel::LEVEL_HIGH) {
        gLogFile.printf(",%.3f,%.3f,%.3f"
                        ",%.3f,%.3f,%.3f"
                        ",%.3f,%.3f,%.3f"
                        ",%.3f,%.3f,%.3f"
                        ",%.3f,%.3f,%.3f"
                        ",%.3f,%.3f,%.3f"
                        ",%.2f,%.2f",
                        d.mag_raw.x, d.mag_raw.y, d.mag_raw.z,
                        d.accel_raw.x, d.accel_raw.y, d.accel_raw.z,
                        d.gyro_raw.x, d.gyro_raw.y, d.gyro_raw.z,
                        d.mag_cal.x, d.mag_cal.y, d.mag_cal.z,
                        d.accel_cal.x, d.accel_cal.y, d.accel_cal.z,
                        d.gyro_cal.x, d.gyro_cal.y, d.gyro_cal.z,
                        d.pitch_deg, d.roll_deg);
    }

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
    if (!gReady || gLevel == LogLevel::LEVEL_OFF || !gLogFile) return;

    char localTimeBuf[24] = "";
    time_t now_t = time(nullptr);
    if (now_t >= 1700000000L) {
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

    if (gLevel == LogLevel::LEVEL_HIGH) {
        gLogFile.printf(",%.3f,%.3f,%.3f"
                        ",%.3f,%.3f,%.3f"
                        ",%.3f,%.3f,%.3f"
                        ",%.3f,%.3f,%.3f"
                        ",%.3f,%.3f,%.3f"
                        ",%.3f,%.3f,%.3f"
                        ",%.2f,%.2f",
                        d.mag_raw.x, d.mag_raw.y, d.mag_raw.z,
                        d.accel_raw.x, d.accel_raw.y, d.accel_raw.z,
                        d.gyro_raw.x, d.gyro_raw.y, d.gyro_raw.z,
                        d.mag_cal.x, d.mag_cal.y, d.mag_cal.z,
                        d.accel_cal.x, d.accel_cal.y, d.accel_cal.z,
                        d.gyro_cal.x, d.gyro_cal.y, d.gyro_cal.z,
                        d.pitch_deg, d.roll_deg);
    }

    gLogFile.print('\n');
    gLogFile.flush();
}

}  // namespace logging
