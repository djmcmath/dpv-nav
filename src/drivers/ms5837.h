#pragma once

#include <cstdint>

// Raw I2C driver for the TE/Measurement Specialties MS5837-30BA pressure +
// temperature sensor (as used on the BlueRobotics Bar30 breakout).
// Shares the IMU's I2C bus (SDA_PIN/SCL_PIN) — no dedicated GPIO needed.
//
// Pure sensor physics only (PROM read/CRC, ADC conversion, temperature
// compensation). Depth/water-density/zero-offset logic lives one layer up
// in sensors/depth.h.
namespace ms5837 {

// I2C address is fixed on this part.
constexpr uint8_t MS5837_ADDR = 0x76;

// Probe the sensor: reset, read PROM, validate CRC4. Returns false (and
// leaves the driver in a permanently-absent state) if the sensor doesn't
// respond or the CRC check fails — the expected outcome on boards where
// J3 isn't populated.
bool init();

// True once init() has succeeded.
bool isPresent();

// Non-blocking state-machine tick. Call every nav-loop iteration (~100 Hz).
// Advances a D1 (pressure) -> D2 (temperature) conversion cycle using
// millis()-gated waits; never blocks. A full cycle takes ~40 ms.
void update();

// True for one update() cycle after a new pressure+temperature pair has
// been computed. Read pressureMbar()/temperatureC() then it clears.
bool hasNewSample();

// Compensated pressure, in millibar (mbar / hPa).
float pressureMbar();

// Compensated temperature, in degrees Celsius.
float temperatureC();

}  // namespace ms5837
