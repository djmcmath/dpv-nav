#line 1 "D:\\Documents\\dpv-nav\\firmware\\src\\nav\\ui_controller.cpp"
#include "../types/types.h"
#include "ui_controller.h"
#include <Arduino.h>

namespace ui {

void console_update(mag_reading mag, float headingDeg) {
    Serial.print("Raw mag: X=");
    Serial.print(mag.x);
    Serial.print("  Y=");
    Serial.print(mag.y);
    Serial.print("  Z=");
    Serial.print(mag.z);
    Serial.print("  | Heading: ");
    Serial.print(headingDeg);
    Serial.println(" deg");
}

} //namespace ui