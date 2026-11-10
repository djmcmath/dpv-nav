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

// How often a conversion cycle is allowed to start, measured from the start
// of the previous one. Running the ADC flat out at OSR 8192 (two ~20 ms
// conversions back to back, so ~25 Hz at 100% duty) self-heats the die, and
// the die temperature is what temperatureC() reports -- so it reads warm.
// Nothing downstream needs depth faster than the 1 Hz log: the display shows
// one depth, and the dive trigger only needs to notice 3 m. 500 ms drops the
// ADC duty cycle to ~8%.
constexpr uint32_t SAMPLE_PERIOD_MS = 500;

// Non-blocking state-machine tick. Call every nav-loop iteration (~100 Hz).
// Advances a D1 (pressure) -> D2 (temperature) conversion cycle using
// millis()-gated waits; never blocks. A cycle takes ~40 ms and a new one
// starts every SAMPLE_PERIOD_MS.
void update();

// True for one update() cycle after a new pressure+temperature pair has
// been computed. Read pressureMbar()/temperatureC() then it clears.
bool hasNewSample();

// Compensated pressure, in millibar (mbar / hPa).
float pressureMbar();

// Compensated temperature, in degrees Celsius. This is the sensor's own die
// temperature. It is what the pressure compensation needs, but it is only
// water temperature insofar as the probe is coupled to the water -- see
// sensors/depth.h.
float temperatureC();

// I2C health counters since boot, for the tern.local Diagnostics card.
//
// An ADC read that fails leaves the chip's previous raw value in the driver,
// so a failed read drops the whole cycle instead of recompensating stale raws
// as if they were fresh. Without that, a sensor that stops answering reads out
// as a plausible frozen pressure and temperature with nothing to flag it.
struct Health {
    uint32_t goodSamples;     // completed D1+D2 cycles
    uint32_t adcReadErrors;   // failed reads of the 24-bit ADC result
    uint32_t cmdWriteErrors;  // failed convert-command writes
    uint32_t lastErrorMs;     // millis() of the most recent error; 0 = none yet
};
Health health();

}  // namespace ms5837
