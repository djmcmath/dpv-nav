#include "hdg_cal.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <math.h>

namespace hdg_cal {

bool load(HdgCal& cal) {
    File f = LittleFS.open(FILE_PATH, "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;

    int n = doc["n"] | 0;
    if (n < 1 || n > MAX_HARMONICS) return false;

    JsonArray arr = doc["c"];
    int nCoeffs = 2 * n + 1;
    for (int i = 0; i < nCoeffs; i++) {
        cal.c[i] = arr[i] | 0.0f;
    }
    cal.n = n;
    return true;
}

float apply(float headingDeg, const HdgCal& cal) {
    if (cal.n < 1 || cal.n > MAX_HARMONICS) return headingDeg;

    while (headingDeg <    0.0f) headingDeg += 360.0f;
    while (headingDeg >= 360.0f) headingDeg -= 360.0f;

    float theta = headingDeg * ((float)M_PI / 180.0f);
    float corr  = cal.c[0];
    for (int k = 1; k <= cal.n; k++) {
        corr += cal.c[2*k - 1] * cosf((float)k * theta);
        corr += cal.c[2*k]     * sinf((float)k * theta);
    }
    return fmodf(headingDeg + corr + 360.0f, 360.0f);
}

}  // namespace hdg_cal
