#include "boot_log.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <cstring>

namespace boot_log {

// NVS namespace name -- max 15 chars (ESP32 NVS constraint).
static constexpr char NS[] = "boot_log";

static constexpr int    MAX_BOOTS               = 6;
static constexpr int    MAX_DELETIONS_PER_BOOT  = 4;
static constexpr size_t PATH_BUF_LEN            = 40;  // matches logging::LOG_PATH_MAX plus margin

static char gProtectedPath[PATH_BUF_LEN] = "";
static bool gProtectedPathLoaded         = false;

static const char* reasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT_PIN";
        case ESP_RST_SW:        return "SW_RESTART";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP_WAKE";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

// Read the ring buffer (a JSON array stored as one NVS string) into `doc`.
// Leaves `doc` holding an empty array if nothing has been stored yet or the
// stored value didn't parse (e.g. a namespace wipe between firmware builds).
static void loadRing(JsonDocument& doc) {
    Preferences prefs;
    if (!prefs.begin(NS, /*readOnly=*/true)) {
        doc.to<JsonArray>();
        return;
    }
    String raw = prefs.getString("ring", "[]");
    prefs.end();
    if (deserializeJson(doc, raw) != DeserializationError::Ok || !doc.is<JsonArray>()) {
        doc.clear();
        doc.to<JsonArray>();
    }
}

static void saveRing(JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    Preferences prefs;
    if (!prefs.begin(NS, /*readOnly=*/false)) return;
    prefs.putString("ring", out);
    prefs.end();
}

void recordBoot() {
    uint32_t seq = 0;
    {
        Preferences prefs;
        if (prefs.begin(NS, /*readOnly=*/false)) {
            seq = prefs.getUInt("seq", 0) + 1;
            prefs.putUInt("seq", seq);
            prefs.end();
        }
    }

    esp_reset_reason_t reason = esp_reset_reason();
    const char* name = reasonName(reason);
    Serial.printf("[BOOT] #%u reset reason: %s\n", (unsigned)seq, name);

    JsonDocument doc;
    loadRing(doc);
    JsonArray arr = doc.as<JsonArray>();
    while (arr.size() >= MAX_BOOTS) arr.remove(0);

    JsonObject entry = arr.add<JsonObject>();
    entry["seq"]    = seq;
    entry["reason"] = name;
    entry["del"].to<JsonArray>();
    saveRing(doc);

    // Prime the cache now, before anything else this boot has a chance to
    // call setActiveLogPath() and overwrite the value we need to read.
    gProtectedPathLoaded = false;
    protectedLogPath();
}

void setActiveLogPath(const char* path) {
    Preferences prefs;
    if (!prefs.begin(NS, /*readOnly=*/false)) return;
    prefs.putString("active_path", path ? path : "");
    prefs.end();
}

void clearActiveLogPath() {
    setActiveLogPath("");
}

const char* protectedLogPath() {
    if (!gProtectedPathLoaded) {
        gProtectedPathLoaded = true;
        gProtectedPath[0] = '\0';
        Preferences prefs;
        if (prefs.begin(NS, /*readOnly=*/true)) {
            String p = prefs.getString("active_path", "");
            prefs.end();
            strlcpy(gProtectedPath, p.c_str(), sizeof(gProtectedPath));
        }
        if (gProtectedPath[0]) {
            Serial.printf("[BOOT] protected log from previous session: %s\n", gProtectedPath);
        }
    }
    return gProtectedPath;
}

void recordCleanupDeletion(const char* path, uint32_t sizeBytes, bool wasProtected) {
    Serial.printf("[BOOT] cleanup deleted %s (%u bytes)%s\n", path, (unsigned)sizeBytes,
                  wasProtected ? " -- WAS THE ACTIVE LOG; PROTECTION FAILED" : "");

    JsonDocument doc;
    loadRing(doc);
    JsonArray arr = doc.as<JsonArray>();
    if (arr.size() == 0) return;  // recordBoot() wasn't called this boot -- shouldn't happen

    JsonObject last = arr[arr.size() - 1];
    JsonArray del = last["del"].is<JsonArray>() ? last["del"].as<JsonArray>()
                                                 : last["del"].to<JsonArray>();
    if ((int)del.size() >= MAX_DELETIONS_PER_BOOT) return;  // defensive cap

    JsonObject d = del.add<JsonObject>();
    d["n"]   = path;
    d["s"]   = sizeBytes;
    d["act"] = wasProtected;
    saveRing(doc);
}

String statusJson() {
    JsonDocument doc;
    loadRing(doc);
    String out;
    serializeJson(doc, out);
    return out;
}

}  // namespace boot_log
