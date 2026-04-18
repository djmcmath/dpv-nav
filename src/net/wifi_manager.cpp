#include "wifi_manager.h"
#include "../config.h"
#include <WiFi.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>
#include <time.h>

namespace wifi {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const char*    NETWORKS_PATH      = "/config/wifi_networks.json";
static const int      MAX_NETWORKS       = 10;
static const uint32_t CONNECT_TIMEOUT_MS = 8000;
static const uint32_t RECONNECT_CYCLE_MS = 60000;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct NetEntry { char ssid[64]; char pass[64]; };
static NetEntry  gNets[MAX_NETWORKS];
static int       gNetCount          = 0;
static bool      gStaConnected      = false;
static bool      gApRunning         = false;
static char      gConnectedSsid[64] = "";

// Reconnect state machine (non-blocking)
static int       gReconnectIdx   = -1;   // -1 = idle, >=0 = attempt in progress
static uint32_t  gAttemptStartMs = 0;
static uint32_t  gLastCycleMs    = 0;

// ---------------------------------------------------------------------------
// Network list persistence
// ---------------------------------------------------------------------------

static void saveNetworks() {
    LittleFS.mkdir("/config");
    File f = LittleFS.open(NETWORKS_PATH, "w");
    if (!f) {
        Serial.println("[WiFi] ERROR: could not write wifi_networks.json");
        return;
    }
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < gNetCount; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["ssid"] = gNets[i].ssid;
        obj["pass"] = gNets[i].pass;
    }
    serializeJson(doc, f);
    f.close();
}

static void loadNetworks() {
    gNetCount = 0;

    if (!LittleFS.exists(NETWORKS_PATH)) {
        // First boot: seed from compile-time defaults if provided
        if (strlen(WIFI_STA_SSID) > 0) {
            strlcpy(gNets[0].ssid, WIFI_STA_SSID, sizeof(gNets[0].ssid));
            strlcpy(gNets[0].pass, WIFI_STA_PASS, sizeof(gNets[0].pass));
            gNetCount = 1;
            saveNetworks();
            Serial.printf("[WiFi] Seeded network list from compile-time defaults ('%s')\n",
                          WIFI_STA_SSID);
        }
        return;
    }

    File f = LittleFS.open(NETWORKS_PATH, "r");
    if (!f) { Serial.println("[WiFi] ERROR: could not read wifi_networks.json"); return; }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { Serial.printf("[WiFi] JSON parse error: %s\n", err.c_str()); return; }

    for (JsonObject obj : doc.as<JsonArray>()) {
        if (gNetCount >= MAX_NETWORKS) break;
        const char* ssid = obj["ssid"];
        const char* pass = obj["pass"] | "";
        if (!ssid || strlen(ssid) == 0) continue;
        strlcpy(gNets[gNetCount].ssid, ssid, sizeof(gNets[0].ssid));
        strlcpy(gNets[gNetCount].pass, pass, sizeof(gNets[0].pass));
        gNetCount++;
    }
    Serial.printf("[WiFi] Loaded %d known network(s)\n", gNetCount);
}

// ---------------------------------------------------------------------------
// STA connection helpers
// ---------------------------------------------------------------------------

static void startNtp() {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.println("[WiFi] NTP sync started");
}

// Scan and attempt to connect to the first known network found.
// Blocking — only called at init().  Returns true if connected.
static bool connectByScan() {
    Serial.println("[WiFi] Scanning for known networks...");
    int found = WiFi.scanNetworks();
    if (found <= 0) {
        Serial.println("[WiFi] No networks found in scan");
        WiFi.scanDelete();
        return false;
    }

    for (int s = 0; s < found; s++) {
        String scanned = WiFi.SSID(s);
        for (int k = 0; k < gNetCount; k++) {
            if (scanned != gNets[k].ssid) continue;

            Serial.printf("[WiFi] Connecting to '%s' (RSSI %d dBm)...\n",
                          gNets[k].ssid, (int)WiFi.RSSI(s));
            WiFi.begin(gNets[k].ssid, gNets[k].pass);

            uint32_t start = millis();
            while (WiFi.status() != WL_CONNECTED && (millis() - start) < CONNECT_TIMEOUT_MS) {
                delay(200);
            }
            WiFi.scanDelete();

            if (WiFi.status() == WL_CONNECTED) {
                WiFi.setSleep(false);
                esp_wifi_set_ps(WIFI_PS_NONE);
                strlcpy(gConnectedSsid, gNets[k].ssid, sizeof(gConnectedSsid));
                Serial.printf("[WiFi] STA connected to '%s', IP: %s\n",
                              gConnectedSsid, WiFi.localIP().toString().c_str());
                return true;
            }
            WiFi.disconnect(true);
            Serial.printf("[WiFi] Connect to '%s' failed\n", gNets[k].ssid);
            break;
        }
    }
    WiFi.scanDelete();
    return false;
}

static void beginReconnect(int idx) {
    Serial.printf("[WiFi] Reconnect attempt: '%s'\n", gNets[idx].ssid);
    WiFi.begin(gNets[idx].ssid, gNets[idx].pass);
    gReconnectIdx   = idx;
    gAttemptStartMs = millis();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void init() {
    loadNetworks();
    gStaConnected     = false;
    gApRunning        = false;
    gReconnectIdx     = -1;
    gLastCycleMs      = 0;
    gConnectedSsid[0] = '\0';

    // Set mode and bring up AP first — mode must not change after STA connects
    // or the radio resets and drops the association.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
    gApRunning = true;
    Serial.printf("[WiFi] AP '%s' up, IP: %s\n",
                  WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());

    if (gNetCount > 0) {
        gStaConnected = connectByScan();
    }

    if (!gStaConnected) {
        if (gNetCount > 0) Serial.println("[WiFi] No known network reachable, AP-only");
        WiFi.mode(WIFI_AP);  // drop idle STA interface
    } else {
        startNtp();
    }
}

void stop() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    gStaConnected     = false;
    gApRunning        = false;
    gReconnectIdx     = -1;
    gConnectedSsid[0] = '\0';
    Serial.println("[WiFi] Stopped");
}

void update() {
    // Detect STA drop
    if (gStaConnected && WiFi.status() != WL_CONNECTED) {
        gStaConnected     = false;
        gReconnectIdx     = -1;
        gConnectedSsid[0] = '\0';
        gLastCycleMs      = millis();
        Serial.println("[WiFi] STA disconnected");
    }

    // Non-blocking reconnect state machine
    if (!gStaConnected && gNetCount > 0 && gApRunning) {
        uint32_t now = millis();

        if (gReconnectIdx >= 0) {
            if (WiFi.status() == WL_CONNECTED) {
                WiFi.setSleep(false);
                esp_wifi_set_ps(WIFI_PS_NONE);
                gStaConnected = true;
                strlcpy(gConnectedSsid, gNets[gReconnectIdx].ssid, sizeof(gConnectedSsid));
                gReconnectIdx = -1;
                Serial.printf("[WiFi] Reconnected to '%s', IP: %s\n",
                              gConnectedSsid, WiFi.localIP().toString().c_str());
                startNtp();
            } else if (now - gAttemptStartMs > CONNECT_TIMEOUT_MS) {
                WiFi.disconnect(true);
                int next = gReconnectIdx + 1;
                if (next < gNetCount) {
                    beginReconnect(next);
                } else {
                    gReconnectIdx = -1;
                    gLastCycleMs  = now;
                }
            }
        } else {
            if (now - gLastCycleMs > RECONNECT_CYCLE_MS) {
                gLastCycleMs = now;
                beginReconnect(0);
            }
        }
    }
}

bool      isAP()           { return gApRunning; }
bool      isStaConnected() { return gStaConnected; }
IPAddress ip()             { return WiFi.softAPIP(); }
IPAddress staIP()          { return WiFi.localIP(); }
const char* staSSID()      { return gConnectedSsid; }

bool addNetwork(const char* ssid, const char* pass) {
    if (!ssid || strlen(ssid) == 0) return false;
    for (int i = 0; i < gNetCount; i++) {
        if (strcmp(gNets[i].ssid, ssid) == 0) {
            strlcpy(gNets[i].pass, pass ? pass : "", sizeof(gNets[i].pass));
            saveNetworks();
            return true;
        }
    }
    if (gNetCount >= MAX_NETWORKS) return false;
    strlcpy(gNets[gNetCount].ssid, ssid,            sizeof(gNets[gNetCount].ssid));
    strlcpy(gNets[gNetCount].pass, pass ? pass : "", sizeof(gNets[gNetCount].pass));
    gNetCount++;
    saveNetworks();
    Serial.printf("[WiFi] Added network '%s' (%d total)\n", ssid, gNetCount);
    return true;
}

bool removeNetwork(const char* ssid) {
    for (int i = 0; i < gNetCount; i++) {
        if (strcmp(gNets[i].ssid, ssid) != 0) continue;
        for (int j = i; j < gNetCount - 1; j++) gNets[j] = gNets[j + 1];
        gNetCount--;
        saveNetworks();
        Serial.printf("[WiFi] Removed network '%s' (%d remaining)\n", ssid, gNetCount);
        return true;
    }
    return false;
}

String getNetworksJson() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < gNetCount; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["ssid"] = gNets[i].ssid;
    }
    String result;
    serializeJson(doc, result);
    return result;
}

}  // namespace wifi
