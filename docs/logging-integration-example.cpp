// Example: Integration of data logging into main.cpp
// This file shows how to wire up the logging system with your sensor reads
// Copy the relevant parts into your main.cpp

#include <Wire.h>
#include <math.h>
#include <Arduino.h>
#include "main.h"
#include "board_pins.h"
#include "./sensors/imu.h"
#include "./nav/ui_controller.h"
#include "./types/types.h"
#include "./math/mahony.h"
#include "./sensors/calib.h"
#include "./util/logging.h"  // <-- ADD THIS LINE

MahonyState ahrs;
MahonyParams params{ .kp = 2.0f, .ki = 0.0f, .useMag = true };

Calib3 gyroCal{{0,0,0},{1,1,1}};
Calib3 accelCal{{0,0,0},{1,1,1}};
MagCalib magCal{{0,0,0}, { {1,0,0},{0,1,0},{0,0,1} }};
imu::ImuConfig imuConfig{ .accel_g_fullscale = 16.0f,
                         .gyro_dps_fullscale = 2000.0f,
                         .mag_uT_fullscale = 0.0f,
                         .sample_hz = 100 };
imu::AxisMap imuAxisMap{ .x_axis = +1, .y_axis = +2, .z_axis = +3 };

namespace dpvnav {

void setup() {
  Serial.begin(115200);
  Serial.print("Waiting for Serial");
  while (!Serial) {
    delay(10);
    Serial.print(".");
  }

  Serial.println("Initializing I2C and IMU...");

  if (!imu::init(imuConfig, imuAxisMap)) {
    Serial.println("Error initializing IMU");
    while (1);
  }
  delay(50);
  
  mahonyInit(ahrs);

  // ========== ADD DATA LOGGING INITIALIZATION ==========
  Serial.println("Initializing data logging...");
  if (!logging::init()) {
    Serial.println("Warning: Could not initialize logging");
    // Note: don't fatal error here, system can continue without logging
  }
  // ==================================================
}

void loop() {
  static uint32_t lastMicros = micros();
  uint32_t now = micros();
  float dt = (now - lastMicros) / 1e6f;
  lastMicros = now;

  // Read raw sensor data
  imu::Vec3i16 magRawRead, accelRawRead, gyroRawRead;
  if (imu::readMagRaw(magRawRead) != imu::ImuStatus::Ok) {
    Serial.println("Error reading magnetometer");
    delay(1000);
    return;
  }
  if (imu::readAccelRaw(accelRawRead) != imu::ImuStatus::Ok) {
    Serial.println("Error reading accelerometer");
    delay(1000);
    return;
  }
  if (imu::readGyroRaw(gyroRawRead) != imu::ImuStatus::Ok) {
    Serial.println("Error reading gyroscope");
    delay(1000);
    return;
  }
  
  // Calculate heading (crude method)
  float fx = (float)magRawRead.x;
  float fy = (float)magRawRead.y;
  float fz = (float)magRawRead.z;
  float headingRad = atan2(fy, fx);
  float headingDeg = headingRad * 180.0f / PI;
  if (headingDeg < 0) {
    headingDeg += 360.0f;
  }

  // ========== ADD DATA LOGGING ==========
  // Apply calibrations to convert raw to calibrated values
  imu::Vec3f magCal_out = {
    (magRawRead.x - magCal.bias.x),
    (magRawRead.y - magCal.bias.y),
    (magRawRead.z - magCal.bias.z)
  };
  
  imu::Vec3f accelCal_out = {
    (accelRawRead.x - accelCal.bias.x) * accelCal.scale.x,
    (accelRawRead.y - accelCal.bias.y) * accelCal.scale.y,
    (accelRawRead.z - accelCal.bias.z) * accelCal.scale.z
  };
  
  imu::Vec3f gyroCal_out = {
    (gyroRawRead.x - gyroCal.bias.x) * gyroCal.scale.x,
    (gyroRawRead.y - gyroCal.bias.y) * gyroCal.scale.y,
    (gyroRawRead.z - gyroCal.bias.z) * gyroCal.scale.z
  };
  
  // Create and log entry
  logging::LogEntry entry{
    .timestamp_ms = millis(),
    .mag_raw = magRawRead,
    .accel_raw = accelRawRead,
    .gyro_raw = gyroRawRead,
    .mag_cal = magCal_out,
    .accel_cal = accelCal_out,
    .gyro_cal = gyroCal_out,
    .heading_deg = headingDeg,
    .roll_deg = 0.0f,   // TODO: extract from AHRS quaternion
    .pitch_deg = 0.0f   // TODO: extract from AHRS quaternion
  };
  
  if (logging::isLogging()) {
    if (!logging::logEntry(entry)) {
      Serial.println("Error: Failed to log entry");
    }
  }
  // ========================================

  ui::console_update(magRawRead, accelRawRead, gyroRawRead, headingDeg);

  delay(200);  // 5 Hz display update
}

}  // namespace dpvnav
