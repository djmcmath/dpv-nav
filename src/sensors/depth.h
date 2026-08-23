#pragma once

// Depth sensor (BlueRobotics Bar30 / MS5837-30BA): wraps drivers/ms5837.h
// with the application-level physics — surface zero-offset and water
// density — the same way sensors/imu.h wraps the raw IMU drivers with
// calibration. See config.h for WATER_DENSITY_*_KG_M3 defaults.
namespace depth {

// Probes the sensor (drivers/ms5837.h). Returns false if absent — callers
// should treat that as "no depth sensor on this board" and skip everything
// downstream (display, logging, dive-mode trigger).
bool init();

// True once init() has succeeded.
bool isPresent();

// Ticks the underlying driver's non-blocking conversion state machine.
// Call every nav-loop iteration (~100 Hz).
void update();

// True for one cycle after a new depth+temperature reading is ready.
bool hasNewSample();

// Captures the most recent pressure reading as the surface (0 m) reference.
// Call once, shortly after boot, on the first available sample — assumes
// the device is in air/at the surface when this runs.
void zero();

// Select fresh (997 kg/m^3) vs salt (1025 kg/m^3) water density for the
// pressure->depth conversion. Persisted/restored by the caller (NVS).
void setSaltWater(bool salt);

// Current depth in meters (0 = surface, increasing with depth). 0 if the
// sensor is absent or hasn't been zeroed yet.
float getDepth_m();

// Current water temperature in degrees Celsius. 0 if the sensor is absent.
float getTemp_c();

}  // namespace depth
