#pragma once

#include <WString.h>

// Over-the-air firmware updates, driven from tern.local.
//
// Releases live on the Dive Map server (see dive-map backend/src/handlers/firmware.rs):
//   GET /api/firmware/dpv_nav/manifest.json
//   GET /api/firmware/dpv_nav/{version}/{nav|display}.bin
// Both are public; integrity is the manifest's sha256 + size, checked before the
// boot partition is switched. A new image then has to prove itself before it is
// kept -- see util/ota_confirm.h.
//
// An install updates the display first and nav second, skipping whichever board
// is already on the release:
//   1. display.bin streams HTTPS -> Serial1 to the display (lib/dpvlink/ota_link.h);
//      nothing is stored on nav. The display verifies it and restarts.
//   2. nav pings the display until it reports the new version -- which also
//      catches a display that rolled back to its old image.
//   3. nav.bin streams into nav's own spare slot, and nav restarts.
// A failure at any step leaves every board that hadn't finished on its old,
// working firmware.
//
// Nothing here blocks for the length of a download: update() is polled from the
// nav loop and moves at most one chunk or frame per call, so the web server keeps
// answering progress requests throughout.
namespace ota {

enum class State : uint8_t {
    IDLE,              // not checked yet
    CHECKING,          // manifest request in flight (blocking, one request)
    UP_TO_DATE,
    AVAILABLE,         // a newer release exists for at least one board
    DISPLAY_TRANSFER,  // sending display.bin over the link -- ownsDisplayLink()
    DISPLAY_VERIFY,    // waiting for the restarted display to report its version
    NAV_DOWNLOAD,      // streaming nav.bin into the inactive app slot
    REBOOTING,         // nav image verified and installed; restarting shortly
    DONE,              // display-only update finished (nav was already current)
    FAILED,            // last check or update failed -- see statusJson().error
};

// Poll every nav-loop iteration (and from the ERROR-state loop). Runs the
// one-shot manifest check once STA WiFi is up, and advances an install.
void update();

// True while the display transfer is using Serial1. The nav loop must then
// call only update() (and web::update()): no NavPackets, no command parsing --
// anything else on the link would land in the middle of the transfer.
bool ownsDisplayLink();

// Re-fetch the manifest now. Blocks for one HTTPS request. Returns a short
// human-readable result for the web page.
String checkNow();

// Begin installing the newest release. Refuses (false, errOut set) when no
// update is available, one is already running, or the unit is busy -- see
// otaBlockedReason() in nav_main.h.
bool start(String& errOut);

// {current_nav, current_display, latest, update_available, targets[], state,
//  progress, error, message, blocked_reason, releases:[{version, date, changes[]}]}
// -- releases lists only versions newer than the older of the two boards.
String statusJson();

}  // namespace ota
