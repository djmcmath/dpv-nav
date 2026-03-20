#include "menu.h"
#include "../drivers/display.h"
#include "../util/nvs_state.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

namespace menu {

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
static SubMenu  submenus[MAX_SUBMENUS];
static uint8_t  submenuCount = 0;

// Navigation stack (for 2-level depth)
static int8_t   menuStack[MAX_MENU_DEPTH];  // submenu indices
static uint8_t   stackDepth = 0;             // 0 = closed

static uint8_t  selectedItem = 0;            // highlighted item in current submenu
static uint32_t lastActivityMs = 0;          // for idle timeout

static SendCmdFn gSendCmd = nullptr;

static bool gDiveMode = false;  // Op mode toggle state (display-side tracking)
static uint8_t gLogLevel = 0;  // Log level tracking (0=OFF, 1=LOW, 2=HIGH)
static bool gSpeedCalPending = false;  // true after user selects "Speed cal"

// Nav-device toggle states (updated from NavPacket flags)
static bool gGpsPosEnabled = true;
static bool gGpsSpdEnabled = true;
static bool gWifiEnabled   = true;

static DisplaySettings gSettings = {
    .debugMode   = false,
    .showETA     = false,
    .imperial    = false,
    .trueHeading = true,
};

// Display coordinates for menu area
// Layout: separator line at y=54, title at y=56 (size 1),
//         single large item at y=70 (size 2, one item at a time)
constexpr int MENU_SEP_Y  = 54;
constexpr int TITLE_Y     = 56;
constexpr int ITEM_Y      = 70;   // size 2 text = 16px tall, fits in 70..86

// Colors
constexpr uint16_t CLR_CYAN   = 0x07FF;
constexpr uint16_t CLR_YELLOW = 0xFFE0;
constexpr uint16_t CLR_WHITE  = 0xFFFF;
constexpr uint16_t CLR_BLACK  = 0x0000;
constexpr uint16_t CLR_GRAY   = 0x7BEF;
constexpr uint16_t CLR_BLUE   = 0x001F;

// ---------------------------------------------------------------------------
// Render cache — only re-send SPI bytes when content actually changes.
// ---------------------------------------------------------------------------
static char prevTitle[22] = "";
static char prevItem[22]  = "";

static void invalidateMenuCache() {
    prevTitle[0] = '\0';
    prevItem[0]  = '\0';
}

// ---------------------------------------------------------------------------
// Hardcoded default menu
// ---------------------------------------------------------------------------
static void loadDefaults() {
    submenuCount = 5;

    // Root menu (index 0)
    auto& root = submenus[0];
    strncpy(root.title, "MENU", MENU_LABEL_LEN);
    root.count = 4;
    strncpy(root.items[0].label, "Nav", MENU_LABEL_LEN);
    root.items[0].action = Action::SUBMENU; root.items[0].submenuIdx = 1;
    strncpy(root.items[1].label, "Cal", MENU_LABEL_LEN);
    root.items[1].action = Action::SUBMENU; root.items[1].submenuIdx = 2;
    strncpy(root.items[2].label, "Input", MENU_LABEL_LEN);
    root.items[2].action = Action::SUBMENU; root.items[2].submenuIdx = 3;
    strncpy(root.items[3].label, "Display", MENU_LABEL_LEN);
    root.items[3].action = Action::SUBMENU; root.items[3].submenuIdx = 4;

    // NAV submenu (index 1)
    auto& nav = submenus[1];
    strncpy(nav.title, "Nav", MENU_LABEL_LEN);
    nav.count = 5;  // 4 items + back
    strncpy(nav.items[0].label, "Outbound", MENU_LABEL_LEN);
    nav.items[0].action = Action::NAV_OUTBOUND; nav.items[0].submenuIdx = -1;
    strncpy(nav.items[1].label, "Home", MENU_LABEL_LEN);
    nav.items[1].action = Action::NAV_HOME; nav.items[1].submenuIdx = -1;
    strncpy(nav.items[2].label, "Mark", MENU_LABEL_LEN);
    nav.items[2].action = Action::NAV_MARK; nav.items[2].submenuIdx = -1;
    strncpy(nav.items[3].label, "Op Mode", MENU_LABEL_LEN);
    nav.items[3].action = Action::NAV_OP_MODE; nav.items[3].submenuIdx = -1;
    strncpy(nav.items[4].label, "..", MENU_LABEL_LEN);
    nav.items[4].action = Action::NONE; nav.items[4].submenuIdx = -1;

    // CAL submenu (index 2)
    auto& cal = submenus[2];
    strncpy(cal.title, "Cal", MENU_LABEL_LEN);
    cal.count = 4;
    strncpy(cal.items[0].label, "Quick cal", MENU_LABEL_LEN);
    cal.items[0].action = Action::CAL_QUICK; cal.items[0].submenuIdx = -1;
    strncpy(cal.items[1].label, "Full cal", MENU_LABEL_LEN);
    cal.items[1].action = Action::CAL_FULL; cal.items[1].submenuIdx = -1;
    strncpy(cal.items[2].label, "Speed cal", MENU_LABEL_LEN);
    cal.items[2].action = Action::CAL_SPEED; cal.items[2].submenuIdx = -1;
    strncpy(cal.items[3].label, "..", MENU_LABEL_LEN);
    cal.items[3].action = Action::NONE; cal.items[3].submenuIdx = -1;

    // INPUT submenu (index 3)
    auto& inp = submenus[3];
    strncpy(inp.title, "Input", MENU_LABEL_LEN);
    inp.count = 5;
    strncpy(inp.items[0].label, "GPS Pos", MENU_LABEL_LEN);
    inp.items[0].action = Action::INPUT_GPS_POS; inp.items[0].submenuIdx = -1;
    strncpy(inp.items[1].label, "GPS Spd", MENU_LABEL_LEN);
    inp.items[1].action = Action::INPUT_GPS_SPD; inp.items[1].submenuIdx = -1;
    strncpy(inp.items[2].label, "WiFi", MENU_LABEL_LEN);
    inp.items[2].action = Action::INPUT_WIFI; inp.items[2].submenuIdx = -1;
    strncpy(inp.items[3].label, "Log", MENU_LABEL_LEN);
    inp.items[3].action = Action::INPUT_LOG_CYCLE; inp.items[3].submenuIdx = -1;
    strncpy(inp.items[4].label, "..", MENU_LABEL_LEN);
    inp.items[4].action = Action::NONE; inp.items[4].submenuIdx = -1;

    // DISPLAY submenu (index 4)
    auto& dsp = submenus[4];
    strncpy(dsp.title, "Display", MENU_LABEL_LEN);
    dsp.count = 5;
    strncpy(dsp.items[0].label, "Mode", MENU_LABEL_LEN);
    dsp.items[0].action = Action::DISP_MODE; dsp.items[0].submenuIdx = -1;
    strncpy(dsp.items[1].label, "Spd/ETA", MENU_LABEL_LEN);
    dsp.items[1].action = Action::DISP_SPD_ETA; dsp.items[1].submenuIdx = -1;
    strncpy(dsp.items[2].label, "Units", MENU_LABEL_LEN);
    dsp.items[2].action = Action::DISP_UNITS; dsp.items[2].submenuIdx = -1;
    strncpy(dsp.items[3].label, "Heading", MENU_LABEL_LEN);
    dsp.items[3].action = Action::DISP_HDG_TYPE; dsp.items[3].submenuIdx = -1;
    strncpy(dsp.items[4].label, "..", MENU_LABEL_LEN);
    dsp.items[4].action = Action::NONE; dsp.items[4].submenuIdx = -1;
}

// ---------------------------------------------------------------------------
// JSON loading
// ---------------------------------------------------------------------------
static bool loadFromJSON() {
    if (!LittleFS.begin(true)) return false;

    File f = LittleFS.open("/menu.json", "r");
    if (!f) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.print("[MENU] JSON parse error: ");
        Serial.println(err.c_str());
        return false;
    }

    JsonArray menus = doc["menus"];
    if (menus.isNull() || menus.size() == 0) return false;

    submenuCount = 0;
    for (JsonObject menuObj : menus) {
        if (submenuCount >= MAX_SUBMENUS) break;
        auto& sm = submenus[submenuCount];

        const char* title = menuObj["title"] | "???";
        strncpy(sm.title, title, MENU_LABEL_LEN);
        sm.title[MENU_LABEL_LEN] = '\0';

        sm.count = 0;
        JsonArray items = menuObj["items"];
        for (JsonObject itemObj : items) {
            if (sm.count >= MAX_ITEMS - 1) break;  // leave room for ".."
            auto& it = sm.items[sm.count];

            const char* label = itemObj["label"] | "???";
            strncpy(it.label, label, MENU_LABEL_LEN);
            it.label[MENU_LABEL_LEN] = '\0';

            if (itemObj["sub"].is<int>()) {
                it.action = Action::SUBMENU;
                it.submenuIdx = itemObj["sub"].as<int8_t>();
            } else {
                it.action = static_cast<Action>(itemObj["act"].as<uint8_t>());
                it.submenuIdx = -1;
            }
            sm.count++;
        }

        // Add ".." back item for non-root menus
        if (submenuCount > 0) {
            auto& back = sm.items[sm.count];
            strncpy(back.label, "..", MENU_LABEL_LEN);
            back.action = Action::NONE;
            back.submenuIdx = -1;
            sm.count++;
        }

        submenuCount++;
    }

    Serial.print("[MENU] Loaded ");
    Serial.print(submenuCount);
    Serial.println(" submenus from JSON");
    return true;
}

// ---------------------------------------------------------------------------
// Current submenu helper
// ---------------------------------------------------------------------------
static SubMenu& currentMenu() {
    return submenus[menuStack[stackDepth - 1]];
}

// ---------------------------------------------------------------------------
// Get label with toggle state suffix for display items
// ---------------------------------------------------------------------------
static void getDisplayLabel(const MenuItem& item, char* buf, size_t bufLen) {
    const char* suffix = "";
    switch (item.action) {
        case Action::INPUT_GPS_POS:
            suffix = gGpsPosEnabled ? "ON" : "OFF";
            break;
        case Action::INPUT_GPS_SPD:
            suffix = gGpsSpdEnabled ? "ON" : "OFF";
            break;
        case Action::INPUT_WIFI:
            suffix = gWifiEnabled ? "ON" : "OFF";
            break;
        case Action::INPUT_LOG_CYCLE:
            suffix = gLogLevel == 0 ? "OFF" : (gLogLevel == 1 ? "LOW" : "HI");
            break;
        case Action::DISP_MODE:
            suffix = gSettings.debugMode ? "DBG" : "NAV";
            break;
        case Action::DISP_SPD_ETA:
            suffix = gSettings.showETA ? "ETA" : "SPD";
            break;
        case Action::DISP_UNITS:
            suffix = gSettings.imperial ? "ft" : "m";
            break;
        case Action::DISP_HDG_TYPE:
            suffix = gSettings.trueHeading ? "TRUE" : "MAG";
            break;
        case Action::NAV_OP_MODE:
            suffix = gDiveMode ? "DIVE" : "SURF";
            break;
        default:
            strncpy(buf, item.label, bufLen);
            buf[bufLen - 1] = '\0';
            return;
    }
    snprintf(buf, bufLen, "%s:%s", item.label, suffix);
}

// ---------------------------------------------------------------------------
// Execute a leaf action
// ---------------------------------------------------------------------------
static void executeAction(Action act) {
    switch (act) {
        // Nav-device commands
        case Action::NAV_OUTBOUND:
            if (gSendCmd) gSendCmd(DisplayCmd::NAV_OUTBOUND);
            Serial.println("[MENU] NAV_OUTBOUND");
            break;
        case Action::NAV_HOME:
            if (gSendCmd) gSendCmd(DisplayCmd::SET_HOME);
            Serial.println("[MENU] NAV_HOME (SET_HOME)");
            break;
        case Action::NAV_MARK:
            if (gSendCmd) gSendCmd(DisplayCmd::MARK_POSITION);
            Serial.println("[MENU] MARK_POSITION");
            break;
        case Action::CAL_QUICK:
            if (gSendCmd) gSendCmd(DisplayCmd::START_MAG_CAL);
            Serial.println("[MENU] CAL_QUICK (START_MAG_CAL)");
            break;
        case Action::CAL_FULL:
            if (gSendCmd) gSendCmd(DisplayCmd::START_FULL_CAL);
            Serial.println("[MENU] CAL_FULL (START_FULL_CAL)");
            break;
        case Action::CAL_SPEED:
            gSpeedCalPending = true;  // signal display_main to enter distance selection
            Serial.println("[MENU] CAL_SPEED: entering distance selection");
            break;
        case Action::INPUT_GPS_POS:
            if (gSendCmd) gSendCmd(DisplayCmd::TOGGLE_GPS_POS);
            Serial.println("[MENU] TOGGLE_GPS_POS");
            break;
        case Action::INPUT_GPS_SPD:
            if (gSendCmd) gSendCmd(DisplayCmd::TOGGLE_GPS_SPD);
            Serial.println("[MENU] TOGGLE_GPS_SPD");
            break;
        case Action::INPUT_WIFI:
            if (gSendCmd) gSendCmd(DisplayCmd::TOGGLE_WIFI);
            Serial.println("[MENU] TOGGLE_WIFI");
            break;
        case Action::INPUT_LOG_CYCLE:
            gLogLevel = (gLogLevel + 1) % 3;  // 0->1->2->0
            if (gSendCmd) gSendCmd(DisplayCmd::CYCLE_LOG_LEVEL);
            Serial.printf("[MENU] Log level: %s\n",
                          gLogLevel == 0 ? "OFF" : (gLogLevel == 1 ? "LOW" : "HIGH"));
            break;
        // Local display settings
        case Action::DISP_MODE:
            gSettings.debugMode = !gSettings.debugMode;
            Serial.print("[MENU] Mode: "); Serial.println(gSettings.debugMode ? "DEBUG" : "NAV");
            nvs_disp::save({gSettings.debugMode, gSettings.showETA, gSettings.imperial, gSettings.trueHeading});
            break;
        case Action::DISP_SPD_ETA:
            gSettings.showETA = !gSettings.showETA;
            Serial.print("[MENU] Show: "); Serial.println(gSettings.showETA ? "ETA" : "SPEED");
            nvs_disp::save({gSettings.debugMode, gSettings.showETA, gSettings.imperial, gSettings.trueHeading});
            break;
        case Action::DISP_UNITS:
            gSettings.imperial = !gSettings.imperial;
            Serial.print("[MENU] Units: "); Serial.println(gSettings.imperial ? "ft" : "m");
            nvs_disp::save({gSettings.debugMode, gSettings.showETA, gSettings.imperial, gSettings.trueHeading});
            break;
        case Action::DISP_HDG_TYPE:
            gSettings.trueHeading = !gSettings.trueHeading;
            Serial.print("[MENU] Heading: "); Serial.println(gSettings.trueHeading ? "TRUE" : "MAG");
            nvs_disp::save({gSettings.debugMode, gSettings.showETA, gSettings.imperial, gSettings.trueHeading});
            break;
        case Action::NAV_OP_MODE:
            gDiveMode = !gDiveMode;
            if (gSendCmd) gSendCmd(DisplayCmd::TOGGLE_OP_MODE);
            Serial.print("[MENU] Op Mode: "); Serial.println(gDiveMode ? "DIVE" : "SURFACE");
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void init(SendCmdFn sendFn) {
    gSendCmd = sendFn;
    if (!loadFromJSON()) {
        Serial.println("[MENU] Using hardcoded default menu");
        loadDefaults();
    }

    // Restore display settings from NVS
    nvs_disp::State nvsDisp = nvs_disp::load();
    gSettings.debugMode   = nvsDisp.debug_mode;
    gSettings.showETA     = nvsDisp.show_eta;
    gSettings.imperial    = nvsDisp.imperial;
    gSettings.trueHeading = nvsDisp.true_heading;
    Serial.printf("[NVS] Disp restored: debug=%d eta=%d imperial=%d trueHdg=%d\n",
                  gSettings.debugMode, gSettings.showETA,
                  gSettings.imperial, gSettings.trueHeading);

    stackDepth = 0;  // closed
    invalidateMenuCache();
}

bool isOpen() {
    return stackDepth > 0;
}

void open() {
    stackDepth = 1;
    menuStack[0] = 0;  // root menu
    selectedItem = 0;
    lastActivityMs = millis();
    invalidateMenuCache();
    Serial.println("[MENU] Opened");
}

void close() {
    stackDepth = 0;
    Serial.println("[MENU] Closed");
}

void next() {
    if (!isOpen()) return;
    auto& sm = currentMenu();
    selectedItem = (selectedItem + 1) % sm.count;
    lastActivityMs = millis();
}

bool select() {
    if (!isOpen()) return false;

    auto& sm = currentMenu();
    auto& item = sm.items[selectedItem];
    lastActivityMs = millis();

    if (item.action == Action::SUBMENU && item.submenuIdx >= 0 &&
        item.submenuIdx < (int8_t)submenuCount && stackDepth < MAX_MENU_DEPTH) {
        // Enter submenu
        menuStack[stackDepth] = item.submenuIdx;
        stackDepth++;
        selectedItem = 0;
        invalidateMenuCache();
        Serial.print("[MENU] Enter: ");
        Serial.println(currentMenu().title);
        return false;
    }

    if (item.action == Action::NONE && strcmp(item.label, "..") == 0) {
        // Back
        if (stackDepth > 1) {
            stackDepth--;
            selectedItem = 0;
            invalidateMenuCache();
            Serial.println("[MENU] Back");
            return false;
        } else {
            close();
            return true;
        }
    }

    // Leaf action — for toggle items, execute but keep menu open
    bool isToggle = (item.action == Action::DISP_SPD_ETA ||
                     item.action == Action::DISP_UNITS ||
                     item.action == Action::DISP_HDG_TYPE ||
                     item.action == Action::INPUT_GPS_POS ||
                     item.action == Action::INPUT_GPS_SPD ||
                     item.action == Action::INPUT_WIFI ||
                     item.action == Action::INPUT_LOG_CYCLE ||
                     item.action == Action::NAV_OP_MODE);

    executeAction(item.action);

    if (isToggle) {
        invalidateMenuCache();  // force redraw to show new toggle state
        return false;  // keep menu open
    }

    close();
    return true;
}

void tick() {
    if (!isOpen()) return;
    if (millis() - lastActivityMs >= MENU_TIMEOUT_MS) {
        Serial.println("[MENU] Timeout — closing");
        close();
    }
}

void forceRedraw() {
    invalidateMenuCache();
}

// ---------------------------------------------------------------------------
// Render menu area (y=54..95): one item at a time in large text.
//   - Separator line at y=54
//   - Submenu title in size 1 cyan at y=56
//   - Selected item in size 2 yellow at y=70
// Cached — safe to call every frame, only changed text costs SPI bytes.
// ---------------------------------------------------------------------------
void render() {
    if (!isOpen()) return;

    auto& sm = currentMenu();

    // Separator line
    display::drawHLine(0, MENU_SEP_Y, 128, CLR_CYAN);

    // Title (cached)
    char titleBuf[22];
    snprintf(titleBuf, sizeof(titleBuf), "%-21s", sm.title);
    if (strcmp(titleBuf, prevTitle) != 0) {
        display::drawText(1, TITLE_Y, titleBuf, CLR_CYAN, 1);
        strncpy(prevTitle, titleBuf, sizeof(prevTitle));
    }

    // Current item — large text, one item at a time
    auto& item = sm.items[selectedItem];
    char labelBuf[22];
    getDisplayLabel(item, labelBuf, sizeof(labelBuf));

    // Pad to 10 chars (size 2 = 12px/char, 10 chars = 120px fits in 128px)
    char itemBuf[12];
    snprintf(itemBuf, sizeof(itemBuf), "%-10s", labelBuf);

    if (strcmp(itemBuf, prevItem) != 0) {
        display::drawText(1, ITEM_Y, itemBuf, CLR_YELLOW, 2);
        strncpy(prevItem, itemBuf, sizeof(prevItem));
    }
}

const DisplaySettings& settings() {
    return gSettings;
}

void updateNavState(uint8_t flags) {
    gGpsPosEnabled = (flags & FLAG_GPS_POS_ENABLED) != 0;
    gGpsSpdEnabled = (flags & FLAG_GPS_SPD_ENABLED) != 0;
    gWifiEnabled   = (flags & FLAG_WIFI_ENABLED)    != 0;
}

bool isPendingSpeedCal() {
    return gSpeedCalPending;
}

void clearSpeedCalPending() {
    gSpeedCalPending = false;
}

}  // namespace menu
