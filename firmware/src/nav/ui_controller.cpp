#include "../types/types.h"
#include "ui_controller.h"
#include <Arduino.h>


namespace ui {

    const bool PRINT_RAW = false;

void console_update(
    imu::Vec3f magRaw, imu::Vec3f magCal,
    imu::Vec3f accelRaw, imu::Vec3f accelCal,
    imu::Vec3f gyroRaw, imu::Vec3f gyroCal,
    float headingDeg
) {
    if (PRINT_RAW) {
        Serial.print("Mag-Raw(X/Y/Z):");
        Serial.print(magRaw.x, 3);
        Serial.print(", ");
        Serial.print(magRaw.y, 3);
        Serial.print(", ");
        Serial.print(magRaw.z, 3);
    }

    Serial.print("  | Mag-Cal(X/Y/Z):");
    Serial.print(magCal.x, 3);
    Serial.print(", ");
    Serial.print(magCal.y, 3);
    Serial.print(", ");
    Serial.print(magCal.z, 3);

    if (PRINT_RAW) {
        Serial.print("  | Accel-Raw(X/Y/Z):");
        Serial.print(accelRaw.x, 3);
        Serial.print(", ");
        Serial.print(accelRaw.y, 3);
        Serial.print(", ");
        Serial.print(accelRaw.z, 3);
    }

    Serial.print("  | Accel-Cal(X/Y/Z):");
    Serial.print(accelCal.x, 3);
    Serial.print(", ");
    Serial.print(accelCal.y, 3);
    Serial.print(", ");
    Serial.print(accelCal.z, 3);
    
    if (PRINT_RAW) {
        Serial.print("  | Gyro-Raw(X/Y/Z):");
        Serial.print(gyroRaw.x, 3);
        Serial.print(", ");
        Serial.print(gyroRaw.y, 3);
        Serial.print(", ");
        Serial.print(gyroRaw.z, 3);
    }

    Serial.print("  | Gyro-Cal(X/Y/Z):");
    Serial.print(gyroCal.x, 3);
    Serial.print(", ");
    Serial.print(gyroCal.y, 3);
    Serial.print(", ");
    Serial.print(gyroCal.z, 3);

    Serial.print("\t| Heading: ");
    Serial.print(headingDeg, 1);
    Serial.println(" deg");
}

} //namespace ui