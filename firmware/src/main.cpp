#include <Wire.h>
#include <math.h>
#include <Arduino.h>
#include "main.h"
#include "board_pins.h"
#include "sensors/imu.h"
#include "nav/ui_controller.h"
#include "types/types.h"

namespace dpvnav {

void setup() {
  Serial.begin(115200);
  Serial.print("Waiting for Serial");
  while (!Serial) {
    delay(10);
    Serial.print(".");
  }

  Serial.println("Initializing I2C and LIS3MDL magnetometer...");

  imu::init();
  delay(50);

  imu::initMag();
}

void loop() {

  static uint32_t lastMicros = micros();
  uint32_t now = micros();
  float dt = (now - lastMicros) / 1e6f;
  lastMicros = now;

  //sensors::readMagRaw(dt);       // reads IMU, flow sensor
  int16_t mx, my, mz;
  mag_reading reading = imu::readMagRaw(mx, my, mz);
  //math::updateOrientation(dt);  // runs Mahony/Madgwick
  //nav::updatePosition(dt);      // integrates x,y using speed + yaw
  
  // Convert to float for math
  float fx = (float)mx;
  float fy = (float)my;
  float fz = (float)mz;

  // Super crude heading in the XY plane
  // Note: depending on board orientation, you may need atan2(fx, fy) or negatives.
  float headingRad = atan2(fy, fx);  // atan2(Y, X)
  float headingDeg = headingRad * 180.0f / PI;

  if (headingDeg < 0) {
    headingDeg += 360.0f;
  }

  ui::console_update(reading, headingDeg);       // handles button input, screen output
  //stateMachine::update(dt);     // handles BOOT/CAL/NAV/ERROR

  
  /*Serial.print("Raw mag: X=");
  Serial.print(fx);
  Serial.print("  Y=");
  Serial.print(fy);
  Serial.print("  Z=");
  Serial.print(fz);
  Serial.print("  | Heading: ");
  Serial.print(headingDeg);
  Serial.println(" deg");*/

  delay(200);  // 5 Hz printout
}

} // namespace dpvnav