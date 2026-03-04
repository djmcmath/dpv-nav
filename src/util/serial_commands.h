#pragma once
#include "../types/types.h"
#include "../math/mahony.h"
#include "../sensors/imu.h"

namespace serial_cmd {

// Print available commands list (call during setup)
void printHelp();

// Check Serial for input, parse and dispatch commands (call from loop)
void processInput(MahonyState& ahrs);

// Feed current sensor readings to active stability test (call from loop after sensor read)
void updateStabilityTest(const imu::Vec3f& magCal, const imu::Vec3f& accelCal);

}  // namespace serial_cmd
