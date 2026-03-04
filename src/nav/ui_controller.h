#include "../types/types.h"
#include "../sensors/imu.h"


#pragma once

namespace ui {
    void console_update(
        imu::Vec3f magRaw, imu::Vec3f magCal,
        imu::Vec3f accelRaw, imu::Vec3f accelCal,
        imu::Vec3f gyroRaw, imu::Vec3f gyroCal,
        float headingDeg
    );
}