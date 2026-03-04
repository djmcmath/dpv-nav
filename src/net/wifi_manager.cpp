#include "wifi_manager.h"
#include "../config.h"
#include <WiFi.h>

namespace wifi {

static bool staConnected = false;

void init() {
    // Always start the AP so the web server is reachable regardless of STA
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
    Serial.printf("[WiFi] AP '%s' started, IP: %s\n",
                   WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());

    // Optionally also connect to a home network (set WIFI_STA_SSID to "" to skip)
    if (strlen(WIFI_STA_SSID) > 0) {
        WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
        Serial.printf("[WiFi] Connecting to '%s'...\n", WIFI_STA_SSID);

        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED &&
               (millis() - start) < WIFI_STA_TIMEOUT_MS) {
            delay(250);
            Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            staConnected = true;
            WiFi.setSleep(false);
            Serial.printf("[WiFi] STA connected, IP: %s\n",
                          WiFi.localIP().toString().c_str());
        } else {
            WiFi.disconnect(true);  // stop STA attempts, keep AP running
            WiFi.mode(WIFI_AP);
            Serial.println("[WiFi] STA failed, AP-only mode");
        }
    }
}

void update() {
    // placeholder for future reconnect logic
}

bool isAP() {
    return true;  // AP is always running
}

IPAddress ip() {
    // Prefer AP IP since it's always available
    return WiFi.softAPIP();
}

}  // namespace wifi
