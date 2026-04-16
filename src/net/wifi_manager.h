#pragma once

#include <IPAddress.h>

namespace wifi {

// Lifecycle
void init();
void stop();
void update();

// Status
bool      isAP();
bool      isStaConnected();
IPAddress ip();        // AP IP (always available when WiFi enabled)
IPAddress staIP();     // STA IP (valid only when isStaConnected())
const char* staSSID(); // SSID of connected network (empty string if not connected)

// Network list management (persisted to /config/wifi_networks.json)
bool   addNetwork(const char* ssid, const char* pass);
bool   removeNetwork(const char* ssid);
String getNetworksJson();  // JSON array of {ssid} objects (no passwords)

}  // namespace wifi
