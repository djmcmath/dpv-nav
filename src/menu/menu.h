#pragma once

#include <stdint.h>
#include <dpvlink.h>

#include "../util/nvs_state.h"  // nvs_disp::HeadingMode

namespace menu {

// ---------------------------------------------------------------------------
// Menu geometry
// ---------------------------------------------------------------------------
constexpr int MAX_SUBMENUS    = 8;    // max number of submenus (including root)
constexpr int MAX_ITEMS       = 8;    // max items per submenu (including auto back item)
constexpr int MAX_MENU_DEPTH  = 3;    // max nesting depth (root + 2 levels)
constexpr int MENU_LABEL_LEN  = 12;   // max chars for item label

constexpr uint32_t MENU_TIMEOUT_MS = 45000;  // auto-close after 45 s idle

// After an idle timeout, reopening the menu within this window resumes at the
// item the diver was last on instead of dumping them back at the root. Hunting
// seven items deep for Log with gloves on, twice, is how the old 15 s timeout
// lost people mid-dive.
constexpr uint32_t MENU_RESUME_WINDOW_MS = 120000;

// ---------------------------------------------------------------------------
// Menu actions — what happens when a leaf item is selected
// ---------------------------------------------------------------------------
enum class Action : uint8_t {
    NONE          = 0,
    SUBMENU       = 1,   // open child submenu (submenuIdx valid)
    // Nav-device commands (sent over serial link)
    NAV_OUTBOUND  = 2,
    NAV_HOME      = 3,
    NAV_MARK      = 4,
    CAL_BASELINE  = 5,   // bin-aware baseline (off-scooter) cal
    CAL_MOUNTED   = 6,   // bin-aware mounted (on-scooter) cal
    CAL_SPEED     = 7,
    INPUT_GPS     = 8,   // toggle GPS usage (position + speed together)
    // 9 retired (was INPUT_GPS_SPD — split GPS pos/speed menu items merged into one)
    INPUT_WIFI    = 10,
    INPUT_LOG_CYCLE = 11,
    // Local display-device settings
    DISP_MODE     = 12,  // debug vs navigate
    DISP_SPD_ETA  = 13,  // speed vs ETA
    DISP_UNITS    = 14,  // m vs ft
    DISP_HDG_TYPE = 15,  // mag vs true
    NAV_OP_MODE          = 16,  // dive vs surface (toggle GPS + WiFi)
    POWER_OFF            = 17,  // save state and enter deep sleep
    CAL_HDG              = 18,  // 4-point heading calibration
    NAV_SELECT_WAYPOINT  = 19,  // open waypoint selection UI (navigate TO a waypoint)
    NAV_ARRIVE_WAYPOINT  = 20,  // open waypoint arrival UI (snap position TO a waypoint)
    CLOUD_LINK           = 21,  // begin device-auth cloud account link (RFC 8628)
    INPUT_WATER          = 22,  // toggle salt/fresh water density (depth calc)
    CAL_GAPFILL          = 23,  // guided gap-fill pass over the cells the server flagged
    BACK                 = 24,  // leave submenu, or close the menu at root
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
    bool    debugMode;    // true = debug, false = nav
    bool    showETA;      // true = ETA, false = speed
    bool    imperial;     // true = ft, false = m
    uint8_t headingMode;  // nvs_disp::HeadingMode (TRUE / MAG / RAW)
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

// Open menu. Resumes the last position if the menu idle-timed-out within
// MENU_RESUME_WINDOW_MS, otherwise opens at the root's first item.
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

// "TRUE" / "MAG" / "RAW" for a nvs_disp::HeadingMode value.
const char* headingModeLabel(uint8_t mode);

// Update nav-device toggle states from NavPacket flags/flags2.
// Call whenever a NavPacket is received so menu labels stay in sync.
void updateNavState(uint8_t flags, uint8_t flags2);

// Returns true if the user just selected "Speed cal" and the display device
// should enter distance-selection mode.  Cleared by clearSpeedCalPending().
bool isPendingSpeedCal();
void clearSpeedCalPending();

// Returns true if the user just selected "Hdg cal" and the display device
// should enter the 4-point heading calibration UI.  Cleared by clearHdgCalPending().
bool isPendingHdgCal();
void clearHdgCalPending();

// Returns true if the user just selected "OFF" and the display device
// should begin the power-off sequence.  Cleared by clearPowerOffPending().
bool isPendingPowerOff();
void clearPowerOffPending();

// Returns true if the user just selected "Select WP" — display should enter
// waypoint-selection UI.  Cleared by clearWaypointSelectPending().
bool isPendingWaypointSelect();
void clearWaypointSelectPending();

// Returns true if the user just selected "Arrive WP" — display should enter
// waypoint-arrival UI (snaps position to chosen waypoint).
// Cleared by clearWaypointArrivePending().
bool isPendingWaypointArrive();
void clearWaypointArrivePending();

// Returns true if the user just selected "Link acct" — display should send
// DisplayCmd::LINK_ACCOUNT and enter the cloud account-link UI.
// Cleared by clearCloudLinkPending().
bool isPendingCloudLink();
void clearCloudLinkPending();

}  // namespace menu
