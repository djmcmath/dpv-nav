#pragma once

#include <IPAddress.h>
#include <WString.h>

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
//
// `hidden` marks a network that does not beacon its SSID, or that is only
// intermittently up -- a phone hotspot is both.  Hidden entries are invisible
// to a scan, so they are attempted by name instead of being matched against
// scan results.  See connectByScan().
bool   addNetwork(const char* ssid, const char* pass, bool hidden = false);
bool   removeNetwork(const char* ssid);
String getNetworksJson();  // JSON array of {ssid, hidden} objects (no passwords)

// Force an immediate (non-blocking) connect attempt to one known network,
// jumping the reconnect cycle.  Used by the web UI's "Try now" button after
// a hotspot has been switched on.  False if the SSID is not in the list.
bool connectNow(const char* ssid);

// ---------------------------------------------------------------------------
// SSID scanning
//
// The scan is asynchronous on purpose.  Scanning takes the radio off the
// softAP channel for seconds at a time, so a handler that blocked on it would
// stall -- and often drop -- the very HTTP connection the diver is using to
// pick a network.  startScan() returns immediately; the UI polls
// getScanJson() until "state" is no longer "running".
// ---------------------------------------------------------------------------
bool   startScan();
bool   isScanning();
String getScanJson();  // {"state":"idle|running|done|failed","hidden":N,"nets":[...]}

}  // namespace wifi
