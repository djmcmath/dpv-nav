#pragma once

#include <stdint.h>

struct NavPacket;
struct DebugPacket;

namespace display {

// Initialize SPI bus and SSD1351 OLED.  Call once during setup.
bool init();

// Cycle through solid color fills to verify display is working.
void selfTest();

// Fill screen black and reset all caches.
void clear();

// Re-initialize display controller (recovers from brown-out or SPI disruption).
// Safe to call multiple times.  Does NOT do a hardware reset pulse.
void reinit();

// Reset all cached values so everything redraws on next frame.
void invalidateCache();

// Draw a boot diagnostic line: "label .. ok" or "label .. FAIL"
// Auto-advances Y cursor for next call.
void statusLine(const char* label, bool ok);

// Draw a single pixel (color is 16-bit 565).
void drawPixel(int x, int y, uint16_t color);

// Draw text at (x,y) with opaque black background.  size is GFX text scale.
void drawText(int x, int y, const char* text, uint16_t color, uint8_t size);

// Draw the navigation screen (incremental 4-slot frame rotation).
// Status bar + 2x2 grid: heading, bearing, range, speed.
void showNav(const NavPacket& pkt);

// Draw the debug screen (incremental 5-slot frame rotation).
// Calibrated sensor data, headings, pitch/roll.
void showDebug(const DebugPacket& pkt);

}  // namespace display
