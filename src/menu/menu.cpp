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

static bool gDiveMode = false;  // Dive/surface mode, synced from NavPacket flags2 (updateNavState)
static uint8_t gLogLevel = 0;  // Log level tracking (0=OFF, 1=LOW, 2=HIGH)
static bool gSpeedCalPending        = false;
static bool gHdgCalPending          = false;
static bool gPowerOffPending        = false;
static bool gWaypointSelectPending  = false;
static bool gWaypointArrivePending  = false;
static bool gCloudLinkPending       = false;

// POWER_OFF is the one menu action that cannot be undone underwater, so it
// takes two BTN2 presses: the first arms it and relabels the item, the second
// fires. Any other navigation disarms it.
static bool gPowerOffArmed = false;

// Idle-timeout resume point. tick() stashes the position it closed from so a
// diver who paused to think reopens where they were instead of at the root.
static int8_t  savedStack[MAX_MENU_DEPTH];
static uint8_t savedDepth = 0;   // 0 = nothing saved
static uint8_t savedItem  = 0;
static uint32_t savedAtMs = 0;

// Nav-device toggle states (updated from NavPacket flags)
static bool gGpsEnabled  = true;
static bool gWifiEnabled = true;
static bool gSaltWater   = true;

static DisplaySettings gSettings = {
    .debugMode   = false,
    .showETA     = false,
    .imperial    = false,
    .headingMode = nvs_disp::HEADING_TRUE,
};

// Label for the DISPLAY > Heading item, and for the RAW marker on the nav
// screen. Kept here so the menu suffix and the nav-screen banner can never
// disagree about what mode the unit is in.
const char* headingModeLabel(uint8_t mode) {
    switch (mode) {
        case nvs_disp::HEADING_MAG: return "MAG";
        case nvs_disp::HEADING_RAW: return "RAW";
        default:                    return "TRUE";
    }
}

// Display coordinates for menu area (320×240 ST7789)
// Layout: separator line at y=132 (= DIV_Y_MID), title at y=135 (size 2),
//         single large item at y=162 (size 3, one item at a time)
constexpr int MENU_SEP_Y  = 132;
constexpr int TITLE_Y     = 135;
constexpr int ITEM_Y      = 162;  // size 3 text = 24px tall, fits in 162..186

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
static char prevTitle[27] = "";
static char prevItem[18]  = "";

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
    //
    // Item order here is a safety property, not a preference. OFF used to sit
    // at index 0, which made it both the item the menu opened on AND the item
    // Display wrapped onto -- two separate ways to shut the unit down by
    // pressing BTN2 one beat too early. It now sits second-to-last behind a
    // confirm (see gPowerOffArmed), with the harmless "Close" after it so that
    // overshooting OFF lands on a no-op instead of on the next real action.
    auto& root = submenus[0];
    strncpy(root.title, "MENU", MENU_LABEL_LEN);
    root.count = 6;
    strncpy(root.items[0].label, "Nav", MENU_LABEL_LEN);
    root.items[0].action = Action::SUBMENU; root.items[0].submenuIdx = 1;
    strncpy(root.items[1].label, "Cal", MENU_LABEL_LEN);
    root.items[1].action = Action::SUBMENU; root.items[1].submenuIdx = 2;
    strncpy(root.items[2].label, "Config", MENU_LABEL_LEN);
    root.items[2].action = Action::SUBMENU; root.items[2].submenuIdx = 3;
    strncpy(root.items[3].label, "Display", MENU_LABEL_LEN);
    root.items[3].action = Action::SUBMENU; root.items[3].submenuIdx = 4;
    strncpy(root.items[4].label, "OFF", MENU_LABEL_LEN);
    root.items[4].action = Action::POWER_OFF; root.items[4].submenuIdx = -1;
    strncpy(root.items[5].label, "Close", MENU_LABEL_LEN);
    root.items[5].action = Action::BACK; root.items[5].submenuIdx = -1;

    // NAV submenu (index 1)
    auto& nav = submenus[1];
    strncpy(nav.title, "Nav", MENU_LABEL_LEN);
    nav.count = 5;  // 4 items + back
    strncpy(nav.items[0].label, "Select WP", MENU_LABEL_LEN);
    nav.items[0].action = Action::NAV_SELECT_WAYPOINT; nav.items[0].submenuIdx = -1;
    strncpy(nav.items[1].label, "Arrive WP", MENU_LABEL_LEN);
    nav.items[1].action = Action::NAV_ARRIVE_WAYPOINT; nav.items[1].submenuIdx = -1;
    strncpy(nav.items[2].label, "Mark", MENU_LABEL_LEN);
    nav.items[2].action = Action::NAV_MARK; nav.items[2].submenuIdx = -1;
    strncpy(nav.items[3].label, "Op Mode", MENU_LABEL_LEN);
    nav.items[3].action = Action::NAV_OP_MODE; nav.items[3].submenuIdx = -1;
    strncpy(nav.items[4].label, "..", MENU_LABEL_LEN);
    nav.items[4].action = Action::BACK; nav.items[4].submenuIdx = -1;

    // CAL submenu (index 2)
    auto& cal = submenus[2];
    strncpy(cal.title, "Cal", MENU_LABEL_LEN);
    cal.count = 6;  // 5 actions + ".."
    strncpy(cal.items[0].label, "Baseline", MENU_LABEL_LEN);
    cal.items[0].action = Action::CAL_BASELINE; cal.items[0].submenuIdx = -1;
    // Directly under Baseline: gap-fill is the second half of that job, and
    // the diver reaches for it right after the website tells them where the
    // holes are. (Previously placed after Mounted despite this comment
    // already saying otherwise -- fixed to match.)
    strncpy(cal.items[1].label, "Fill gaps", MENU_LABEL_LEN);
    cal.items[1].action = Action::CAL_GAPFILL; cal.items[1].submenuIdx = -1;
    strncpy(cal.items[2].label, "Mounted", MENU_LABEL_LEN);
    cal.items[2].action = Action::CAL_MOUNTED; cal.items[2].submenuIdx = -1;
    strncpy(cal.items[3].label, "Hdg cal", MENU_LABEL_LEN);
    cal.items[3].action = Action::CAL_HDG; cal.items[3].submenuIdx = -1;
    strncpy(cal.items[4].label, "Speed cal", MENU_LABEL_LEN);
    cal.items[4].action = Action::CAL_SPEED; cal.items[4].submenuIdx = -1;
    strncpy(cal.items[5].label, "..", MENU_LABEL_LEN);
    cal.items[5].action = Action::BACK; cal.items[5].submenuIdx = -1;

    // CONFIG submenu (index 3)
    auto& inp = submenus[3];
    strncpy(inp.title, "Config", MENU_LABEL_LEN);
    inp.count = 6;
    strncpy(inp.items[0].label, "GPS", MENU_LABEL_LEN);
    inp.items[0].action = Action::INPUT_GPS; inp.items[0].submenuIdx = -1;
    strncpy(inp.items[1].label, "WiFi", MENU_LABEL_LEN);
    inp.items[1].action = Action::INPUT_WIFI; inp.items[1].submenuIdx = -1;
    strncpy(inp.items[2].label, "Log", MENU_LABEL_LEN);
    inp.items[2].action = Action::INPUT_LOG_CYCLE; inp.items[2].submenuIdx = -1;
    strncpy(inp.items[3].label, "Water", MENU_LABEL_LEN);
    inp.items[3].action = Action::INPUT_WATER; inp.items[3].submenuIdx = -1;
    strncpy(inp.items[4].label, "Link acct", MENU_LABEL_LEN);
    inp.items[4].action = Action::CLOUD_LINK; inp.items[4].submenuIdx = -1;
    strncpy(inp.items[5].label, "..", MENU_LABEL_LEN);
    inp.items[5].action = Action::BACK; inp.items[5].submenuIdx = -1;

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
    dsp.items[4].action = Action::BACK; dsp.items[4].submenuIdx = -1;
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
            if (sm.count >= MAX_ITEMS - 1) break;  // leave room for the back item
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

        // Every menu gets an explicit exit, root included. Without one at the
        // root the only ways out were the idle timeout or executing something,
        // which is what made "I just wanted to look" so expensive.
        if (sm.count < MAX_ITEMS) {
            auto& back = sm.items[sm.count];
            strncpy(back.label, submenuCount > 0 ? ".." : "Close", MENU_LABEL_LEN);
            back.label[MENU_LABEL_LEN] = '\0';
            back.action = Action::BACK;
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
        case Action::INPUT_GPS:
            suffix = gGpsEnabled ? "ON" : "OFF";
            break;
        case Action::INPUT_WIFI:
            suffix = gWifiEnabled ? "ON" : "OFF";
            break;
        case Action::INPUT_LOG_CYCLE:
            suffix = gLogLevel == 0 ? "OFF" : (gLogLevel == 1 ? "LOW" : "HI");
            break;
        case Action::INPUT_WATER:
            suffix = gSaltWater ? "SALT" : "FRESH";
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
            suffix = headingModeLabel(gSettings.headingMode);
            break;
        case Action::NAV_OP_MODE:
            suffix = gDiveMode ? "DIVE" : "SURF";
            break;
        case Action::POWER_OFF:
            // Unarmed, this renders as a plain "OFF" -- the confirm only shows
            // once the diver has actually pressed BTN2 on it.
            if (!gPowerOffArmed) {
                strncpy(buf, item.label, bufLen);
                buf[bufLen - 1] = '\0';
                return;
            }
            suffix = "SURE?";
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
        case Action::NAV_SELECT_WAYPOINT:
            gWaypointSelectPending = true;
            Serial.println("[MENU] NAV_SELECT_WAYPOINT: entering waypoint selection");
            break;
        case Action::NAV_ARRIVE_WAYPOINT:
            gWaypointArrivePending = true;
            Serial.println("[MENU] NAV_ARRIVE_WAYPOINT: entering waypoint arrival UI");
            break;
        case Action::NAV_MARK:
            if (gSendCmd) gSendCmd(DisplayCmd::MARK_POSITION);
            Serial.println("[MENU] MARK_POSITION");
            break;
        case Action::CAL_BASELINE:
            if (gSendCmd) gSendCmd(DisplayCmd::START_BASELINE_CAL);
            // Force a clean screen for the new session. showBaselineRoughScan/
            // showCalGrid only wipe their background when their own "static
            // drawn" flag is false, which is only reset by clear()/reinit() --
            // if a prior cal session this power cycle ended any way other than
            // reaching completion (aborted, display recovering from a link
            // drop mid-session), that flag is stale, and the new session's
            // bars get drawn right over whatever was on screen before.
            display::clear();
            Serial.println("[MENU] CAL_BASELINE (START_BASELINE_CAL)");
            break;
        case Action::CAL_MOUNTED:
            if (gSendCmd) gSendCmd(DisplayCmd::START_MOUNTED_CAL);
            display::clear();  // see CAL_BASELINE above
            Serial.println("[MENU] CAL_MOUNTED (START_MOUNTED_CAL)");
            break;
        case Action::CAL_GAPFILL:
            // Nav may refuse this (no installed baseline, or no synced target
            // map) and answer with a CalCloudResult FAILED screen instead of a
            // cal session. Clearing here is still right either way -- both
            // outcomes are full-screen.
            if (gSendCmd) gSendCmd(DisplayCmd::START_GAPFILL_CAL);
            display::clear();  // see CAL_BASELINE above
            Serial.println("[MENU] CAL_GAPFILL (START_GAPFILL_CAL)");
            break;
        case Action::CAL_SPEED:
            gSpeedCalPending = true;  // signal display_main to enter distance selection
            Serial.println("[MENU] CAL_SPEED: entering distance selection");
            break;
        case Action::CAL_HDG:
            gHdgCalPending = true;
            Serial.println("[MENU] CAL_HDG: entering 4-point heading cal");
            break;
        case Action::INPUT_GPS:
            if (gSendCmd) gSendCmd(DisplayCmd::TOGGLE_GPS);
            Serial.println("[MENU] TOGGLE_GPS");
            break;
        case Action::INPUT_WIFI:
            if (gSendCmd) gSendCmd(DisplayCmd::TOGGLE_WIFI);
            Serial.println("[MENU] TOGGLE_WIFI");
            break;
        case Action::INPUT_LOG_CYCLE:
            // Fire-and-forget: nav device owns the log level and echoes it back
            // in NavPacket flags (see updateNavState). gLogLevel is not touched
            // here — it tracks the authoritative value from the next packet.
            if (gSendCmd) gSendCmd(DisplayCmd::CYCLE_LOG_LEVEL);
            Serial.println("[MENU] CYCLE_LOG_LEVEL sent");
            break;
        case Action::INPUT_WATER:
            if (gSendCmd) gSendCmd(DisplayCmd::TOGGLE_WATER_DENSITY);
            Serial.println("[MENU] TOGGLE_WATER_DENSITY");
            break;
        // Local display settings
        case Action::DISP_MODE:
            gSettings.debugMode = !gSettings.debugMode;
            Serial.print("[MENU] Mode: "); Serial.println(gSettings.debugMode ? "DEBUG" : "NAV");
            nvs_disp::save({gSettings.debugMode, gSettings.showETA, gSettings.imperial, gSettings.headingMode});
            break;
        case Action::DISP_SPD_ETA:
            gSettings.showETA = !gSettings.showETA;
            Serial.print("[MENU] Show: "); Serial.println(gSettings.showETA ? "ETA" : "SPEED");
            nvs_disp::save({gSettings.debugMode, gSettings.showETA, gSettings.imperial, gSettings.headingMode});
            break;
        case Action::DISP_UNITS:
            gSettings.imperial = !gSettings.imperial;
            Serial.print("[MENU] Units: "); Serial.println(gSettings.imperial ? "ft" : "m");
            nvs_disp::save({gSettings.debugMode, gSettings.showETA, gSettings.imperial, gSettings.headingMode});
            break;
        case Action::DISP_HDG_TYPE:
            // TRUE -> MAG -> RAW -> TRUE. RAW is deliberately in the cycle
            // rather than hidden: it's a routine step in the heading gap-fill
            // procedure, not a service mode.
            gSettings.headingMode = (gSettings.headingMode + 1) % 3;
            Serial.print("[MENU] Heading: ");
            Serial.println(headingModeLabel(gSettings.headingMode));
            nvs_disp::save({gSettings.debugMode, gSettings.showETA, gSettings.imperial, gSettings.headingMode});
            break;
        case Action::NAV_OP_MODE:
            // Not optimistically toggled locally — dive mode can now also be
            // triggered automatically by depth, so gDiveMode is only ever
            // updated from the nav device's NavPacket (see updateNavState()).
            if (gSendCmd) gSendCmd(DisplayCmd::TOGGLE_OP_MODE);
            Serial.println("[MENU] TOGGLE_OP_MODE sent");
            break;
        case Action::POWER_OFF:
            gPowerOffPending = true;
            Serial.println("[MENU] POWER_OFF: entering power-off sequence");
            break;
        case Action::CLOUD_LINK:
            gCloudLinkPending = true;
            Serial.println("[MENU] CLOUD_LINK: entering account-link UI");
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
    gSettings.headingMode = nvsDisp.heading_mode;  // never RAW on boot (nvs_disp::load)
    Serial.printf("[NVS] Disp restored: debug=%d eta=%d imperial=%d hdg=%s\n",
                  gSettings.debugMode, gSettings.showETA,
                  gSettings.imperial, headingModeLabel(gSettings.headingMode));

    stackDepth = 0;  // closed
    invalidateMenuCache();
}

bool isOpen() {
    return stackDepth > 0;
}

void open() {
    // Resume where the diver was if the menu idle-timed-out recently. Past the
    // window, start clean -- dropping someone into CONFIG > Log twenty minutes
    // later is its own kind of confusing.
    if (savedDepth > 0 && (millis() - savedAtMs) < MENU_RESUME_WINDOW_MS) {
        memcpy(menuStack, savedStack, sizeof(menuStack));
        stackDepth   = savedDepth;
        selectedItem = savedItem;
        Serial.print("[MENU] Opened (resumed at ");
        Serial.print(currentMenu().title);
        Serial.println(")");
    } else {
        stackDepth   = 1;
        menuStack[0] = 0;  // root menu
        selectedItem = 0;
        Serial.println("[MENU] Opened");
    }
    savedDepth     = 0;
    gPowerOffArmed = false;
    lastActivityMs = millis();
    invalidateMenuCache();
}

void close() {
    stackDepth     = 0;
    savedDepth     = 0;   // an explicit close discards the resume point
    gPowerOffArmed = false;
    Serial.println("[MENU] Closed");
}

void next() {
    if (!isOpen()) return;
    auto& sm = currentMenu();
    selectedItem = (selectedItem + 1) % sm.count;
    // Moving off OFF cancels a pending power-off. Also covers the case where
    // the diver armed it by accident and just keeps pressing BTN1.
    if (gPowerOffArmed) {
        gPowerOffArmed = false;
        invalidateMenuCache();
        Serial.println("[MENU] Power-off disarmed");
    }
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

    if (item.action == Action::BACK) {
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

    // POWER_OFF takes two presses: the first arms it and repaints the item as
    // "OFF:SURE?", the second actually fires. This is the only action a diver
    // cannot walk back underwater.
    if (item.action == Action::POWER_OFF && !gPowerOffArmed) {
        gPowerOffArmed = true;
        invalidateMenuCache();
        Serial.println("[MENU] POWER_OFF armed — press BTN2 again to confirm");
        return false;
    }

    // Leaf action — for toggle items, execute but keep menu open
    bool isToggle = (item.action == Action::DISP_SPD_ETA ||
                     item.action == Action::DISP_UNITS ||
                     item.action == Action::DISP_HDG_TYPE ||
                     item.action == Action::INPUT_GPS ||
                     item.action == Action::INPUT_WIFI ||
                     item.action == Action::INPUT_LOG_CYCLE ||
                     item.action == Action::INPUT_WATER ||
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
        // Capture before close(), which clears the resume point on purpose.
        int8_t  stack[MAX_MENU_DEPTH];
        memcpy(stack, menuStack, sizeof(stack));
        uint8_t depth = stackDepth;
        uint8_t item  = selectedItem;
        close();
        memcpy(savedStack, stack, sizeof(savedStack));
        savedDepth = depth;
        savedItem  = item;
        savedAtMs  = millis();
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
    display::drawHLine(0, MENU_SEP_Y, 320, CLR_CYAN);

    // Title (cached) — size 2, pad to 25 chars to clear previous content
    char titleBuf[27];
    snprintf(titleBuf, sizeof(titleBuf), "%-25s", sm.title);
    if (strcmp(titleBuf, prevTitle) != 0) {
        display::drawText(1, TITLE_Y, titleBuf, CLR_CYAN, 2);
        strncpy(prevTitle, titleBuf, sizeof(prevTitle));
    }

    // Current item — size 3 large text, one item at a time
    auto& item = sm.items[selectedItem];
    char labelBuf[22];
    getDisplayLabel(item, labelBuf, sizeof(labelBuf));

    // Pad to 16 chars (size 3 = 18px/char, 16 chars = 288px fits in 320px)
    char itemBuf[18];
    snprintf(itemBuf, sizeof(itemBuf), "%-16s", labelBuf);

    if (strcmp(itemBuf, prevItem) != 0) {
        display::drawText(1, ITEM_Y, itemBuf, CLR_YELLOW, 3);
        strncpy(prevItem, itemBuf, sizeof(prevItem));
    }
}

const DisplaySettings& settings() {
    return gSettings;
}

void updateNavState(uint8_t flags, uint8_t flags2) {
    gGpsEnabled    = (flags & FLAG_GPS_ENABLED)     != 0;
    gWifiEnabled   = (flags & FLAG_WIFI_ENABLED)    != 0;
    gLogLevel      = (flags & FLAG_LOG_LEVEL_MASK) >> FLAG_LOG_LEVEL_SHIFT;
    gSaltWater     = (flags2 & FLAG2_SALT_WATER)    != 0;
    gDiveMode      = (flags2 & FLAG2_DIVE_MODE)     != 0;
}

bool isPendingSpeedCal() {
    return gSpeedCalPending;
}

void clearSpeedCalPending() {
    gSpeedCalPending = false;
}

bool isPendingHdgCal() {
    return gHdgCalPending;
}

void clearHdgCalPending() {
    gHdgCalPending = false;
}

bool isPendingPowerOff() {
    return gPowerOffPending;
}

void clearPowerOffPending() {
    gPowerOffPending = false;
}

bool isPendingWaypointSelect() {
    return gWaypointSelectPending;
}

void clearWaypointSelectPending() {
    gWaypointSelectPending = false;
}

bool isPendingWaypointArrive() {
    return gWaypointArrivePending;
}

void clearWaypointArrivePending() {
    gWaypointArrivePending = false;
}

bool isPendingCloudLink() {
    return gCloudLinkPending;
}

void clearCloudLinkPending() {
    gCloudLinkPending = false;
}

}  // namespace menu
