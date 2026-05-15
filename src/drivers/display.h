#pragma once

#include <stdint.h>

struct NavPacket;
struct DebugPacket;
struct CalProgressPacket;

namespace display {

// Initialize SPI bus and ST7789 TFT (320x240 landscape).  Call once during setup.
bool init();

// Cycle through solid color fills to verify display is working.
void selfTest();

// Fill screen black (both canvas and hardware display).
void clear();

// Re-initialize display controller (recovers from brown-out or SPI disruption).
// Safe to call multiple times.  Does NOT do a hardware reset pulse.
void reinit();

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
// Shows pass/fail for each subsystem (IMU, GPS, cal files).
// boot_flags uses the BOOT_* bit constants from dpvlink.h.
void showBootStatus(uint8_t boot_flags);

// Draw a calibration progress screen and flush to display.
//   remaining_s    — seconds left in the calibration
//   coverage_pct   — 0-100 coverage/completeness indicator
//   isFull         — true = soft-iron data collection, false = quick hard-iron sweep
void showCal(uint8_t remaining_s, uint8_t coverage_pct, bool isFull);

// Draw the bin-coverage calibration grid screen and flush to display.
// Shows a color-coded heading × elevation grid (black/red/yellow/green per bin).
//   pkt     — CalProgressPacket with bin counts and totals
//   title   — short title string ("BASELINE CAL" or "MOUNTED CAL")
void showCalGrid(const struct CalProgressPacket& pkt, const char* title);

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
// Accept/reject result screen.
//   choice: 0 = RESET+ACCEPT, 1 = ACCEPT, 2 = REJECT  (highlighted item)
void showSpeedCalResult(uint16_t dist_ft, uint16_t elapsed_s,
                        float k_existing, float k_proposed,
                        uint8_t choice);

// Fourier heading calibration screens.
// Prompt screen: instruct user to align to a target heading and press BTN2.
//   step         — 0-based step index
//   total        — total number of steps
//   targetDeg    — target heading the user should align to (0-360)
//   indicatedDeg — live AHRS heading from NavPacket (pre-correction)
void showHdgFourierCalPrompt(int step, int total, float targetDeg, float indicatedDeg);

// Done screen: shown after all points collected and CSV saved.
//   nPoints — number of samples collected
void showHdgFourierCalDone(int nPoints);

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
