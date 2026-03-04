#pragma once

#include "../types/types.h"
#include "../sensors/imu.h"

struct NavPacket;
struct DebugPacket;

namespace ui {

// Initialize the display hardware.  Call once during setup.
bool init();

// Run display self-test (color cycle + text).
void selfTest();

// Show a boot diagnostic line on OLED: "label .. ok/FAIL"
void bootStatus(const char* label, bool ok);

// Draw the navigation screen on OLED.
void showNav(const NavPacket& pkt);

// Draw the debug screen on OLED.
void showDebug(const DebugPacket& pkt);

void console_update(
    imu::Vec3f magRaw, imu::Vec3f magCal,
    imu::Vec3f accelRaw, imu::Vec3f accelCal,
    imu::Vec3f gyroRaw, imu::Vec3f gyroCal,
    float headingDeg
);

}  // namespace ui
