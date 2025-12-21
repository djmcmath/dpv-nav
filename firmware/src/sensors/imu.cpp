#include <Wire.h>
#include "../board_pins.h"
#include "../drivers/lis3mdl.h"
#include "../types/types.h"
#include <Arduino.h>

namespace imu {

void init() {
    Wire.begin(SDA_PIN, SCL_PIN);
}
  
void initMag() {
  // Optional: sanity check WHO_AM_I
  Wire.beginTransmission(LIS3MDL_ADDR);
  Wire.write(LIS3MDL_REG_WHO_AM_I);
  Wire.endTransmission(false);
  Wire.requestFrom((int)LIS3MDL_ADDR, 1);
  uint8_t who = Wire.available() ? Wire.read() : 0xFF;

  Serial.print("LIS3MDL WHO_AM_I (expect 0x3D): 0x");
  Serial.println(who, HEX);

  // Basic configuration:
  // CTRL_REG1: ultra-high-performance on X/Y, 10 Hz (for now), temp disabled
  // 0b01100000 = 0x60: TEMP_EN=0, OM=11 (UHP), DO=000 (0.625 Hz) – but let's bump to 10 Hz
  // The datasheet uses different encoding; 0x70 gives DO ~20 Hz w/ UHP.
  magWrite(LIS3MDL_REG_CTRL_REG1, 0x70); // UHP XY, ~20 Hz

  // CTRL_REG2: full-scale ±4 gauss (0x00) is fine
  magWrite(LIS3MDL_REG_CTRL_REG2, 0x00);

  // CTRL_REG3: continuous-conversion mode (MD[1:0] = 00)
  magWrite(LIS3MDL_REG_CTRL_REG3, 0x00);

  // CTRL_REG4: ultra-high-performance on Z
  magWrite(LIS3MDL_REG_CTRL_REG4, 0x0C);

  delay(20); // give it a moment
}

mag_reading readMagRaw(int16_t &mx, int16_t &my, int16_t &mz) {
  uint8_t buffer[6];
  magRead(LIS3MDL_REG_OUT_X_L | 0x80, buffer, 6); // 0x80 for auto-increment

  // Little-endian: low byte first
  mx = (int16_t)(buffer[1] << 8 | buffer[0]);
  my = (int16_t)(buffer[3] << 8 | buffer[2]);
  mz = (int16_t)(buffer[5] << 8 | buffer[4]);

  // Create the return object
  mag_reading reading;
  reading.x = mx;
  reading.y = my;
  reading.z = mz;
  return reading;
}

} // namespace imu
