#include "menu.h"
#include "../drivers/display.h"
#include "../util/nvs_state.h"
#include <Arduino.h>
#include <LittleFS.h>

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
static uint8_t gLogLevel = 0;  // Log level tracking (0=OFF, 1=LOW, 2=MID, 3=HIGH)
static bool gSpeedCalPending        = false;
static bool gHdgCalPending          = false;
static bool gPowerOffPending        = false;
static bool gWaypointSelectPending  = false;
static bool gWaypointArrivePending  = false;
static bool gMarkPending            = false;
static bool gCloudLinkPending       = false;
static bool gCurrentHoldPending     = false;

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

// Turning WiFi on blocks the nav device inside wifi::init() (AP bring-up plus a
// scan-and-connect), so NavPackets stop for seconds. This latch replaces the
// menu row with "WiFi CONNECTING" and suppresses both the menu idle-close and
// the display's NO LINK screen until nav starts talking again. Turning WiFi off
// is immediate and sets none of this.
static bool     gWifiConnecting   = false;
static uint32_t gWifiConnectingMs = 0;
// Ceiling on that wait. Generous: a scan across every channel plus connect
// attempts to each stored network is the worst case, and the only cost of being
// too patient is a stale row, while being too eager flashes NO LINK for nothing.
static constexpr uint32_t WIFI_CONNECT_WAIT_MS = 45000;

static DisplaySettings gSettings = {
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
// Menu definition -- the only one. Change the menu here and reflash (or OTA).
// ---------------------------------------------------------------------------
static void addItem(SubMenu& sm, const char* label, Action action,
                    int8_t submenuIdx = -1, bool surfaceOnly = false) {
    MenuItem& it = sm.items[sm.count++];
    strncpy(it.label, label, MENU_LABEL_LEN);
    it.label[MENU_LABEL_LEN] = '\0';
    it.action      = action;
    it.submenuIdx  = submenuIdx;
    it.surfaceOnly = surfaceOnly;
}

static void buildMenu() {
    submenuCount = 4;
    for (int i = 0; i < submenuCount; i++) submenus[i].count = 0;

    // Root menu (index 0)
    //
    // The things a diver reaches for at depth sit directly on the root, so each
    // is one BTN2 away instead of behind a Nav submenu (Mark went from 5 presses
    // to 4, and to 1 with the BTN2 shortcut in display_main). Land-only items
    // are surfaceOnly: in dive mode they vanish rather than refuse, so the list
    // a diver cycles through underwater is short and holds nothing dangerous.
    //
    // Item order is also a safety property. OFF used to sit at index 0, which
    // made it both the item the menu opened on AND the item the last entry
    // wrapped onto -- two separate ways to shut the unit down by pressing BTN2
    // one beat too early. It now sits second-to-last behind a confirm (see
    // gPowerOffArmed), with the harmless "Close" after it so that overshooting
    // OFF lands on a no-op instead of on the next real action. In dive mode OFF
    // is hidden outright: there is no reason to power off underwater.
    auto& root = submenus[0];
    strncpy(root.title, "MENU", MENU_LABEL_LEN);
    addItem(root, "Select WP", Action::NAV_SELECT_WAYPOINT);
    addItem(root, "Arrive WP", Action::NAV_ARRIVE_WAYPOINT);
    addItem(root, "Mark",      Action::NAV_MARK);
    addItem(root, "Current",   Action::NAV_CURRENT);
    addItem(root, "Log",       Action::INPUT_LOG_CYCLE);
    addItem(root, "Display",   Action::SUBMENU, 1);
    // Setup stays visible at depth for Op Mode alone (everything else in it is
    // surfaceOnly): a board with no depth sensor, or one forced to DIVE on the
    // bench, must always have a way back to SURF.
    addItem(root, "Setup",     Action::SUBMENU, 2);
    addItem(root, "Cal",       Action::SUBMENU, 3, /*surfaceOnly=*/true);
    addItem(root, "OFF",       Action::POWER_OFF, -1, /*surfaceOnly=*/true);
    addItem(root, "Close",     Action::BACK);

    // DISPLAY submenu (index 1)
    auto& dsp = submenus[1];
    strncpy(dsp.title, "Display", MENU_LABEL_LEN);
    addItem(dsp, "Spd/ETA", Action::DISP_SPD_ETA);
    addItem(dsp, "Units",   Action::DISP_UNITS);
    addItem(dsp, "Heading", Action::DISP_HDG_TYPE);
    addItem(dsp, "..",      Action::BACK);

    // SETUP submenu (index 2). In dive mode only Op Mode shows. WiFi in
    // particular must stay unreachable there: underwater the radio cannot reach
    // anything, and a WiFi-on attempt blocks the nav loop for up to ~28 s (a
    // scan plus MAX_BOOT_ATTEMPTS x CONNECT_TIMEOUT_MS in wifi_manager.cpp) with
    // heading frozen and a hole in the dead reckoning. Surfacing (Op Mode, or
    // the depth trigger) turns WiFi back on.
    auto& set = submenus[2];
    strncpy(set.title, "Setup", MENU_LABEL_LEN);
    addItem(set, "GPS",       Action::INPUT_GPS,     -1, /*surfaceOnly=*/true);
    addItem(set, "WiFi",      Action::INPUT_WIFI,    -1, /*surfaceOnly=*/true);
    addItem(set, "Water",     Action::INPUT_WATER,   -1, /*surfaceOnly=*/true);
    addItem(set, "Op Mode",   Action::NAV_OP_MODE);
    addItem(set, "Link acct", Action::CLOUD_LINK,    -1, /*surfaceOnly=*/true);
    addItem(set, "..",        Action::BACK);

    // CAL submenu (index 3) -- land work, hidden in dive mode. Speed cal stays
    // here (the stick-in-shallow-water rough cal) but is not reachable at depth
    // for now; an underwater opt-in is a backlog item.
    auto& cal = submenus[3];
    strncpy(cal.title, "Cal", MENU_LABEL_LEN);
    addItem(cal, "Baseline",  Action::CAL_BASELINE);
    // Directly under Baseline: gap-fill is the second half of that job, and
    // the diver reaches for it right after the website tells them where the
    // holes are.
    addItem(cal, "Fill gaps", Action::CAL_GAPFILL);
    addItem(cal, "Mounted",   Action::CAL_MOUNTED);
    addItem(cal, "Hdg cal",   Action::CAL_HDG);
    addItem(cal, "Speed cal", Action::CAL_SPEED);
    addItem(cal, "..",        Action::BACK);
}

// ---------------------------------------------------------------------------
// Stale menu file cleanup
// ---------------------------------------------------------------------------
// The menu used to be overridable by /menu.json on this board's LittleFS. OTA
// and firmware flashes never touch the filesystem, so a file left by an old
// `uploadfs` silently hid every menu item added since (NAV > Current, Sept
// 2026). The firmware menu above is now the only menu; this removes any
// leftover file so nobody later reads it and assumes it is live.
static void removeStaleMenuFile() {
    if (!LittleFS.begin(false)) return;  // never format: nothing here needs a filesystem
    if (LittleFS.exists("/menu.json")) {
        LittleFS.remove("/menu.json");
        Serial.println("[MENU] Removed stale /menu.json (menu is built into firmware)");
    }
    LittleFS.end();
}

// ---------------------------------------------------------------------------
// Current submenu helper
// ---------------------------------------------------------------------------
static SubMenu& currentMenu() {
    return submenus[menuStack[stackDepth - 1]];
}

// ---------------------------------------------------------------------------
// Dive-mode visibility
// ---------------------------------------------------------------------------
static bool isHidden(const MenuItem& item) {
    return item.surfaceOnly && gDiveMode;
}

// First visible item at or after `from` in sm, wrapping. Every submenu ends in
// a never-hidden ".." / "Close", so this always finds one.
static uint8_t visibleFrom(const SubMenu& sm, uint8_t from) {
    for (uint8_t n = 0; n < sm.count; n++) {
        uint8_t i = (from + n) % sm.count;
        if (!isHidden(sm.items[i])) return i;
    }
    return 0;
}

// False if any submenu on the stack was entered through an item that is now
// hidden -- e.g. the diver was in Cal when the depth trigger switched to dive.
static bool stackVisible(const int8_t* stack, uint8_t depth) {
    for (uint8_t lvl = 1; lvl < depth; lvl++) {
        const SubMenu& parent = submenus[stack[lvl - 1]];
        for (uint8_t i = 0; i < parent.count; i++) {
            if (parent.items[i].submenuIdx == stack[lvl] && isHidden(parent.items[i])) {
                return false;
            }
        }
    }
    return true;
}

// Called when dive mode changes with the menu open: a hidden submenu drops the
// diver back to the root, and a hidden item moves the selection to the next
// visible one. Either way an armed OFF is disarmed.
static void ensureVisible() {
    if (!stackVisible(menuStack, stackDepth)) {
        stackDepth   = 1;
        menuStack[0] = 0;
        selectedItem = 0;
    }
    uint8_t vis = visibleFrom(currentMenu(), selectedItem);
    if (vis != selectedItem) {
        selectedItem   = vis;
        gPowerOffArmed = false;
    }
    invalidateMenuCache();
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
            suffix = gLogLevel == 0 ? "OFF"
                   : gLogLevel == 1 ? "LOW"
                   : gLogLevel == 2 ? "MID"
                   : "HI";
            break;
        case Action::INPUT_WATER:
            suffix = gSaltWater ? "SALT" : "FRESH";
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
            gMarkPending = true;  // display_main sends it and shows the ack
            Serial.println("[MENU] NAV_MARK");
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
        case Action::NAV_CURRENT:
            gCurrentHoldPending = true;  // display_main runs the countdown, nav runs the clock
            Serial.println("[MENU] NAV_CURRENT: entering current hold");
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
            // Hidden in dive mode (see the SETUP submenu in buildMenu); this is
            // the backstop should a packet flip the mode between render and press.
            if (!gWifiEnabled && gDiveMode) {
                Serial.println("[MENU] TOGGLE_WIFI refused — dive mode");
                break;
            }
            // Latch before sending: nav blocks inside wifi::init() the moment it
            // reads the command, so the row has to be claimed now or the diver
            // watches a frozen menu with no explanation. gWifiEnabled is the
            // last state nav reported, so !gWifiEnabled means this turns it on.
            if (!gWifiEnabled) {
                gWifiConnecting   = true;
                gWifiConnectingMs = millis();
                invalidateMenuCache();
            }
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
        case Action::DISP_SPD_ETA:
            gSettings.showETA = !gSettings.showETA;
            Serial.print("[MENU] Show: "); Serial.println(gSettings.showETA ? "ETA" : "SPEED");
            nvs_disp::save({gSettings.showETA, gSettings.imperial, gSettings.headingMode});
            break;
        case Action::DISP_UNITS:
            gSettings.imperial = !gSettings.imperial;
            Serial.print("[MENU] Units: "); Serial.println(gSettings.imperial ? "ft" : "m");
            nvs_disp::save({gSettings.showETA, gSettings.imperial, gSettings.headingMode});
            break;
        case Action::DISP_HDG_TYPE:
            // TRUE -> MAG -> RAW -> TRUE. RAW is deliberately in the cycle
            // rather than hidden: it's a routine step in the heading gap-fill
            // procedure, not a service mode.
            gSettings.headingMode = (gSettings.headingMode + 1) % 3;
            Serial.print("[MENU] Heading: ");
            Serial.println(headingModeLabel(gSettings.headingMode));
            nvs_disp::save({gSettings.showETA, gSettings.imperial, gSettings.headingMode});
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
    removeStaleMenuFile();
    buildMenu();

    // Restore display settings from NVS
    nvs_disp::State nvsDisp = nvs_disp::load();
    gSettings.showETA     = nvsDisp.show_eta;
    gSettings.imperial    = nvsDisp.imperial;
    gSettings.headingMode = nvsDisp.heading_mode;  // never RAW on boot (nvs_disp::load)
    Serial.printf("[NVS] Disp restored: eta=%d imperial=%d hdg=%s\n",
                  gSettings.showETA, gSettings.imperial,
                  headingModeLabel(gSettings.headingMode));

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
    // A resume point inside something dive mode has since hidden is dropped.
    if (savedDepth > 0 && (millis() - savedAtMs) < MENU_RESUME_WINDOW_MS &&
        stackVisible(savedStack, savedDepth)) {
        memcpy(menuStack, savedStack, sizeof(menuStack));
        stackDepth   = savedDepth;
        selectedItem = visibleFrom(currentMenu(), savedItem);
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
    selectedItem = visibleFrom(sm, (selectedItem + 1) % sm.count);
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
        selectedItem = visibleFrom(currentMenu(), 0);
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

    if (gWifiConnecting) {
        if (millis() - gWifiConnectingMs >= WIFI_CONNECT_WAIT_MS) {
            gWifiConnecting = false;
            invalidateMenuCache();
            Serial.println("[MENU] WiFi connect wait timed out — row released");
        } else {
            // Hold the menu open. A connect can outlast MENU_TIMEOUT_MS, and
            // closing would take the only progress indication with it — leaving
            // a diver staring at a nav screen that has stopped updating.
            lastActivityMs = millis();
        }
    }

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

    // Title (cached) — size 2, pad to 25 chars to clear previous content.
    char titleBuf[27];
    snprintf(titleBuf, sizeof(titleBuf), "%-25s", sm.title);
    if (strcmp(titleBuf, prevTitle) != 0) {
        display::drawText(1, TITLE_Y, titleBuf, CLR_CYAN, 2);
        strncpy(prevTitle, titleBuf, sizeof(prevTitle));
    }

    // Current item — size 3 large text, one item at a time
    auto& item = sm.items[selectedItem];
    char labelBuf[22];
    if (gWifiConnecting) {
        // 15 chars, inside the 16 the size-3 row holds. Takes the item's place
        // rather than adding a row: there is nowhere below it to put one, and
        // the item underneath is exactly the thing that is mid-change anyway.
        snprintf(labelBuf, sizeof(labelBuf), "WiFi CONNECTING");
    } else {
        getDisplayLabel(item, labelBuf, sizeof(labelBuf));
    }

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
    const bool wifiNow = (flags & FLAG_WIFI_ENABLED) != 0;
    // Packets only resume once nav is out of wifi::init(), and the flag it sends
    // is the authoritative state — so the first packet agreeing that WiFi is up
    // ends the wait. Anything else (a late packet queued before the block, a
    // failed bring-up that left it off) leaves the latch to time out in tick().
    if (gWifiConnecting && wifiNow) {
        gWifiConnecting = false;
        invalidateMenuCache();
    }

    gGpsEnabled    = (flags & FLAG_GPS_ENABLED)     != 0;
    gWifiEnabled   = wifiNow;
    gLogLevel      = (flags & FLAG_LOG_LEVEL_MASK) >> FLAG_LOG_LEVEL_SHIFT;
    gSaltWater     = (flags2 & FLAG2_SALT_WATER)    != 0;
    const bool diveNow = (flags2 & FLAG2_DIVE_MODE) != 0;
    if (diveNow != gDiveMode) {
        gDiveMode = diveNow;
        if (isOpen()) ensureVisible();
    }
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

bool isPendingMark() {
    return gMarkPending;
}

void clearMarkPending() {
    gMarkPending = false;
}

bool isDiveMode() {
    return gDiveMode;
}

bool isPendingCloudLink() {
    return gCloudLinkPending;
}

void clearCloudLinkPending() {
    gCloudLinkPending = false;
}

bool isWifiConnecting() {
    return gWifiConnecting;
}

bool isPendingCurrentHold() {
    return gCurrentHoldPending;
}

void clearCurrentHoldPending() {
    gCurrentHoldPending = false;
}

}  // namespace menu
