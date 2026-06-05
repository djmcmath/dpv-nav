#include "waypoints.h"
#include "../config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>

namespace waypoints {

static constexpr const char* FILE_PATH = "/config/waypoints.json";
static constexpr const char* HOME_NAME = "HOME";

static std::vector<Waypoint> gList;

static Waypoint makeHome(float lat = DEFAULT_BASELINE_LAT,
                         float lon = DEFAULT_BASELINE_LON) {
    Waypoint w{};
    strncpy(w.name, HOME_NAME, WP_NAME_LEN);
    w.lat = lat;
    w.lon = lon;
    return w;
}

void load() {
    gList.clear();

    if (!LittleFS.exists(FILE_PATH)) {
        gList.push_back(makeHome());
        Serial.println("[WP] No waypoints file — HOME initialized at baseline");
        return;
    }

    File f = LittleFS.open(FILE_PATH, "r");
    if (!f) {
        gList.push_back(makeHome());
        Serial.println("[WP] Could not open waypoints file — HOME initialized");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err || !doc.is<JsonArray>()) {
        gList.push_back(makeHome());
        Serial.printf("[WP] Parse error (%s) — HOME initialized\n", err ? err.c_str() : "not array");
        return;
    }

    bool hasHome = false;
    for (JsonObject obj : doc.as<JsonArray>()) {
        if ((int)gList.size() >= WP_MAX_COUNT) break;
        const char* name = obj["name"] | "";
        float lat = obj["lat"] | 0.0f;
        float lon = obj["lon"] | 0.0f;
        if (name[0] == '\0') continue;

        Waypoint w{};
        strncpy(w.name, name, WP_NAME_LEN);
        w.name[WP_NAME_LEN] = '\0';
        w.lat = lat;
        w.lon = lon;

        if (strcmp(w.name, HOME_NAME) == 0) {
            if (!hasHome) {
                gList.insert(gList.begin(), w);  // HOME must be at index 0
                hasHome = true;
            }
        } else {
            gList.push_back(w);
        }
    }

    if (!hasHome) {
        gList.insert(gList.begin(), makeHome());
    }

    Serial.printf("[WP] Loaded %d waypoints\n", (int)gList.size());
}

bool save() {
    LittleFS.mkdir("/config");
    File f = LittleFS.open(FILE_PATH, "w");
    if (!f) {
        Serial.println("[WP] ERROR: could not open waypoints file for write");
        return false;
    }

    // Write JSON array manually to avoid large heap allocation for many waypoints
    f.print("[");
    for (int i = 0; i < (int)gList.size(); i++) {
        if (i > 0) f.print(",");
        const Waypoint& w = gList[i];
        f.printf("{\"name\":\"%s\",\"lat\":%.8f,\"lon\":%.8f}", w.name, w.lat, w.lon);
    }
    f.print("]");
    f.close();

    Serial.printf("[WP] Saved %d waypoints\n", (int)gList.size());
    return true;
}

int count() {
    return (int)gList.size();
}

const Waypoint* get(int idx) {
    if (idx < 0 || idx >= (int)gList.size()) return nullptr;
    return &gList[idx];
}

int indexOf(const char* name) {
    for (int i = 0; i < (int)gList.size(); i++) {
        if (strcmp(gList[i].name, name) == 0) return i;
    }
    return -1;
}

void updateHome(float lat, float lon) {
    if (gList.empty()) {
        gList.push_back(makeHome(lat, lon));
        return;
    }
    gList[0].lat = lat;
    gList[0].lon = lon;
}

bool addOrUpdate(const char* name, float lat, float lon) {
    if (!name || name[0] == '\0') return false;
    if (strcmp(name, HOME_NAME) == 0) return false;

    for (auto& w : gList) {
        if (strcmp(w.name, name) == 0) {
            w.lat = lat;
            w.lon = lon;
            return true;
        }
    }

    if ((int)gList.size() >= WP_MAX_COUNT) return false;

    Waypoint w{};
    strncpy(w.name, name, WP_NAME_LEN);
    w.name[WP_NAME_LEN] = '\0';
    w.lat = lat;
    w.lon = lon;
    gList.push_back(w);
    return true;
}

bool remove(const char* name) {
    if (!name || strcmp(name, HOME_NAME) == 0) return false;
    for (auto it = gList.begin(); it != gList.end(); ++it) {
        if (strcmp(it->name, name) == 0) {
            gList.erase(it);
            return true;
        }
    }
    return false;
}

String toJson() {
    String out = "[";
    for (int i = 0; i < (int)gList.size(); i++) {
        if (i > 0) out += ",";
        const Waypoint& w = gList[i];
        char entry[80];
        snprintf(entry, sizeof(entry),
                 "{\"name\":\"%s\",\"lat\":%.8f,\"lon\":%.8f}",
                 w.name, w.lat, w.lon);
        out += entry;
    }
    out += "]";
    return out;
}

}  // namespace waypoints
