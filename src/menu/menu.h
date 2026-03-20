#pragma once

#include <stdint.h>
#include <dpvlink.h>

namespace menu {

// ---------------------------------------------------------------------------
// Menu geometry
// ---------------------------------------------------------------------------
constexpr int MAX_SUBMENUS    = 8;    // max number of submenus (including root)
constexpr int MAX_ITEMS       = 6;    // max items per submenu (including auto "..")
constexpr int MAX_MENU_DEPTH  = 3;    // max nesting depth (root + 2 levels)
constexpr int MENU_LABEL_LEN  = 12;   // max chars for item label

constexpr uint32_t MENU_TIMEOUT_MS = 15000;  // auto-close after 15 s idle

// ---------------------------------------------------------------------------
// Menu actions — what happens when a leaf item is selected
// ---------------------------------------------------------------------------
enum class Action : uint8_t {
    NONE = 0,
    SUBMENU,            // open child submenu (submenuIdx valid)
    // Nav-device commands (sent over serial link)
    NAV_OUTBOUND,
    NAV_HOME,
    NAV_MARK,
    CAL_QUICK,
    CAL_FULL,
    CAL_SPEED,
    INPUT_GPS_POS,
    INPUT_GPS_SPD,
    INPUT_WIFI,
    INPUT_LOG_CYCLE,
    // Local display-device settings
    DISP_MODE,          // debug vs navigate
    DISP_SPD_ETA,       // speed vs ETA
    DISP_UNITS,         // m vs ft
    DISP_HDG_TYPE,      // mag vs true
    NAV_OP_MODE,        // dive vs surface (toggle GPS + WiFi)
};

// ---------------------------------------------------------------------------
// Menu data structures
// ---------------------------------------------------------------------------
struct MenuItem {
    char     label[MENU_LABEL_LEN + 1];
    Action   action;
    int8_t   submenuIdx;   // index into submenus[] if action==SUBMENU, else -1
};

struct SubMenu {
    char     title[MENU_LABEL_LEN + 1];
    MenuItem items[MAX_ITEMS];
    uint8_t  count;        // number of items (including auto ".." for non-root)
};

// ---------------------------------------------------------------------------
// Local display settings (toggle states visible to menu)
// ---------------------------------------------------------------------------
struct DisplaySettings {
    bool debugMode;       // true = debug, false = nav
    bool showETA;         // true = ETA, false = speed
    bool imperial;        // true = ft, false = m
    bool trueHeading;     // true = true, false = mag
};

// ---------------------------------------------------------------------------
// Callback type for sending commands to nav device
// ---------------------------------------------------------------------------
using SendCmdFn = void (*)(DisplayCmd cmd);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Initialize menu system.  Call once during setup.
// Attempts to load menu.json from LittleFS; falls back to hardcoded default.
void init(SendCmdFn sendFn);

// Returns true if the menu is currently visible.
bool isOpen();

// Open menu (top-level, first item selected).
void open();

// Close menu and return to normal nav display.
void close();

// Move selection to next item (wraps around).  Resets idle timer.
void next();

// Select current item: enter submenu, execute action, or go back.
// Returns true if menu closed as a result (action executed or back from root).
bool select();

// Call from main loop — auto-closes menu after MENU_TIMEOUT_MS idle.
void tick();

// Render menu area (y=54..95).  Uses cached opaque text — safe to call
// every frame (only changed lines cost SPI bytes).
void render();

// Invalidate render cache so next render() redraws everything.
// Call after display::reinit() or display::clear() while menu is open.
void forceRedraw();

// Access current display settings (for use by display_main rendering logic).
const DisplaySettings& settings();

// Update nav-device toggle states from NavPacket flags.
// Call whenever a NavPacket is received so menu labels stay in sync.
void updateNavState(uint8_t flags);

// Returns true if the user just selected "Speed cal" and the display device
// should enter distance-selection mode.  Cleared by clearSpeedCalPending().
bool isPendingSpeedCal();
void clearSpeedCalPending();

}  // namespace menu
