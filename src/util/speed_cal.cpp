#include "speed_cal.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

namespace speed_cal {

static constexpr const char* SPEED_CAL_FILE = "/speed_cal.json";

History load() {
    History h{};
    h.count = 0;

    if (!LittleFS.exists(SPEED_CAL_FILE)) return h;

    File f = LittleFS.open(SPEED_CAL_FILE, "r");
    if (!f) return h;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return h;

    uint8_t n = doc["n"] | (uint8_t)0;
    if (n > HISTORY_SIZE) n = HISTORY_SIZE;
    h.count = n;

    JsonArray arr = doc["k"];
    if (arr) {
        for (int i = 0; i < n && i < HISTORY_SIZE; i++) {
            h.k_values[i] = arr[i] | 0.0f;
        }
    }
    return h;
}

void save(const History& h) {
    File f = LittleFS.open(SPEED_CAL_FILE, "w");
    if (!f) return;

    JsonDocument doc;
    doc["n"] = h.count;
    JsonArray arr = doc["k"].to<JsonArray>();
    for (int i = 0; i < h.count; i++) {
        arr.add(h.k_values[i]);
    }
    serializeJson(doc, f);
    f.close();
}

void addMeasurement(History& h, float k_value) {
    if (h.count < HISTORY_SIZE) {
        h.k_values[h.count++] = k_value;
    } else {
        // Shift oldest out, append new at end
        for (int i = 0; i < HISTORY_SIZE - 1; i++) {
            h.k_values[i] = h.k_values[i + 1];
        }
        h.k_values[HISTORY_SIZE - 1] = k_value;
    }
}

float averageK(const History& h, float default_k) {
    if (h.count == 0) return default_k;
    float sum = 0.0f;
    for (int i = 0; i < h.count; i++) sum += h.k_values[i];
    return sum / (float)h.count;
}

void reset(History& h) {
    h.count = 0;
}

}  // namespace speed_cal
