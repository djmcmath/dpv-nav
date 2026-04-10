#include "display.h"
#include "../board_pins.h"
#include "../config.h"
#include <dpvlink.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Arduino.h>
#include <math.h>

// Display dimensions (landscape orientation)
static constexpr int SCREEN_WIDTH  = 320;
static constexpr int SCREEN_HEIGHT = 240;

// 16-bit 565 colors
static constexpr uint16_t COLOR_BLACK  = 0x0000;
static constexpr uint16_t COLOR_WHITE  = 0xFFFF;
static constexpr uint16_t COLOR_RED    = 0xF800;
static constexpr uint16_t COLOR_GREEN  = 0x07E0;
static constexpr uint16_t COLOR_BLUE   = 0x001F;
static constexpr uint16_t COLOR_CYAN   = 0x07FF;
static constexpr uint16_t COLOR_YELLOW = 0xFFE0;
static constexpr uint16_t COLOR_GRAY   = 0x7BEF;

// 40 MHz SPI — ST7789 is stable at this rate on ESP32 with short traces
#define SPI_CLOCK_HZ 40000000

// Set the rotation such that the EyeSPI connector is on the bottom
#define rotation 3

// Hardware SPI — uses ESP32 VSPI peripheral
static Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// Note: 320×240×2 = 153 KB exceeds ESP32 DRAM budget, so there is no
// offscreen canvas.  All drawing goes directly to TFT hardware.
// flush() is a no-op; it exists only for API compatibility with callers
// (menu system) that expect the canvas/flush pattern.

// ---------------------------------------------------------------------------
// Rendering mode switch
// ---------------------------------------------------------------------------
// Define DISPLAY_FULL_REDRAW to use the simple clear+full-redraw approach.
// This is easy to reason about but causes visible flicker because the display
// goes black for ~30 ms before content is redrawn.
//
// Default (undefined): incremental mode — only values that changed since the
// last frame are redrawn, in-place with opaque black background.  The static
// grid lines are drawn once and never cleared.  This eliminates the flicker.
//
// #define DISPLAY_FULL_REDRAW

// ---------------------------------------------------------------------------
// Incremental nav-screen render cache
// ---------------------------------------------------------------------------
#ifndef DISPLAY_FULL_REDRAW
struct NavCache {
    bool    gridDrawn     = false;  // false → force full draw on next showNav()
    bool    bottomDirty   = false;  // true → bottom half overwritten by menu, needs redraw
    // Status bar
    uint8_t gpsFixQuality = 0xFF;
    uint8_t statusFlags   = 0xFF;   // relevant flag bits (WiFi, P, S)
    // BRG cell
    bool    hasHome       = false;
    int     bearingInt    = -999;
    bool    trueHeading   = false;
    // RNG cell (compared as integer centimetres to avoid float equality issues)
    int     distCm        = -1;
    // HDG cell
    int     headingInt    = -999;
    // SPD cell
    int     speedDisplay  = -999;
    bool    gpsSpeed      = false;
    // Log level indicator
    uint8_t logLevel      = 0xFF;
};
static NavCache navCache;

static void invalidateNavCache() { navCache = NavCache{}; }
#else
static void invalidateNavCache() {}  // no-op under full-redraw mode
#endif // DISPLAY_FULL_REDRAW

static bool tftReady = false;

// Boot status line Y cursor
static int bootLineY = 0;

// ---------------------------------------------------------------------------
// Nav mode layout constants
// ---------------------------------------------------------------------------
static constexpr int STATUS_BAR_H  = 24;   // height of status bar
static constexpr int DIV_Y_TOP     = 24;   // separator below status bar
static constexpr int DIV_Y_MID     = 132;  // horizontal mid-divider
static constexpr int DIV_X         = 159;  // vertical divider (center column)

static constexpr int CELL_UL_Y     = DIV_Y_TOP + 1;   // top-left cell origin Y
static constexpr int CELL_UR_Y     = DIV_Y_TOP + 1;   // top-right cell origin Y
static constexpr int CELL_LL_Y     = DIV_Y_MID + 1;   // bottom-left cell origin Y
static constexpr int CELL_LR_Y     = DIV_Y_MID + 1;   // bottom-right cell origin Y

namespace display {

// ===========================================================================
// Core API
// ===========================================================================

bool init() {
    // Hardware reset pulse
    pinMode(TFT_RST, OUTPUT);
    digitalWrite(TFT_RST, LOW);
    delay(10);
    digitalWrite(TFT_RST, HIGH);
    delay(10);

    // Initialize SPI bus then ST7789
    SPI.begin(DISP_SCK, DISP_MISO, DISP_MOSI);
    Serial.println("Initializing TFT display (ST7789, hardware SPI)...");
    tft.init(240, 320);          // panel dimensions (portrait: 240 wide, 320 tall)
    tft.setRotation(rotation);          // landscape: 320 wide, 240 tall
    tft.setSPISpeed(SPI_CLOCK_HZ);
    delay(50);
    tft.fillScreen(COLOR_BLACK);
    delay(50);

    tftReady = true;
    Serial.println("TFT display initialized (320x240 landscape)");
    return true;
}

void flush() {
    // No-op: no offscreen canvas; all draws go directly to TFT hardware.
    // Kept for API compatibility with callers (menu system).
}

void selfTest() {
    Serial.println("Display self-test: color cycle...");
    tft.fillScreen(COLOR_RED);   delay(500);
    tft.fillScreen(COLOR_GREEN); delay(500);
    tft.fillScreen(COLOR_BLUE);  delay(500);
    tft.fillScreen(COLOR_BLACK); delay(500);

    tft.setTextColor(COLOR_WHITE);
    tft.setTextSize(3);
    tft.setCursor(20, 80);
    tft.println("DPV-Nav");

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN);
    tft.setCursor(20, 130);
    tft.println("Display OK");
    delay(50);
    Serial.println("Display self-test complete");
}

void clear() {
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);
    delay(50);
    bootLineY = 0;
}

void reinit() {
    tft.init(240, 320);
    tft.setRotation(rotation);
    tft.setSPISpeed(SPI_CLOCK_HZ);
    delay(50);
    invalidateNavCache();  // display is blank after reinit; force full redraw
    Serial.println("[DISP] reinit complete");
}

void statusLine(const char* label, bool ok) {
    // Boot-time function — draws directly to TFT
    tft.setTextSize(2);
    tft.setCursor(0, bootLineY);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.print(label);
    delay(20);
    int labelLen = strlen(label);
    for (int i = labelLen; i < 14; i++) { tft.print('.'); delay(10); }
    if (ok) {
        tft.setTextColor(COLOR_GREEN, COLOR_BLACK);
        tft.print("ok");
    } else {
        tft.setTextColor(COLOR_RED, COLOR_BLACK);
        tft.print("FAIL");
    }
    delay(20);
    bootLineY += 18;
}

// ---------------------------------------------------------------------------
// Drawing primitives — write directly to TFT hardware
// ---------------------------------------------------------------------------

void drawPixel(int x, int y, uint16_t color) {
    tft.drawPixel(x, y, color);
}

void drawText(int x, int y, const char* text, uint16_t color, uint8_t size) {
    tft.setTextSize(size);
    tft.setTextColor(color, COLOR_BLACK);
    tft.setCursor(x, y);
    tft.print(text);
}

void fillRect(int x, int y, int w, int h, uint16_t color) {
    tft.fillRect(x, y, w, h, color);
}

void drawHLine(int x, int y, int w, uint16_t color) {
    tft.drawFastHLine(x, y, w, color);
}

// ===========================================================================
// Nav mode internals — full-frame redraw each call
// ===========================================================================

static void drawStatusBar(const NavPacket& pkt) {
    tft.setTextSize(2);

    // Layout: BATT  CAL  WiFi  GPS  P  S
    // Each size-2 char is 12px wide.
    // Positions chosen for even spacing across 320px.

    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(2, 4);
    tft.print("BATT");

    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(66, 4);
    tft.print("CAL");

    uint16_t wifiColor = (pkt.flags & FLAG_WIFI_ENABLED) ? COLOR_WHITE : COLOR_GRAY;
    tft.setTextColor(wifiColor, COLOR_BLACK);
    tft.setCursor(122, 4);
    tft.print("WiFi");

    uint16_t gpsColor;
    if (pkt.gps_fix_quality >= 2) {
        gpsColor = COLOR_GREEN;
    } else if (pkt.gps_fix_quality == 1) {
        gpsColor = COLOR_YELLOW;
    } else {
        gpsColor = COLOR_RED;
    }
    tft.setTextColor(gpsColor, COLOR_BLACK);
    tft.setCursor(207, 4);
    tft.print("GPS");

    uint16_t posColor = (pkt.flags & FLAG_GPS_POS_ENABLED) ? COLOR_WHITE : COLOR_GRAY;
    tft.setTextColor(posColor, COLOR_BLACK);
    tft.setCursor(268, 4);
    tft.print("P");

    uint16_t spdColor = (pkt.flags & FLAG_GPS_SPD_ENABLED) ? COLOR_WHITE : COLOR_GRAY;
    tft.setTextColor(spdColor, COLOR_BLACK);
    tft.setCursor(294, 4);
    tft.print("S");
}

static void drawNavGrid() {
    // Horizontal dividers
    tft.drawFastHLine(0, DIV_Y_TOP, SCREEN_WIDTH, COLOR_CYAN);
    tft.drawFastHLine(0, DIV_Y_MID, SCREEN_WIDTH, COLOR_CYAN);
    // Vertical divider
    tft.drawFastVLine(DIV_X, DIV_Y_TOP + 1, SCREEN_HEIGHT - DIV_Y_TOP - 1, COLOR_CYAN);
    // Cell labels
    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(2,   CELL_UL_Y + 2); tft.print("BRG");
    tft.setCursor(162, CELL_UR_Y + 2); tft.print("RNG");
    tft.setCursor(2,   CELL_LL_Y + 2); tft.print("HDG");
    tft.setCursor(162, CELL_LR_Y + 2); tft.print("SPD");
}

static void drawBearing(const NavPacket& pkt) {
    if (pkt.flags & FLAG_HAS_HOME) {
        int brg_int = (int)(pkt.bearing_home_deg + 0.5f) % 360;
        char suffix = (pkt.flags & FLAG_TRUE_HEADING) ? 'T' : 'M';

        // Digits (size 4 — 24px wide per char)
        char digits[4];
        snprintf(digits, sizeof(digits), "%03d", brg_int);
        tft.setTextSize(4);
        tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
        tft.setCursor(2, 54);
        tft.print(digits);

        // Suffix (size 2)
        char s[2] = { suffix, '\0' };
        tft.setTextSize(2);
        tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
        tft.setCursor(80, 58);
        tft.print(s);
    } else {
        tft.setTextSize(4);
        tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
        tft.setCursor(2, 54);
        tft.print("---");

        tft.setTextSize(2);
        tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
        tft.setCursor(80, 58);
        tft.print(" ");
    }
}

static void drawRange(const NavPacket& pkt) {
    // All branches produce exactly 6 chars so in-place overwrite (no pre-clear)
    // never leaves leftover digits from a longer previous value.
    char buf[10];
    if (pkt.flags & FLAG_HAS_HOME) {
        float dist = pkt.distance_home_m;
#if DISPLAY_UNITS_IMPERIAL
        dist *= 3.28084f;
        if (dist < 1000.0f) {
            snprintf(buf, sizeof(buf), "%4dft", (int)(dist + 0.5f));   // e.g. " 300ft" (6)
        } else {
            snprintf(buf, sizeof(buf), "%5.1fk", (double)(dist / 1000.0f)); // e.g. " 1.0k " → " 1.0k" (6)
        }
#else
        if (dist < 1000.0f) {
            snprintf(buf, sizeof(buf), "%4dm ", (int)(dist + 0.5f));   // e.g. " 300m " (6)
        } else {
            snprintf(buf, sizeof(buf), "%5.1fk", (double)(dist / 1000.0f)); // e.g. " 1.0k" (6)
        }
#endif
    } else {
        snprintf(buf, sizeof(buf), " ---  ");  // 6 chars, matches number-case width
    }

    tft.setTextSize(3);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(162, 54);
    tft.print(buf);
}

static void drawHeading(const NavPacket& pkt) {
    int hdg_int = (int)(pkt.heading_deg + 0.5f) % 360;
    char suffix = (pkt.flags & FLAG_TRUE_HEADING) ? 'T' : 'M';

    char digits[4];
    snprintf(digits, sizeof(digits), "%03d", hdg_int);
    tft.setTextSize(4);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(2, 158);
    tft.print(digits);

    char s[2] = { suffix, '\0' };
    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(80, 165);
    tft.print(s);
}

static void drawSpeed(const NavPacket& pkt) {
    char buf[8];
#if DISPLAY_UNITS_IMPERIAL
    int spd_display = (int)(pkt.speed_ms * 60.0f * 3.28084f + 0.5f);
    snprintf(buf, sizeof(buf), "%4d", spd_display);
#else
    int spd_display = (int)(pkt.speed_ms * 60.0f + 0.5f);
    snprintf(buf, sizeof(buf), "%4d", spd_display);
#endif
    tft.setTextSize(3);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(162, 158);
    tft.print(buf);

    // Meta line: "m/m GPS" or "ft/m FLW"
    const char* src = (pkt.flags & FLAG_GPS_SPEED) ? "GPS" : "FLW";
#if DISPLAY_UNITS_IMPERIAL
    char meta[10];
    snprintf(meta, sizeof(meta), "ft/m %s", src);
#else
    char meta[10];
    snprintf(meta, sizeof(meta), "m/m  %s", src);
#endif
    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(162, 212);
    tft.print(meta);
}

static void drawLogIndicator(const NavPacket& pkt) {
    uint8_t level = (pkt.flags & FLAG_LOG_LEVEL_MASK) >> FLAG_LOG_LEVEL_SHIFT;
    uint16_t color;
    const char* label;
    switch (level) {
        case 1:  color = COLOR_YELLOW; label = "L1"; break;
        case 2:  color = COLOR_RED;    label = "L2"; break;
        default: color = COLOR_GRAY;   label = "L0"; break;
    }
    tft.setTextSize(2);
    tft.setTextColor(color, COLOR_BLACK);
    tft.setCursor(2, 212);
    tft.print(label);
}

void showNav(const NavPacket& pkt) {
    if (!tftReady) return;

#ifdef DISPLAY_FULL_REDRAW
    // Simple clear + full redraw (flickers due to ~30 ms black screen)
    tft.fillScreen(COLOR_BLACK);
    drawStatusBar(pkt);
    drawNavGrid();
    drawBearing(pkt);
    drawRange(pkt);
    drawHeading(pkt);
    drawSpeed(pkt);
    drawLogIndicator(pkt);

#else // INCREMENTAL UPDATE
    // First call (or after invalidate): clear screen and draw everything
    if (!navCache.gridDrawn) {
        tft.fillScreen(COLOR_BLACK);
        drawNavGrid();
        navCache = NavCache{};
        navCache.gridDrawn = true;
    }

    // If the bottom half was overwritten by the menu overlay, redraw its
    // grid lines and force-update the bottom cells by invalidating their cache.
    if (navCache.bottomDirty) {
        tft.drawFastHLine(0, DIV_Y_MID, SCREEN_WIDTH, COLOR_CYAN);
        tft.drawFastVLine(DIV_X, DIV_Y_MID + 1, SCREEN_HEIGHT - DIV_Y_MID - 1, COLOR_CYAN);
        navCache.headingInt   = -999;
        navCache.speedDisplay = -999;
        navCache.logLevel     = 0xFF;
        navCache.bottomDirty  = false;
    }

    // Compute current values for comparison
    bool   hasHome   = (pkt.flags & FLAG_HAS_HOME) != 0;
    bool   trueHdg   = (pkt.flags & FLAG_TRUE_HEADING) != 0;
    bool   gpsSpd    = (pkt.flags & FLAG_GPS_SPEED) != 0;
    int    bearingInt = hasHome ? ((int)(pkt.bearing_home_deg + 0.5f) % 360) : -1;
    int    distCm     = hasHome ? (int)(pkt.distance_home_m * 100.0f) : -1;
    int    headingInt = (int)(pkt.heading_deg + 0.5f) % 360;
    uint8_t logLevel  = (pkt.flags & FLAG_LOG_LEVEL_MASK) >> FLAG_LOG_LEVEL_SHIFT;
    uint8_t statFlags = pkt.flags & (FLAG_WIFI_ENABLED | FLAG_GPS_POS_ENABLED | FLAG_GPS_SPD_ENABLED);
#if DISPLAY_UNITS_IMPERIAL
    int speedDisp = (int)(pkt.speed_ms * 60.0f * 3.28084f + 0.5f);
#else
    int speedDisp = (int)(pkt.speed_ms * 60.0f + 0.5f);
#endif

    // Determine which elements changed — evaluate all before updating cache
    bool statusChanged  = (statFlags != navCache.statusFlags || pkt.gps_fix_quality != navCache.gpsFixQuality);
    bool brgChanged     = (bearingInt != navCache.bearingInt || hasHome != navCache.hasHome || trueHdg != navCache.trueHeading);
    bool rngChanged     = (distCm != navCache.distCm        || hasHome != navCache.hasHome);
    bool hdgChanged     = (headingInt != navCache.headingInt || trueHdg != navCache.trueHeading);
    bool spdChanged     = (speedDisp != navCache.speedDisplay || gpsSpd != navCache.gpsSpeed);
    bool logChanged     = (logLevel != navCache.logLevel);

    // Redraw only what changed (opaque text background overwrites old content)
    if (statusChanged)  drawStatusBar(pkt);
    if (brgChanged)     drawBearing(pkt);
    if (rngChanged)     drawRange(pkt);
    if (hdgChanged)     drawHeading(pkt);
    if (spdChanged)     drawSpeed(pkt);
    if (logChanged)     drawLogIndicator(pkt);

    // Update cache
    navCache.statusFlags   = statFlags;
    navCache.gpsFixQuality = pkt.gps_fix_quality;
    navCache.hasHome       = hasHome;
    navCache.trueHeading   = trueHdg;
    navCache.bearingInt    = bearingInt;
    navCache.distCm        = distCm;
    navCache.headingInt    = headingInt;
    navCache.speedDisplay  = speedDisp;
    navCache.gpsSpeed      = gpsSpd;
    navCache.logLevel      = logLevel;
#endif // DISPLAY_FULL_REDRAW
}

void showNavTop(const NavPacket& pkt) {
    if (!tftReady) return;

#ifdef DISPLAY_FULL_REDRAW
    // Simple: clear top half and redraw from scratch
    tft.fillRect(0, 0, SCREEN_WIDTH, DIV_Y_MID, COLOR_BLACK);
    drawStatusBar(pkt);
    tft.drawFastHLine(0, DIV_Y_TOP, SCREEN_WIDTH, COLOR_CYAN);
    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(2,   CELL_UL_Y + 2); tft.print("BRG");
    tft.setCursor(162, CELL_UR_Y + 2); tft.print("RNG");
    drawBearing(pkt);
    drawRange(pkt);
    tft.drawFastVLine(DIV_X, DIV_Y_TOP + 1, DIV_Y_MID - DIV_Y_TOP - 1, COLOR_CYAN);

#else // INCREMENTAL UPDATE
    // First call (or after invalidate): draw static top-half elements once
    if (!navCache.gridDrawn) {
        tft.fillRect(0, 0, SCREEN_WIDTH, DIV_Y_MID, COLOR_BLACK);
        tft.drawFastHLine(0, DIV_Y_TOP, SCREEN_WIDTH, COLOR_CYAN);
        tft.drawFastVLine(DIV_X, DIV_Y_TOP + 1, DIV_Y_MID - DIV_Y_TOP - 1, COLOR_CYAN);
        tft.setTextSize(2);
        tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
        tft.setCursor(2,   CELL_UL_Y + 2); tft.print("BRG");
        tft.setCursor(162, CELL_UR_Y + 2); tft.print("RNG");
        navCache.gridDrawn = true;
        // Force top cells to redraw by invalidating their cached values
        navCache.bearingInt = -999;
        navCache.distCm     = -1;
        navCache.hasHome    = false;
    }

    // Bottom half will be overwritten by menu content — mark it dirty so
    // showNav() redraws it when the menu closes.
    navCache.bottomDirty = true;

    // Update only changed top-half elements
    bool hasHome  = (pkt.flags & FLAG_HAS_HOME) != 0;
    bool trueHdg  = (pkt.flags & FLAG_TRUE_HEADING) != 0;
    int  bearingInt = hasHome ? ((int)(pkt.bearing_home_deg + 0.5f) % 360) : -1;
    int  distCm     = hasHome ? (int)(pkt.distance_home_m * 100.0f) : -1;
    uint8_t statFlags = pkt.flags & (FLAG_WIFI_ENABLED | FLAG_GPS_POS_ENABLED | FLAG_GPS_SPD_ENABLED);

    if (statFlags != navCache.statusFlags || pkt.gps_fix_quality != navCache.gpsFixQuality) {
        drawStatusBar(pkt);
        navCache.statusFlags   = statFlags;
        navCache.gpsFixQuality = pkt.gps_fix_quality;
    }
    if (bearingInt != navCache.bearingInt || hasHome != navCache.hasHome || trueHdg != navCache.trueHeading) {
        drawBearing(pkt);
        navCache.bearingInt = bearingInt;
    }
    if (distCm != navCache.distCm || hasHome != navCache.hasHome) {
        drawRange(pkt);
        navCache.distCm = distCm;
    }
    navCache.hasHome     = hasHome;
    navCache.trueHeading = trueHdg;
#endif // DISPLAY_FULL_REDRAW

    // Do NOT flush — caller adds menu content then calls flush() (no-op)
}

// ===========================================================================
// Debug mode — full-frame redraw
// ===========================================================================

void showDebug(const DebugPacket& pkt) {
    if (!tftReady) return;

    invalidateNavCache();  // screen is about to be cleared; nav must redraw fully on return
    tft.fillScreen(COLOR_BLACK);
    char buf[28];

    // Static labels (size 2)
    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(0,  0);  tft.print("MAG");
    tft.setCursor(0, 40);  tft.print("ACC");
    tft.setCursor(0, 80);  tft.print("GYR");
    tft.setCursor(0, 120); tft.print("AHRS HDG:");
    tft.setCursor(0, 145); tft.print("MAG  HDG:");
    tft.setCursor(0, 170); tft.print("P:");
    tft.setCursor(160, 170); tft.print("R:");

    // Mag XYZ
    snprintf(buf, sizeof(buf), "%6.1f %6.1f %6.1f",
             (double)pkt.mag_x, (double)pkt.mag_y, (double)pkt.mag_z);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(48, 0);
    tft.print(buf);

    // Mag magnitude
    float magMag = sqrtf(pkt.mag_x * pkt.mag_x + pkt.mag_y * pkt.mag_y + pkt.mag_z * pkt.mag_z);
    snprintf(buf, sizeof(buf), "|M|=%6.1fuT", (double)magMag);
    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(48, 20);
    tft.print(buf);

    // Accel XYZ
    snprintf(buf, sizeof(buf), "%5.2f %5.2f %5.2f",
             (double)pkt.accel_x, (double)pkt.accel_y, (double)pkt.accel_z);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(48, 40);
    tft.print(buf);

    // Accel magnitude
    float accMag = sqrtf(pkt.accel_x * pkt.accel_x + pkt.accel_y * pkt.accel_y + pkt.accel_z * pkt.accel_z);
    snprintf(buf, sizeof(buf), "|A|=%5.2fg", (double)accMag);
    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(48, 60);
    tft.print(buf);

    // Gyro XYZ
    snprintf(buf, sizeof(buf), "%6.3f %6.3f %6.3f",
             (double)pkt.gyro_x, (double)pkt.gyro_y, (double)pkt.gyro_z);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(48, 80);
    tft.print(buf);

    // Fused heading
    snprintf(buf, sizeof(buf), "%5.1f", (double)pkt.fused_heading_deg);
    tft.setTextColor(COLOR_GREEN, COLOR_BLACK);
    tft.setCursor(180, 120);
    tft.print(buf);

    // Raw mag heading
    snprintf(buf, sizeof(buf), "%5.1f", (double)pkt.raw_mag_heading_deg);
    tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
    tft.setCursor(180, 145);
    tft.print(buf);

    // Pitch
    snprintf(buf, sizeof(buf), "%6.1f", (double)pkt.pitch_deg);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(26, 170);
    tft.print(buf);

    // Roll
    snprintf(buf, sizeof(buf), "%6.1f", (double)pkt.roll_deg);
    tft.setCursor(186, 170);
    tft.print(buf);
}

// ===========================================================================
// Random-rect self-test
// ===========================================================================

static bool     rectTestActive   = false;
static uint8_t  rectTestCoverage = 25;
static uint32_t rectTestLastMs   = 0;

void startRandomRectTest(uint8_t coveragePct) {
    rectTestCoverage = constrain(coveragePct, 1, 100);
    rectTestActive = true;
    rectTestLastMs = 0;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);
    Serial.printf("[DISP] random-rect test started (%u%% coverage)\n", rectTestCoverage);
}

void stopRandomRectTest() {
    rectTestActive = false;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);
    Serial.println("[DISP] random-rect test stopped");
}

bool isRandomRectTestActive() { return rectTestActive; }

void tickRandomRectTest() {
    if (!rectTestActive) return;
    uint32_t now = millis();
    if (now - rectTestLastMs < 1000) return;
    rectTestLastMs = now;

    float area = SCREEN_WIDTH * SCREEN_HEIGHT * (rectTestCoverage / 100.0f);
    float aspect = 0.5f + (random(0, 1000) / 1000.0f) * 1.5f;
    int h = (int)sqrtf(area / aspect);
    int w = (int)(h * aspect);
    if (w > SCREEN_WIDTH)  w = SCREEN_WIDTH;
    if (h > SCREEN_HEIGHT) h = SCREEN_HEIGHT;
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    int x = random(0, SCREEN_WIDTH  - w + 1);
    int y = random(0, SCREEN_HEIGHT - h + 1);

    uint16_t color = ((uint16_t)random(8, 32) << 11) |
                     ((uint16_t)random(16, 64) << 5) |
                     (uint16_t)random(8, 32);

    tft.fillRect(x, y, w, h, color);
    Serial.printf("[DISP] rect x=%d y=%d w=%d h=%d color=0x%04X\n", x, y, w, h, color);
}

// ===========================================================================
// Random-text self-test
// ===========================================================================

static bool     textTestActive   = false;
static uint8_t  textTestCoverage = 25;
static uint32_t textTestLastMs   = 0;

void startRandomTextTest(uint8_t coveragePct) {
    textTestCoverage = constrain(coveragePct, 1, 100);
    textTestActive = true;
    textTestLastMs = 0;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);
    Serial.printf("[DISP] random-text test started (%u%% coverage)\n", textTestCoverage);
}

void stopRandomTextTest() {
    textTestActive = false;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);
    Serial.println("[DISP] random-text test stopped");
}

bool isRandomTextTestActive() { return textTestActive; }

void tickRandomTextTest() {
    if (!textTestActive) return;
    uint32_t now = millis();
    if (now - textTestLastMs < 1000) return;
    textTestLastMs = now;

    static const int stringlen = 10;
    char str[stringlen];
    for (int i = 0; i < stringlen - 1; i++) {
        str[i] = (char)random(0x21, 0x7F);
    }
    str[stringlen - 1] = '\0';

    tft.fillScreen(COLOR_BLACK);
    tft.setTextSize(3);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(10, 100);
    tft.print(str);

    Serial.printf("[DISP] text '%s'\n", str);
}

// ===========================================================================
// Boot status screen
// Layout (320×240):
//   y= 0  "NAV DEVICE"          cyan, size 2
//   y=22  horizontal separator  cyan
//   y=28..118  5 status rows    label + dots + ok/FAIL, size 2
// ===========================================================================
void showBootStatus(uint8_t boot_flags) {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(0, 0);
    tft.print("NAV DEVICE");
    tft.drawFastHLine(0, 22, SCREEN_WIDTH, COLOR_CYAN);

    struct { const char* label; bool ok; } items[] = {
        { "IMU",       (bool)(boot_flags & 0x01) },
        { "GPS",       (bool)(boot_flags & 0x02) },
        { "Mag cal",   (bool)(boot_flags & 0x04) },
        { "Gyro cal",  (bool)(boot_flags & 0x08) },
        { "Accel cal", (bool)(boot_flags & 0x10) },
    };

    int y = 28;
    for (auto& item : items) {
        tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
        tft.setCursor(0, y);
        tft.print(item.label);

        // Dots from end of label to column 14 (168px at size 2)
        int col = strlen(item.label);
        tft.setCursor(col * 12, y);
        while (col < 14) { tft.print('.'); col++; }

        if (item.ok) {
            tft.setTextColor(COLOR_GREEN, COLOR_BLACK);
            tft.print("ok");
        } else {
            tft.setTextColor(COLOR_RED, COLOR_BLACK);
            tft.print("FAIL");
        }
        y += 18;
    }
}

// ===========================================================================
// Calibration progress screen
// Layout (320×240):
//   y=  4  "MAG CAL"              cyan, size 2
//   y= 26  "QUICK" / "FULL ..."   yellow, size 2
//   y= 52  "Remaining: XXXs"      white, size 2
//   y= 76  time/coverage bar      h=14
//   y=100  "Coverage: XX%"        white, size 2
//   y=124  coverage bar           h=14
//   y=160  "ROTATE DEVICE"        yellow, size 2 (blinks ~1 Hz)
// ===========================================================================
void showCal(uint8_t remaining_s, uint8_t coverage_pct, bool isFull) {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setTextSize(2);
    tft.setCursor(4, 4);
    tft.print("MAG CAL");

    tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
    tft.setCursor(4, 26);
    tft.print(isFull ? "FULL (soft-iron)" : "QUICK (hard-iron)");

    char buf[28];
    snprintf(buf, sizeof(buf), "Remaining: %3us", (unsigned)remaining_s);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(4, 52);
    tft.print(buf);

    constexpr int BAR_X = 4, BAR_W = 312, BAR_H = 14;
    tft.fillRect(BAR_X, 76, BAR_W, BAR_H, COLOR_GRAY);
    int filled = (int)(BAR_W * (int)coverage_pct / 100);
    if (filled > BAR_W) filled = BAR_W;
    if (filled > 0) tft.fillRect(BAR_X, 76, filled, BAR_H, COLOR_GREEN);

    snprintf(buf, sizeof(buf), "Coverage: %3u%%", (unsigned)coverage_pct);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(4, 100);
    tft.print(buf);

    tft.fillRect(BAR_X, 124, BAR_W, BAR_H, COLOR_GRAY);
    if (filled > 0) tft.fillRect(BAR_X, 124, filled, BAR_H, COLOR_CYAN);

    if ((millis() / 1000) & 1) {
        tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
        tft.setCursor(4, 160);
        tft.print("ROTATE DEVICE");
    }
}

// ===========================================================================
// Speed calibration screens
// ===========================================================================

// ---------------------------------------------------------------------------
// Distance selection
// Layout (320×240):
//   y=  4  "SPEED CAL"          cyan, size 2
//   y= 32  "Set distance:"      white, size 2
//   y= 80  "  300 ft  "         yellow, size 3  (big selected value, centered)
//   y=155  "BTN1: change"       gray, size 2
//   y=180  "BTN2: confirm"      gray, size 2
// ---------------------------------------------------------------------------
void showSpeedCalDistSelect(uint16_t dist_ft) {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 4);
    tft.print("SPEED CAL");

    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(4, 32);
    tft.print("Set distance:");

    char buf[16];
    snprintf(buf, sizeof(buf), "%u ft", (unsigned)dist_ft);
    tft.setTextSize(3);
    tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
    // Center horizontally (each char: 18px wide at size 3)
    int textW = (int)strlen(buf) * 18;
    int curX  = (SCREEN_WIDTH - textW) / 2;
    if (curX < 4) curX = 4;
    tft.setCursor(curX, 80);
    tft.print(buf);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(4, 155);
    tft.print("BTN1: change");
    tft.setCursor(4, 180);
    tft.print("BTN2: confirm");
}

// ---------------------------------------------------------------------------
// Waiting for flow
// Layout (320×240):
//   y=  4  "SPEED CAL"       cyan, size 2
//   y= 40  "WAITING FOR"     yellow, size 2
//   y= 66  "FLOW..."         yellow, size 2 (blinks ~1 Hz)
//   y=130  "Start moving"    white, size 2
//   y=156  "to begin run"    white, size 2
// ---------------------------------------------------------------------------
void showSpeedCalWaiting() {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 4);
    tft.print("SPEED CAL");

    tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
    tft.setCursor(4, 40);
    tft.print("WAITING FOR");
    if ((millis() / 500) & 1) {
        tft.setCursor(4, 66);
        tft.print("FLOW...");
    }

    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(4, 130);
    tft.print("Start moving");
    tft.setCursor(4, 156);
    tft.print("to begin run");
}

// ---------------------------------------------------------------------------
// Run in progress — big elapsed timer
// Layout (320×240):
//   y=  4  "SPEED CAL"              cyan, size 2
//   y= 32  "RUNNING"                green, size 2
//   y= 80  elapsed seconds          yellow, size 5 (centered)
//   y=180  "XXXft target"           white, size 2
//   y=210  "Stop=slow/turn"         gray, size 2
// ---------------------------------------------------------------------------
void showSpeedCalRunning(uint16_t elapsed_s, uint16_t dist_ft) {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 4);
    tft.print("SPEED CAL");

    tft.setTextColor(COLOR_GREEN, COLOR_BLACK);
    tft.setCursor(4, 32);
    tft.print("RUNNING");

    // Big timer (size 5 = 40px tall, 30px wide per char)
    char timeBuf[8];
    snprintf(timeBuf, sizeof(timeBuf), "%u", (unsigned)elapsed_s);
    int textW = (int)strlen(timeBuf) * 30;
    int curX  = (SCREEN_WIDTH - textW) / 2;
    if (curX < 4) curX = 4;
    tft.setTextSize(5);
    tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
    tft.setCursor(curX, 80);
    tft.print(timeBuf);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    char distBuf[20];
    snprintf(distBuf, sizeof(distBuf), "%uft target", (unsigned)dist_ft);
    tft.setCursor(4, 180);
    tft.print(distBuf);

    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(4, 210);
    tft.print("Stop=slow/turn");
}

// ---------------------------------------------------------------------------
// Accept/reject result
// Layout (320×240):
//   y=  4   "SPEED CAL"              cyan, size 2
//   y= 32   "XXXft  XXXs"            white, size 2
//   y= 58   "Cur: X.XXX"             white, size 2
//   y= 84   "New: X.XXX"             yellow, size 2
//   y=112   separator line           cyan
//   Three choice rows (each 26px tall):
//   y=118   choice 0
//   y=144   choice 1
//   y=170   choice 2
//   Highlighted choice in yellow with ">" prefix; others in gray.
// ---------------------------------------------------------------------------
void showSpeedCalResult(uint16_t dist_ft, uint16_t elapsed_s,
                        float k_existing, float k_proposed,
                        uint8_t choice) {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);

    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 4);
    tft.print("SPEED CAL");

    char buf[28];
    snprintf(buf, sizeof(buf), "%uft  %us", (unsigned)dist_ft, (unsigned)elapsed_s);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(4, 32);
    tft.print(buf);

    snprintf(buf, sizeof(buf), "Cur: %.3f", k_existing);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(4, 58);
    tft.print(buf);

    snprintf(buf, sizeof(buf), "New: %.3f", k_proposed);
    tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
    tft.setCursor(4, 84);
    tft.print(buf);

    tft.drawFastHLine(0, 112, SCREEN_WIDTH, COLOR_CYAN);

    static const char* const labels[3] = {
        "RESET+ACCEPT",
        "ACCEPT",
        "REJECT"
    };
    const int rowY[3] = { 118, 144, 170 };
    for (int i = 0; i < 3; i++) {
        if (i == (int)choice) {
            tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
            tft.setCursor(4, rowY[i]);
            tft.print("> ");
            tft.print(labels[i]);
        } else {
            tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
            tft.setCursor(4, rowY[i]);
            tft.print("  ");
            tft.print(labels[i]);
        }
    }
}

}  // namespace display
