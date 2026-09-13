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
static const uint32_t SCAN_TIMEOUT_MS    = 20000;
static const int      MAX_SCAN_RESULTS   = 32;
// Boot must not hang on WiFi -- cap how many candidates init() will sit
// through at CONNECT_TIMEOUT_MS each before giving up and going AP-only.
static const int      MAX_BOOT_ATTEMPTS  = 3;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct NetEntry { char ssid[64]; char pass[64]; bool hidden; };
static NetEntry  gNets[MAX_NETWORKS];
static int       gNetCount          = 0;
static bool      gStaConnected      = false;
static bool      gApRunning         = false;
static char      gConnectedSsid[64] = "";

// Async scan state
enum ScanState { SCAN_IDLE, SCAN_RUNNING, SCAN_DONE, SCAN_FAILED };
static ScanState gScanState   = SCAN_IDLE;
static String    gScanJson;              // cached result array, built once on completion
static int       gScanHidden  = 0;       // APs seen beaconing with no SSID
static uint32_t  gScanStartMs = 0;

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
        obj["hidden"] = gNets[i].hidden;
    }
    serializeJson(doc, f);
    f.close();
}

static void loadNetworks() {
    gNetCount = 0;

    if (!LittleFS.exists(NETWORKS_PATH)) {
        // No known networks yet.  There are deliberately no compile-time STA
        // credentials in this repo -- join the Tern AP and add a network from
        // the web UI, which persists it to wifi_networks.json.
        Serial.println("[WiFi] No known networks -- add one from the Tern AP web UI");
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
        gNets[gNetCount].hidden = obj["hidden"] | false;
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

// Attempt the known networks at boot, strongest first.
// Blocking — only called at init().  Returns true if connected.
//
// Two candidate sources, in order:
//   1. Known networks that showed up in the scan, sorted by RSSI, so a diver
//      with home + truck + dive-boat saved joins whichever is actually there.
//   2. Known networks flagged `hidden`, which by definition cannot appear in
//      a scan.  A phone hotspot lands here: even the ones that do beacon are
//      usually asleep at boot, so they get tried by name regardless.
// Total attempts are capped at MAX_BOOT_ATTEMPTS to bound boot time.
static bool connectByScan() {
    struct Cand { int idx; int rssi; };
    Cand cands[MAX_NETWORKS];
    int  nCand = 0;

    Serial.println("[WiFi] Scanning for known networks...");
    int found = WiFi.scanNetworks();
    for (int s = 0; s < found; s++) {
        String scanned = WiFi.SSID(s);
        if (scanned.length() == 0) continue;
        int rssi = (int)WiFi.RSSI(s);
        for (int k = 0; k < gNetCount; k++) {
            if (scanned != gNets[k].ssid) continue;
            // Same SSID can appear once per band/repeater -- keep the loudest.
            bool dup = false;
            for (int c = 0; c < nCand; c++) {
                if (cands[c].idx != k) continue;
                dup = true;
                if (rssi > cands[c].rssi) cands[c].rssi = rssi;
                break;
            }
            if (!dup && nCand < MAX_NETWORKS) cands[nCand++] = {k, rssi};
            break;
        }
    }
    WiFi.scanDelete();

    // Insertion sort, strongest first (nCand <= 10).
    for (int i = 1; i < nCand; i++) {
        Cand v = cands[i];
        int j = i - 1;
        while (j >= 0 && cands[j].rssi < v.rssi) { cands[j + 1] = cands[j]; j--; }
        cands[j + 1] = v;
    }

    // Append hidden entries that the scan could not have found.
    for (int k = 0; k < gNetCount && nCand < MAX_NETWORKS; k++) {
        if (!gNets[k].hidden) continue;
        bool already = false;
        for (int c = 0; c < nCand; c++) if (cands[c].idx == k) { already = true; break; }
        if (!already) cands[nCand++] = {k, 0};
    }

    if (nCand == 0) {
        Serial.println("[WiFi] No known network found in scan");
        return false;
    }

    int attempts = (nCand < MAX_BOOT_ATTEMPTS) ? nCand : MAX_BOOT_ATTEMPTS;
    for (int c = 0; c < attempts; c++) {
        NetEntry& n = gNets[cands[c].idx];
        if (cands[c].rssi) Serial.printf("[WiFi] Connecting to '%s' (RSSI %d dBm)...\n", n.ssid, cands[c].rssi);
        else               Serial.printf("[WiFi] Connecting to hidden '%s'...\n", n.ssid);
        WiFi.begin(n.ssid, n.pass);

        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - start) < CONNECT_TIMEOUT_MS) {
            delay(200);
        }

        if (WiFi.status() == WL_CONNECTED) {
            WiFi.setSleep(false);
            esp_wifi_set_ps(WIFI_PS_NONE);
            strlcpy(gConnectedSsid, n.ssid, sizeof(gConnectedSsid));
            Serial.printf("[WiFi] STA connected to '%s', IP: %s\n",
                          gConnectedSsid, WiFi.localIP().toString().c_str());
            return true;
        }
        WiFi.disconnect(true);
        Serial.printf("[WiFi] Connect to '%s' failed\n", n.ssid);
    }
    return false;
}

static void beginReconnect(int idx) {
    Serial.printf("[WiFi] Reconnect attempt: '%s'\n", gNets[idx].ssid);
    WiFi.begin(gNets[idx].ssid, gNets[idx].pass);
    gReconnectIdx   = idx;
    gAttemptStartMs = millis();
}

// ---------------------------------------------------------------------------
// Async SSID scan
// ---------------------------------------------------------------------------

// Collapse the raw scan into a deduplicated, strongest-first JSON array and
// cache it, so the results survive the WiFi.scanDelete() that frees the
// driver's copy and so repeated polls cost nothing.
static void buildScanJson(int n) {
    struct Hit { String ssid; int rssi; bool open; };
    Hit hits[MAX_SCAN_RESULTS];
    int nHit = 0;

    gScanHidden = 0;
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) { gScanHidden++; continue; }  // beaconing, name withheld
        int  rssi = (int)WiFi.RSSI(i);
        bool open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);

        bool dup = false;
        for (int h = 0; h < nHit; h++) {
            if (hits[h].ssid != ssid) continue;
            dup = true;
            if (rssi > hits[h].rssi) { hits[h].rssi = rssi; hits[h].open = open; }
            break;
        }
        if (dup || nHit >= MAX_SCAN_RESULTS) continue;
        hits[nHit].ssid = ssid;
        hits[nHit].rssi = rssi;
        hits[nHit].open = open;
        nHit++;
    }

    for (int i = 1; i < nHit; i++) {
        Hit v = hits[i];
        int j = i - 1;
        while (j >= 0 && hits[j].rssi < v.rssi) { hits[j + 1] = hits[j]; j--; }
        hits[j + 1] = v;
    }

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < nHit; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = hits[i].ssid;
        o["rssi"] = hits[i].rssi;
        o["open"] = hits[i].open;
    }
    gScanJson = "";
    serializeJson(doc, gScanJson);
    Serial.printf("[WiFi] Scan done: %d network(s), %d hidden\n", nHit, gScanHidden);
}

static void pollScan() {
    if (gScanState != SCAN_RUNNING) return;

    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        if (millis() - gScanStartMs > SCAN_TIMEOUT_MS) {
            WiFi.scanDelete();
            gScanState = SCAN_FAILED;
            Serial.println("[WiFi] Scan timed out");
        }
        return;
    }
    if (n < 0) {
        WiFi.scanDelete();
        gScanState = SCAN_FAILED;
        Serial.println("[WiFi] Scan failed");
        return;
    }
    buildScanJson(n);
    WiFi.scanDelete();
    gScanState = SCAN_DONE;
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
    gScanState        = SCAN_IDLE;
    gScanHidden       = 0;
    gScanJson         = "";

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
    if (gScanState == SCAN_RUNNING) { WiFi.scanDelete(); gScanState = SCAN_IDLE; }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    gStaConnected     = false;
    gApRunning        = false;
    gReconnectIdx     = -1;
    gConnectedSsid[0] = '\0';
    Serial.println("[WiFi] Stopped");
}

void update() {
    pollScan();

    // Detect STA drop
    if (gStaConnected && WiFi.status() != WL_CONNECTED) {
        gStaConnected     = false;
        gReconnectIdx     = -1;
        gConnectedSsid[0] = '\0';
        gLastCycleMs      = millis();
        Serial.println("[WiFi] STA disconnected");
    }

    // Non-blocking reconnect state machine.  A scan owns the radio while it
    // runs -- starting a connect underneath it aborts one or the other.
    if (!gStaConnected && gNetCount > 0 && gApRunning && gScanState != SCAN_RUNNING) {
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

bool addNetwork(const char* ssid, const char* pass, bool hidden) {
    if (!ssid || strlen(ssid) == 0) return false;
    for (int i = 0; i < gNetCount; i++) {
        if (strcmp(gNets[i].ssid, ssid) == 0) {
            strlcpy(gNets[i].pass, pass ? pass : "", sizeof(gNets[i].pass));
            gNets[i].hidden = hidden;
            saveNetworks();
            return true;
        }
    }
    if (gNetCount >= MAX_NETWORKS) return false;
    strlcpy(gNets[gNetCount].ssid, ssid,            sizeof(gNets[gNetCount].ssid));
    strlcpy(gNets[gNetCount].pass, pass ? pass : "", sizeof(gNets[gNetCount].pass));
    gNets[gNetCount].hidden = hidden;
    gNetCount++;
    saveNetworks();
    Serial.printf("[WiFi] Added network '%s'%s (%d total)\n",
                  ssid, hidden ? " (hidden)" : "", gNetCount);
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
        obj["ssid"]   = gNets[i].ssid;
        obj["hidden"] = gNets[i].hidden;
    }
    String result;
    serializeJson(doc, result);
    return result;
}

bool connectNow(const char* ssid) {
    if (!ssid || !gApRunning) return false;
    for (int i = 0; i < gNetCount; i++) {
        if (strcmp(gNets[i].ssid, ssid) != 0) continue;
        if (gStaConnected) {
            // Already on a network -- moving is a deliberate act, so drop the
            // current association rather than silently doing nothing.
            WiFi.disconnect(true);
            gStaConnected     = false;
            gConnectedSsid[0] = '\0';
        }
        if (gScanState == SCAN_RUNNING) { WiFi.scanDelete(); gScanState = SCAN_IDLE; }
        if (gReconnectIdx >= 0) WiFi.disconnect(true);
        beginReconnect(i);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------

bool startScan() {
    if (!gApRunning) return false;
    if (gScanState == SCAN_RUNNING) return true;  // already under way, poll it

    // init() drops to WIFI_AP when nothing was reachable; the STA interface
    // has to exist before the driver will scan.
    wifi_mode_t mode = WiFi.getMode();
    if (mode == WIFI_MODE_AP) WiFi.mode(WIFI_AP_STA);

    // A half-finished reconnect attempt owns the radio.  Abandon it -- the
    // cycle timer starts a fresh one once the scan is out of the way.
    if (gReconnectIdx >= 0) {
        WiFi.disconnect(true);
        gReconnectIdx = -1;
        gLastCycleMs  = millis();
    }

    WiFi.scanDelete();
    gScanJson    = "";
    gScanHidden  = 0;
    gScanStartMs = millis();

    // show_hidden=true so APs that beacon without an SSID are counted; they
    // come back with an empty name and are reported only as a count, which is
    // the hint a diver needs to type a hotspot name in by hand.
    int16_t rc = WiFi.scanNetworks(true /*async*/, true /*show_hidden*/);
    if (rc == WIFI_SCAN_FAILED) {
        gScanState = SCAN_FAILED;
        Serial.println("[WiFi] Could not start scan");
        return false;
    }
    gScanState = SCAN_RUNNING;
    Serial.println("[WiFi] Scan started");
    return true;
}

bool isScanning() { return gScanState == SCAN_RUNNING; }

String getScanJson() {
    pollScan();  // keep polling honest even if update() is starved

    const char* state = "idle";
    switch (gScanState) {
        case SCAN_RUNNING: state = "running"; break;
        case SCAN_DONE:    state = "done";    break;
        case SCAN_FAILED:  state = "failed";  break;
        default:           state = "idle";    break;
    }

    String out = "{\"state\":\"";
    out += state;
    out += "\",\"hidden\":";
    out += gScanHidden;
    out += ",\"nets\":";
    out += (gScanState == SCAN_DONE && gScanJson.length()) ? gScanJson : String("[]");
    out += "}";
    return out;
}

}  // namespace wifi
