#include <cstdint>
#include <Wire.h>
#include "lis3mdl.h"

// Helper: write a single register
void magWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(LIS3MDL_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

// Helper: read multiple bytes starting at reg
void magRead(uint8_t startReg, uint8_t *buffer, uint8_t len) {
  Wire.beginTransmission(LIS3MDL_ADDR);
  Wire.write(startReg);
  Wire.endTransmission(false);  // repeated start

  Wire.requestFrom((int)LIS3MDL_ADDR, (int)len);
  for (uint8_t i = 0; i < len && Wire.available(); i++) {
    buffer[i] = Wire.read();
  }
}