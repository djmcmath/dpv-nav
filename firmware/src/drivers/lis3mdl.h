
#include <cstdint>

// LIS3MDL I2C address on AltIMU-10 v5
const uint8_t LIS3MDL_ADDR = 0x1E;

// LIS3MDL registers
const uint8_t LIS3MDL_REG_WHO_AM_I = 0x0F;
const uint8_t LIS3MDL_REG_CTRL_REG1 = 0x20;
const uint8_t LIS3MDL_REG_CTRL_REG2 = 0x21;
const uint8_t LIS3MDL_REG_CTRL_REG3 = 0x22;
const uint8_t LIS3MDL_REG_CTRL_REG4 = 0x23;
const uint8_t LIS3MDL_REG_OUT_X_L   = 0x28; // X_L, X_H, Y_L, Y_H, Z_L, Z_H

void magWrite(uint8_t reg, uint8_t value);
void magRead(uint8_t startReg, uint8_t *buffer, uint8_t len);