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
// Temporarily stopping the softAP (radio contention)
//
// The softAP and the STA share one radio. A server-side packet capture on
// 2026-09-18 showed large cloud uploads stalling because the *server's ACKs*
// were not reaching this device -- it kept retransmitting data the server had
// already acknowledged, on RTO backoff, until HTTPClient gave up with -3.
// Small uploads that fit in one congestion window never need a mid-transfer
// ACK and so always succeeded, which is why it looked like a size limit.
// AP+STA contention is the leading suspect for that inbound loss.
//
// suspendAp() drops to STA-only for the duration of a transfer; resumeAp()
// puts it back. Both are no-ops unless the STA is connected -- with no STA
// there is nothing to protect and stopping the AP would leave the diver no
// way in at all.
//
// It uses WiFi.enableAP() rather than WiFi.mode(): enableAP() clears just the
// AP bit and leaves the STA netif alone, where a full mode() reset is what
// init()'s comment warns drops the association. The STA link is re-checked
// after the switch anyway, and the AP is restored immediately if it did drop.
//
// Callers MUST pair these. Anything on the AP side of the radio (a browser at
// 192.168.4.1) loses its connection for the duration, including the very
// request that triggered the upload -- acceptable because the alternative is
// an upload that cannot finish, and the AP comes straight back.
bool suspendAp();   // true if the AP was actually stopped (i.e. resume it)
void resumeAp();

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
