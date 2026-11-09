#pragma once

#include <stdint.h>

struct NavPacket;
struct DebugPacket;
struct CalProgressPacket;

namespace display {

// The calibration steps as the cal screens present them
// (docs/cal-screens-unified-plan.md): 1 Baseline, 1a Fill gaps (optional),
// 2 Mounted, 3 Heading. Every cal screen's header shows all four, with this
// one marked.
enum class CalStep : uint8_t { BASELINE, FILL_GAPS, MOUNTED, HEADING };

// Why a cal ended on the failed screen, which decides what it says to do next.
enum class CalFailKind : uint8_t {
    TRANSPORT,  // the upload didn't get through: samples are on the unit, retry at tern.local
    REJECTED,   // the server got the samples and refused to fit them: collect again
    REFUSED,    // nav refused to start a gap-fill (no baseline, no target map): nothing collected
};

// Initialize SPI bus and ST7789 TFT (320x240 landscape).  Call once during setup.
bool init();

// Draw the boot logo splash screen (Tern logo, centered, white on black).
void showLogo();

// Fill screen black (both canvas and hardware display).
void clear();

// Re-initialize display controller (recovers from brown-out or SPI disruption).
// Safe to call multiple times.  Does NOT do a hardware reset pulse.
void reinit();

// Drive the backlight enable line (EyeSPI "Lite"): true = lit.
// Safe to call before init() -- it touches only the GPIO, never SPI -- and it
// releases the deep-sleep latch that sleepForPowerOff() leaves behind.
void setBacklight(bool on);

// Put the display all the way down ahead of ESP32 deep sleep: panel controller
// to DISPOFF + sleep-in, backlight off and latched off so it survives the pad
// going high-Z.  Safe whether or not init() has run -- the "woke, buttons
// released early, going straight back to sleep" path reaches it before init().
// After this, drawing calls are no-ops until init() runs again.
void sleepForPowerOff();

// Push offscreen framebuffer to display hardware (single SPI transfer).
void flush();

// Draw a boot diagnostic line: "label .. ok" or "label .. FAIL"
// Auto-advances Y cursor for next call.
void statusLine(const char* label, bool ok);

// Draw a single pixel (color is 16-bit 565).
void drawPixel(int x, int y, uint16_t color);

// Draw text at (x,y) with opaque black background.  size is GFX text scale.
void drawText(int x, int y, const char* text, uint16_t color, uint8_t size);

// Fill a rectangle with a solid color.
void fillRect(int x, int y, int w, int h, uint16_t color);

// Draw a horizontal line.
void drawHLine(int x, int y, int w, uint16_t color);

// Select ft/imperial vs m/metric formatting for distance, speed, and depth
// readouts on the nav screen. Call whenever the runtime Units setting
// changes (and once at startup) — replaces the old compile-time
// DISPLAY_UNITS_IMPERIAL macro, which never actually reached these draws.
void setImperialUnits(bool imperial);

// Mark the nav screen's heading as RAW (uncorrected magnetic). Cosmetic only;
// the value substitution happens in display_main.cpp's applyHeadingMode().
void setRawHeading(bool raw);

// Draw the full navigation screen to canvas and flush to display.
// Status bar + 2x2 grid: bearing, range, heading, speed.
void showNav(const NavPacket& pkt);

// Draw only the top row (bearing + range + status bar) to canvas.
// Does NOT flush — caller should add menu content then call flush().
void showNavTop(const NavPacket& pkt);

// Draw the full debug screen to canvas and flush to display.
// Calibrated sensor data, headings, pitch/roll.
void showDebug(const DebugPacket& pkt);

// Draw the nav-device boot status screen and flush to display.
// Shows pass/fail for each subsystem (IMU, GPS, cal files), then a firmware
// footer: one version when both boards agree, both when they don't (which is
// what an OTA that updated only nav looks like).
// boot_flags uses the BOOT_* bit constants from dpvlink.h.
// nav_fw is nav's FW_VERSION from its boot ping, or "" if it sent none.
// update_version is a newer release nav found at boot ("" = none), shown as a
// "tern.local" hint -- installing only ever happens from the web page.
void showBootStatus(uint8_t boot_flags, const char* nav_fw, const char* update_version);

// Firmware update in progress on this display (see src/display_ota.h).
//   version  -- the release being installed
//   pct      -- 0-100, or <0 to hide the progress bar
//   message  -- status line; shown red when error is true
//   full     -- repaint everything (first draw / message change); otherwise
//               only the percentage and bar are updated, so it doesn't flicker
void showFirmwareUpdate(const char* version, int pct, const char* message, bool error, bool full);

// Short full-screen notice that a newer release is available at tern.local.
void showUpdateAvailable(const char* version);

// Draw a calibration progress screen and flush to display.
//   remaining_s    — seconds left in the calibration
//   coverage_pct   — 0-100 coverage/completeness indicator
//   isFull         — true = soft-iron data collection, false = quick hard-iron sweep
void showCal(uint8_t remaining_s, uint8_t coverage_pct, bool isFull);

// The bin cals' collecting screens, in the cal frame (see setCalKeysOverlay
// below). Keys: hold BTN1 = QUIT, BTN2 = FINISH. `finishing`: FINISH was
// pressed and nav hasn't confirmed yet (it does a synchronous CSV dump
// first); the status says so and both keys go dim. pkt.complete shows the
// "Done - saving" hold.
//
// Baseline before its bootstrap locks (ROUGH_SCAN): per-axis range bars.
void showBaselineRoughScan(const struct CalProgressPacket& pkt, bool finishing);
// Baseline after it locks (BASELINE_GRID), and gap-fill: the 5 x 12 coverage
// grid with the live position dot and the roll widget.
void showCalGrid(const struct CalProgressPacket& pkt, bool finishing);
// Mounted: the 3 x 12 grid, an attitude indicator, a motion bar and a hold
// bar. A cell records only once the unit has sat still inside its row's
// attitude window, and this shows how close that is.
void showMountedCal(const struct CalProgressPacket& pkt, bool finishing);
// Whether FINISH can end this collection yet (the floors nav applies), which
// decides whether the FINISH key is lit and whether BTN2 sends it.
bool calCanFinish(const struct CalProgressPacket& pkt);

// Speed calibration screens (all flush to display).
// Distance selection — user chooses how far they will swim.
void showSpeedCalDistSelect(uint16_t dist_ft);
// Waiting for flow — DPV not yet moving.
void showSpeedCalWaiting();
// Manual countdown to force-start: secondsRemaining = 5..1 (shows "GO!" at 0, but
// caller transitions away before rendering 0).
void showSpeedCalCountdown(int secondsRemaining);
// Run in progress — show large elapsed-time counter.
void showSpeedCalRunning(uint16_t elapsed_s, uint16_t dist_ft);

// Current hold: hold station pointing upstream while the flow sensor reads the
// water. showCurrentHoldRunning shows the live flow as well as the clock -- a
// diver who is losing station needs to see it happening, not find out at the end.
void showCurrentHoldCountdown(int secondsRemaining);
// Headings arrive already converted to the diver's display mode; hdg_suffix is
// 'T', 'M' or 'R' to match.
void showCurrentHoldRunning(uint8_t remaining_s, float heading_deg, char hdg_suffix, float flow_ms);
// averaged_s = seconds that went into the average after trimming; 0 = no data.
void showCurrentHoldResult(float current_ms, float toward_deg, char toward_suffix, uint8_t averaged_s);
// Accept/reject result screen.
//   choice: 0 = RESET+ACCEPT, 1 = ACCEPT, 2 = REJECT  (highlighted item)
void showSpeedCalResult(uint16_t dist_ft, uint16_t elapsed_s,
                        float k_existing, float k_proposed,
                        uint8_t choice);

// Fourier heading calibration screens, in the cal frame. Keys: hold BTN1 =
// QUIT, BTN2 = MARK (green once the level gate is ready).
// Prompt screen: instruct user to align to a target heading and press BTN2.
//   step         — 0-based step index, i.e. marks done so far
//   total        — total number of steps
//   targetDeg    — target heading the user should align to (0-360)
//   indicatedDeg — live AHRS heading from NavPacket (pre-correction)
//   avgErrDeg    — running average of (indicated - target) over points captured
//                  so far this session; ignored unless errCount > 0
//   errCount     — number of points captured so far this session (0 = none yet)
//   levelState   — LevelGateState from NavPacket; INACTIVE = nav sent no gate
//                  data (older nav build) and the panel shows nothing
//   holdPct      — 0-100 through HDG_CAL_LEVEL_SETTLE_MS
//   pitchDeg/rollDeg — the gate's accel-only attitude (drives the horizon)
void showHdgFourierCalPrompt(int step, int total, float targetDeg, float indicatedDeg,
                              float avgErrDeg, int errCount, uint8_t levelState,
                              uint8_t holdPct, float pitchDeg, float rollDeg);

// Brief acknowledgment shown immediately on BTN2, before the step advances --
// held on screen for a fixed beat by the caller. marksDone includes this one.
// errDeg is this point's own (indicated - target); avgErrDeg is the running
// average including this point. nextTargetDeg < 0 = this was the last point.
void showHdgFourierCalAck(int marksDone, int total, float targetDeg, float indicatedDeg,
                           float errDeg, float avgErrDeg, float nextTargetDeg);

// Calibration screens share one frame: header (step strip, step name,
// counter), work area, status line, and two softkeys (BTN1 left, BTN2 right).
// See "The calibration frame" in display.cpp and
// docs/cal-screens-unified-plan.md. Every one is safe to call on every render:
// it repaints only what changed.
//
// The left softkey's hold fill (0-100) and the "Hold BTN1 to ..." hint, from
// display_main's util/cal_keys.h tracker. Call before each cal render.
void setCalKeysOverlay(uint8_t holdPct, bool hint);

// Shared closing screens, after a step's collection ends and nav uploads the
// samples for fitting (docs/cloud-calibration-plan.md).
//
// Uploading: nav is blocked in the upload and fit. secondsWaiting is cosmetic.
void showCloudCalWaiting(CalStep step, uint32_t secondsWaiting);

// No WiFi: the upload was skipped; the samples stay on the unit. BTN2: OK.
void showCloudCalOffline(CalStep step);

// The upload failed, the server refused to fit, or nav refused to start a
// gap-fill (see CalFailKind). message is shown wrapped, up to 3 lines. BTN2: OK.
void showCloudCalFailed(CalStep step, CalFailKind kind, const char* message);

// The fit succeeded: the result and the decision. BTN2 = ACCEPT, hold BTN1 =
// REJECT.
//   quality:      CalCloudQuality (GOOD/WARN/BAD), shown as GOOD/OK/POOR
//   value:        baseline/mounted: rms_pct, as a %. heading: the leave-one-out
//                 worst-case error in degrees, which arrives in the same wire
//                 field; < 0 = the server didn't send it, shown as "--".
//   coverageGaps: baseline only, empty coverage-grid cells; -1 = not applicable
//                 (mounted/hdg fit, or a pre-9-axis-firmware baseline CSV) and
//                 shows nothing extra. See baseline-cal-coverage-feedback-plan.md.
void showCloudCalResult(CalStep step, uint8_t quality, float value, int16_t coverageGaps);

// Gap-fill's upload result: nothing to accept on the unit, BTN2: OK. See the
// definition for why that fit is never offered as an install. coverageGaps
// counts thin or empty cells in this patch's own coverage; -1 = not sent.
void showGapFillUploaded(float rmsPct, int16_t coverageGaps);

// After a BTN1 hold: "Quit" (a collecting screen) or "Rejected" (a result).
void showCalDiscarded(CalStep step, bool rejected);

// Cloud account-link screens (device-code account link). Shown after the
// diver selects "Link acct" from the Config menu. No URL is ever shown --
// the diver enters this same code under "My Devices" in account settings.
//
// Waiting: shown immediately, before the nav device's first response (empty
// userCode), and again once the code arrives (nav then polls for approval
// in the background, up to ~10 min -- BTN2 cancels). secondsWaiting is
// cosmetic.
void showCloudLinkWaiting(const char* userCode, uint32_t secondsWaiting);

// No WiFi connection — flow was not attempted. BTN2 dismisses.
void showCloudLinkOffline();

// Begin/poll failed, or the nav device never responded. BTN2 dismisses.
void showCloudLinkFailed(const char* message);

// Approved — token saved on the nav device. BTN2 dismisses.
void showCloudLinkDone();

// --- Automatic dive-log upload ----------------------------------------------
// Shown for as long as NavPacket carries FLAG2_UPLOADING. The nav device is
// blocked inside an HTTPS call for most of that window and sends nothing, so
// this screen is driven by the last packet received and the display's own
// clock; display_main also suspends its link timeout while the flag is set.
// `total` is 0 before the first file's result comes back.
void showLogUpload(uint8_t done, uint8_t total, uint32_t secondsWaiting);

// --- Random-rect self-test --------------------------------------------------
// Draws a random-color rectangle at a random position once per second.
// coveragePct (1–100) controls the fraction of screen area the rect fills.
void startRandomRectTest(uint8_t coveragePct = 25);
void stopRandomRectTest();
bool isRandomRectTestActive();
// Call from loop(); draws a new rect every ~1 s when active.
void tickRandomRectTest();

// Random-text self-test: draws random strings at random positions once per second.
void startRandomTextTest(uint8_t coveragePct = 25);
void stopRandomTextTest();
bool isRandomTextTestActive();
void tickRandomTextTest();

}  // namespace display
