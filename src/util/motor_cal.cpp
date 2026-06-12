#include "motor_cal.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

namespace motor_cal {

bool load(MotorCal& cal) {
    File f = LittleFS.open(FILE_PATH, "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;

    cal.heading_offset_deg = doc["heading_offset_deg"] | 0.0f;
    return true;
}

}  // namespace motor_cal
