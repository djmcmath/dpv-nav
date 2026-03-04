#include "mag_cal_collect.h"
#include "../sensors/imu.h"
#include <LittleFS.h>
#include <Arduino.h>

namespace mag_cal {

static const char* CAL_DATA_PATH = "/mag_cal_samples.csv";
static bool collecting = false;
static uint32_t collection_start = 0;
static uint32_t collection_duration = 0;
static File cal_file;
static uint32_t sample_count = 0;

bool startCollection(uint32_t duration_ms) {
  if (collecting) {
    Serial.println("[MAG_CAL] Already collecting!");
    return false;
  }

  // Ensure LittleFS mounted
  if (!LittleFS.begin(true)) {
    Serial.println("[MAG_CAL] Error: LittleFS mount failed");
    return false;
  }

  // Open file for writing
  cal_file = LittleFS.open(CAL_DATA_PATH, FILE_WRITE);
  if (!cal_file) {
    Serial.println("[MAG_CAL] Error: Could not open calibration file");
    return false;
  }

  // Write CSV header
  cal_file.println("X,Y,Z");

  collecting = true;
  collection_start = millis();
  collection_duration = duration_ms;
  sample_count = 0;

  Serial.println("\n=========================================================");
  Serial.println("MAGNETOMETER CALIBRATION DATA COLLECTION");
  Serial.println("===========================================================");
  Serial.print("Duration: ");
  Serial.print(duration_ms / 1000);
  Serial.println(" seconds");
  Serial.println("\n*** ROTATE DEVICE THROUGH ALL ORIENTATIONS ***");
  Serial.println("    - Slow, smooth rotations");
  Serial.println("    - Cover all pitch/roll/yaw angles");
  Serial.println("    - Try to trace sphere in 3D space");
  Serial.println("==========================================================\n");

  return true;
}

void stopCollection() {
  if (!collecting) {
    return;
  }

  collecting = false;
  cal_file.close();

  Serial.println("\n=========================================================");
  Serial.println("CALIBRATION DATA COLLECTION COMPLETE");
  Serial.println("===========================================================");
  Serial.print("Samples collected: ");
  Serial.println(sample_count);
  Serial.print("Data saved to: ");
  Serial.println(CAL_DATA_PATH);
  Serial.println("\nNext steps:");
  Serial.println("  1. Type 'dump_cal' to export data via serial");
  Serial.println("  2. Copy-paste output to mag_samples.csv on PC");
  Serial.println("  3. Run: python mag_calibration.py mag_samples.csv");
  Serial.println("  4. Upload generated calib_mag_cal.json to ESP32");
  Serial.println("==========================================================\n");
}

bool isCollecting() {
  // Auto-stop if duration elapsed
  if (collecting && (millis() - collection_start >= collection_duration)) {
    stopCollection();
  }
  return collecting;
}

void dumpToSerial() {
  // Open file for reading
  File f = LittleFS.open(CAL_DATA_PATH, FILE_READ);
  if (!f) {
    Serial.println("[MAG_CAL] Error: Could not open calibration file");
    Serial.print("[MAG_CAL] Path: ");
    Serial.println(CAL_DATA_PATH);
    return;
  }

  Serial.println("\n=========================================================");
  Serial.println("MAGNETOMETER CALIBRATION DATA DUMP");
  Serial.println("===========================================================");
  Serial.println("Copy everything between START and END markers:\n");
  Serial.println("--- START DATA ---");

  // Stream file contents to serial
  while (f.available()) {
    Serial.write(f.read());
  }

  Serial.println("--- END DATA ---\n");
  Serial.println("===========================================================");
  Serial.print("File size: ");
  Serial.print(f.size());
  Serial.println(" bytes");
  Serial.println("==========================================================\n");
  f.close();
}

void clearData() {
  if (LittleFS.remove(CAL_DATA_PATH)) {
    Serial.println("[MAG_CAL] Calibration data cleared");
  } else {
    Serial.println("[MAG_CAL] Error: Could not delete calibration data");
  }
}

// Call this from main loop when collecting
// Returns true if sample was logged
bool logSample() {
  if (!collecting) {
    return false;
  }

  // Check if duration elapsed
  if (millis() - collection_start >= collection_duration) {
    stopCollection();
    return false;
  }

  // Read raw magnetometer
  imu::Vec3i16 raw;
  if (imu::readMagRaw(raw) != imu::ImuStatus::Ok) {
    return false;
  }

  // Write to file as CSV
  cal_file.print(raw.x);
  cal_file.print(",");
  cal_file.print(raw.y);
  cal_file.print(",");
  cal_file.println(raw.z);

  sample_count++;

  // Progress indicator every 50 samples
  if (sample_count % 50 == 0) {
    uint32_t elapsed = millis() - collection_start;
    uint32_t remaining = collection_duration - elapsed;
    Serial.print("[MAG_CAL] Samples: ");
    Serial.print(sample_count);
    Serial.print(" | Time remaining: ");
    Serial.print(remaining / 1000);
    Serial.println("s");
  }

  return true;
}

}  // namespace mag_cal
