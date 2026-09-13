
#include <cstdint>

// LIS3MDL I2C address on AltIMU-10 v5
const uint8_t LIS3MDL_ADDR = 0x1E;

// LIS3MDL registers
const uint8_t LIS3MDL_REG_WHO_AM_I = 0x0F;
const uint8_t LIS3MDL_REG_CTRL_REG1 = 0x20;
const uint8_t LIS3MDL_REG_CTRL_REG2 = 0x21;
const uint8_t LIS3MDL_REG_CTRL_REG3 = 0x22;
const uint8_t LIS3MDL_REG_CTRL_REG4 = 0x23;
const uint8_t LIS3MDL_REG_CTRL_REG5 = 0x24;
const uint8_t LIS3MDL_REG_OUT_X_L   = 0x28; // X_L, X_H, Y_L, Y_H, Z_L, Z_H
// Die temperature, two bytes (L then H), only updated while CTRL_REG1's TEMP_EN
// is set. This sensor sits on the same die as the magnetometer, which is the
// whole point of logging it: the MS5837's water/ambient temperature lags what
// the magnetometer actually feels by ~23 s (measured 2026-09-11), so it is the
// wrong thermometer for explaining a drifting hard-iron offset.
const uint8_t LIS3MDL_REG_TEMP_OUT_L = 0x2E; // TEMP_L, TEMP_H

bool magWrite(uint8_t reg, uint8_t value);
void magRead(uint8_t startReg, uint8_t *buffer, uint8_t len);