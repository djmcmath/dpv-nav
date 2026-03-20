#pragma once

#include <stdint.h>

struct NavPacket;
struct DebugPacket;

namespace display {

// Initialize SPI bus and SSD1351 OLED.  Call once during setup.
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

// Draw a calibration progress screen and flush to display.
//   remaining_s    — seconds left in the calibration
//   coverage_pct   — 0-100 coverage/completeness indicator
//   isFull         — true = soft-iron data collection, false = quick hard-iron sweep
void showCal(uint8_t remaining_s, uint8_t coverage_pct, bool isFull);

// Speed calibration screens (all flush to display).
// Distance selection — user chooses how far they will swim.
void showSpeedCalDistSelect(uint16_t dist_ft);
// Waiting for flow — DPV not yet moving.
void showSpeedCalWaiting();
// Run in progress — show large elapsed-time counter.
void showSpeedCalRunning(uint16_t elapsed_s, uint16_t dist_ft);
// Accept/reject result screen.
//   choice: 0 = RESET+ACCEPT, 1 = ACCEPT, 2 = REJECT  (highlighted item)
void showSpeedCalResult(uint16_t dist_ft, uint16_t elapsed_s,
                        float k_existing, float k_proposed,
                        uint8_t choice);

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
