#include "hdg_cal.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <math.h>

namespace hdg_cal {

static const float kActual[4] = {0.0f, 90.0f, 180.0f, 270.0f};

bool load(HdgCal& cal) {
    File f = LittleFS.open(FILE_PATH, "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;
    if (!doc["h0"].is<float>()) return false;
    cal.indicated[0] = doc["h0"] | 0.0f;
    cal.indicated[1] = doc["h1"] | 90.0f;
    cal.indicated[2] = doc["h2"] | 180.0f;
    cal.indicated[3] = doc["h3"] | 270.0f;
    return true;
}

bool save(const HdgCal& cal) {
    File f = LittleFS.open(FILE_PATH, FILE_WRITE);
    if (!f) return false;
    JsonDocument doc;
    doc["h0"] = cal.indicated[0];
    doc["h1"] = cal.indicated[1];
    doc["h2"] = cal.indicated[2];
    doc["h3"] = cal.indicated[3];
    serializeJson(doc, f);
    f.close();
    return true;
}

float apply(float headingDeg, const HdgCal& cal) {
    // Normalize input to [0, 360)
    while (headingDeg < 0.0f)    headingDeg += 360.0f;
    while (headingDeg >= 360.0f) headingDeg -= 360.0f;

    // Build (indicated, correction) pairs.  Each indicated value is normalized
    // to [0, 360) and paired with correction = actual - indicated (normalized
    // to [-180, 180] to handle near-0°/360° wrap).
    struct Pt { float ind; float corr; };
    Pt pts[4];
    for (int i = 0; i < 4; i++) {
        float ind = fmod(cal.indicated[i] + 360.0f, 360.0f);
        float c   = kActual[i] - ind;
        while (c >  180.0f) c -= 360.0f;
        while (c < -180.0f) c += 360.0f;
        pts[i] = {ind, c};
    }

    // Sort by indicated value (insertion sort, 4 elements)
    for (int i = 1; i < 4; i++) {
        Pt key = pts[i];
        int j = i - 1;
        while (j >= 0 && pts[j].ind > key.ind) { pts[j + 1] = pts[j]; j--; }
        pts[j + 1] = key;
    }

    // Check segments pts[i] → pts[i+1] for i = 0..2
    for (int i = 0; i < 3; i++) {
        if (headingDeg >= pts[i].ind && headingDeg < pts[i + 1].ind) {
            float span = pts[i + 1].ind - pts[i].ind;
            if (span < 0.001f) return headingDeg;
            float t = (headingDeg - pts[i].ind) / span;
            float c = pts[i].corr + t * (pts[i + 1].corr - pts[i].corr);
            return fmod(headingDeg + c + 360.0f, 360.0f);
        }
    }

    // Wrap-around segment: pts[3] → pts[0] + 360
    float lo   = pts[3].ind;
    float hi   = pts[0].ind + 360.0f;
    float span = hi - lo;
    if (span < 0.001f) return headingDeg;
    float hh = (headingDeg >= lo) ? headingDeg : headingDeg + 360.0f;
    float t  = (hh - lo) / span;
    float c  = pts[3].corr + t * (pts[0].corr - pts[3].corr);
    return fmod(headingDeg + c + 360.0f, 360.0f);
}

void corrections(const HdgCal& cal, float out_corrections[4]) {
    for (int i = 0; i < 4; i++) {
        float c = kActual[i] - cal.indicated[i];
        while (c >  180.0f) c -= 360.0f;
        while (c < -180.0f) c += 360.0f;
        out_corrections[i] = c;
    }
}

}  // namespace hdg_cal
