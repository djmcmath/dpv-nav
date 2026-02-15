#include "logging.h"
#include <Arduino.h>
#include <SPIFFS.h>
#include <cstdio>
#include <time.h>

namespace logging {

// Static state
static bool gInitialized = false;
static File gLogFile;// = nullptr;
static char gLogPath[64] = "";
static uint32_t gBytesWritten = 0;
static uint32_t gStartTime = 0;

// Helper to generate unique log filename
static void generateLogFilename(char* buffer, size_t bufsize) {
  // Use timestamp from internal clock
  uint32_t elapsed = millis() - gStartTime;
  uint32_t seconds = elapsed / 1000;
  uint32_t minutes = seconds / 60;
  
  // Format: /logs/log_<minutes>_<seconds>.csv
  snprintf(buffer, bufsize, "/log_%lu_%lu.csv", minutes, seconds % 60);
}

bool init() {
  Serial.println("[LOG] Initializing logging system...");
  if (gInitialized) {
    Serial.println("[LOG] Warning: Logging already initialized");
    return true;
  }

  // Initialize SPIFFS
  if (!SPIFFS.begin(false)) {  // true = format if mount fails
    Serial.println("[LOG] Warning: SPIFFS mount failed, attempting format...");
    SPIFFS.format();
    if (!SPIFFS.begin(false)) {
        Serial.println("[LOG] Error: SPIFFS mount failed after format");
        return false;
    } else {
        Serial.println("[LOG] SPIFFS mounted successfully after format");
    }
  } else {
    Serial.println("[LOG] SPIFFS mounted successfully");
  }

  // Report remaining space on SPIFFS
    size_t totalBytes = SPIFFS.totalBytes();
    size_t usedBytes = SPIFFS.usedBytes();
    size_t freeBytes = totalBytes - usedBytes;
    Serial.print("[LOG] SPIFFS bytes free: ");
    //Serial.print(totalBytes);
    //Serial.print(" bytes, used: ");
    //Serial.print(usedBytes);
    //Serial.print(" bytes, free: ");
    Serial.print(freeBytes);
    Serial.println(" bytes");

  // Clear existing log files to free up space
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  uint32_t filesDeleted = 0;
  while (file) {
    String filename = file.name();
    Serial.print("[LOG] Found file: ");
    Serial.println(filename);
    //Serial.print("[LOG] indexOf(\"log_\") = ");
    //Serial.println(filename.indexOf("log_"));
    //Serial.print("[LOG] endsWith(.csv) = ");
    //Serial.println(filename.endsWith(".csv") ? "true" : "false");
    file.close();
    // Delete all files matching log pattern (e.g., "/log_*.csv")
    if (filename.indexOf("log_") >= 0 && filename.endsWith(".csv")) {
        String path = filename;
        if (!path.startsWith("/")) {
            path = "/" + path;
        }
        if (SPIFFS.remove(path)) {
            filesDeleted++;
            Serial.print("[LOG] Deleted old log file: ");
            Serial.println(path);
        } else {
            Serial.print("[LOG] Warning: Could not delete file: ");
            Serial.println(path);
        }
    }
    file = root.openNextFile();
  }
  root.close();
  if (filesDeleted > 0) {
    Serial.print("[LOG] Cleared ");
    Serial.print(filesDeleted);
    Serial.println(" old log files");
  }
  
  // Generate filename and open file
  gStartTime = millis();
  generateLogFilename(gLogPath, sizeof(gLogPath));
  
  gLogFile = SPIFFS.open(gLogPath, FILE_WRITE);
  if (!gLogFile) {
    Serial.print("[LOG] Error: Could not open log file: ");
    Serial.println(gLogPath);
    SPIFFS.end();
    return false;
  }

  // Write CSV header
  gLogFile.print("timestamp_ms,"
                    "mag_x_raw,mag_y_raw,mag_z_raw,"
                    "accel_x_raw,accel_y_raw,accel_z_raw,"
                    "gyro_x_raw,gyro_y_raw,gyro_z_raw,"
                    "mag_x_cal,mag_y_cal,mag_z_cal,"
                    "accel_x_cal,accel_y_cal,accel_z_cal,"
                    "gyro_x_cal,gyro_y_cal,gyro_z_cal,"
                    "heading_deg,roll_deg,pitch_deg\n");
  
  gBytesWritten = 0;
  gInitialized = true;

  Serial.print("[LOG] Logging initialized to: ");
  Serial.println(gLogPath);
  
  return true;
}

void shutdown() {
  if (!gInitialized) {
    return;
  }

  if (gLogFile) {
    gLogFile.flush();
    gLogFile.close();
    //fflush(gLogFile);
    //fclose(gLogFile);
    //gLogFile = nullptr;
  }

  SPIFFS.end();
  gInitialized = false;
  gBytesWritten = 0;

  Serial.println("[LOG] Logging shutdown");
}

bool logEntry(const LogEntry& entry) {
  if (!gInitialized || !gLogFile) {
    return false;
  }

  // Write CSV line: timestamp, raw values, calibrated values, processed data
  uint32_t written = gLogFile.printf(
    "%lu,"
    "%d,%d,%d,"
    "%d,%d,%d,"
    "%d,%d,%d,"
    "%.3f,%.3f,%.3f,"
    "%.3f,%.3f,%.3f,"
    "%.3f,%.3f,%.3f,"
    "%.2f,%.2f,%.2f\n",
    entry.timestamp_ms,
    entry.mag_raw.x, entry.mag_raw.y, entry.mag_raw.z,
    entry.accel_raw.x, entry.accel_raw.y, entry.accel_raw.z,
    entry.gyro_raw.x, entry.gyro_raw.y, entry.gyro_raw.z,
    entry.mag_cal.x, entry.mag_cal.y, entry.mag_cal.z,
    entry.accel_cal.x, entry.accel_cal.y, entry.accel_cal.z,
    entry.gyro_cal.x, entry.gyro_cal.y, entry.gyro_cal.z,
    entry.heading_deg, entry.roll_deg, entry.pitch_deg
  );

  if (written > 0) {
    gBytesWritten += written;
    // Periodically flush to avoid data loss
    if (gBytesWritten % 1024 < (size_t)written) {
      gLogFile.flush();
    }
    return true;
  }

  return false;
}

const char* getLogPath() {
  return gLogPath;
}

uint32_t getBytesLogged() {
  return gBytesWritten;
}

bool isLogging() {
  return gInitialized; // && (gLogFile != nullptr);
}

bool rotateLog() {
  if (!gInitialized) {
    return false;
  }

  // Close current file
  if (gLogFile) {
    gLogFile.flush();
    gLogFile.close();
    //gLogFile = nullptr;
  }

  // Generate new filename and open new file
  generateLogFilename(gLogPath, sizeof(gLogPath));
  
  gLogFile = SPIFFS.open(gLogPath, FILE_WRITE);
  if (!gLogFile) {
    Serial.print("[LOG] Error: Could not open new log file: ");
    Serial.println(gLogPath);
    gInitialized = false;
    return false;
  }

  // Write CSV header to new file
  gLogFile.print("timestamp_ms,"
                    "mag_x_raw,mag_y_raw,mag_z_raw,"
                    "accel_x_raw,accel_y_raw,accel_z_raw,"
                    "gyro_x_raw,gyro_y_raw,gyro_z_raw,"
                    "mag_x_cal,mag_y_cal,mag_z_cal,"
                    "accel_x_cal,accel_y_cal,accel_z_cal,"
                    "gyro_x_cal,gyro_y_cal,gyro_z_cal,"
                    "heading_deg,roll_deg,pitch_deg\n");
  
  gBytesWritten = 0;

  Serial.print("[LOG] Rotated to new log file: ");
  Serial.println(gLogPath);
  
  return true;
}

}  // namespace logging
