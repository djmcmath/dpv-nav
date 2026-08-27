#include "display.h"
#include "logo_bitmap.h"
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
static constexpr uint16_t COLOR_GRAY     = 0x7BEF;
static constexpr uint16_t COLOR_DIM_GRAY = 0x2104;  // very dark gray for empty signal bars

// 40 MHz SPI — ST7789 is stable at this rate on ESP32 with short traces
#define SPI_CLOCK_HZ 40000000

// Set the rotation such that the EyeSPI connector is on the bottom
// rotation 3: SPI connector on the bottom
// rotation 1: SPI connector on the top
#define rotation 1

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
    uint8_t gpsSignalBars = 0xFF;   // 0–4 computed signal bar count
    uint8_t gpsSatellites = 0xFF;
    uint8_t gpsHdopX10    = 0xFF;
    uint8_t gpsAntenna    = 0xFF;
    uint8_t wifiDispFlags = 0xFF;   // FLAG_WIFI_ENABLED | FLAG2_WIFI_CLIENT combined
    uint8_t battLevel     = 0xFF;   // 0=unknown,1=red,2=yellow,3=green (avoids redraw on mV jitter)
    // BRG cell
    bool    hasHome       = false;
    int     bearingInt    = -999;
    bool    trueHeading   = false;
    // RNG cell (compared as integer centimetres to avoid float equality issues)
    int     distCm        = -1;
    // HDG cell
    int     headingInt    = -999;
    bool    rawHeading    = false;  // last-seen RAW mode, to force redraw on toggle
    // SPD cell
    int     speedDisplay  = -999;
    bool    gpsSpeed      = false;
    // Log level indicator
    uint8_t logLevel      = 0xFF;
    // Depth readout (compared as integer centimetres; -1 = not present/blank)
    int     depthCm       = -999;
    bool    imperialUnits = false;  // last-seen units setting, to force redraw on toggle
};
static NavCache navCache;

static void invalidateNavCache() { navCache = NavCache{}; }
#else
static void invalidateNavCache() {}  // no-op under full-redraw mode
#endif // DISPLAY_FULL_REDRAW

static bool tftReady = false;

// Runtime units setting (ft/m), pushed in from display_main.cpp whenever the
// Units menu setting changes. Replaces the old DISPLAY_UNITS_IMPERIAL macro,
// which was fixed at compile time and never actually reachable from the menu.
static bool gImperialUnits = false;

void display::setImperialUnits(bool imperial) { gImperialUnits = imperial; }

// True while DISPLAY > Heading is RAW, so the nav screen can mark the heading
// as deliberately uncorrected. Purely cosmetic -- the substitution itself
// happens in display_main.cpp's applyHeadingMode().
static bool gRawHeading = false;

void display::setRawHeading(bool raw) { gRawHeading = raw; }

// Boot status line Y cursor
static int bootLineY = 0;

// Cal grid: track whether static layout (title, labels) has been drawn for
// the current cal session.  Reset by clear() so each new session starts fresh.
static bool g_calGridStaticDrawn = false;

// Baseline ROUGH_SCAN screen: same idea, separate flag since it's a different
// static layout from showCalGrid. Baseline stays in ROUGH_SCAN for its whole
// session now (see display_main.cpp's FINISH_BASELINE_COLLECTION handler).
static bool g_roughScanStaticDrawn = false;

// Cloud-link waiting screen: same idea as g_calGridStaticDrawn -- the static
// header/instructions/code are drawn once, and each subsequent call (this
// screen is redrawn at 4 Hz while the elapsed-seconds counter ticks) only
// repaints the small counter region. Reset by clear() so each new session
// starts fresh; also forced by a userCode change (STARTING's "" -> the real
// code once CODE_READY arrives changes the static content itself).
static bool g_cloudLinkWaitingStaticDrawn = false;
static char g_cloudLinkWaitingLastCode[16] = "";

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

void showLogo() {
    if (!tftReady) return;
    tft.fillScreen(COLOR_BLACK);
    int16_t x = (SCREEN_WIDTH  - LOGO_W) / 2;
    int16_t y = (SCREEN_HEIGHT - LOGO_H) / 2;
    tft.drawBitmap(x, y, logo_bitmap, LOGO_W, LOGO_H, COLOR_WHITE, COLOR_BLACK);
}

void clear() {
    invalidateNavCache();
    g_calGridStaticDrawn = false;  // next showCalGrid call redraws static elements
    g_roughScanStaticDrawn = false;
    g_cloudLinkWaitingStaticDrawn = false;
    g_cloudLinkWaitingLastCode[0] = '\0';
    tft.fillScreen(COLOR_BLACK);
    delay(50);
    bootLineY = 0;
}

void reinit() {
    tft.init(240, 320);
    tft.setRotation(rotation);
    tft.setSPISpeed(SPI_CLOCK_HZ);
    delay(50);
    invalidateNavCache();
    g_calGridStaticDrawn = false;  // display is blank after reinit; force full redraw
    g_roughScanStaticDrawn = false;
    g_cloudLinkWaitingStaticDrawn = false;
    g_cloudLinkWaitingLastCode[0] = '\0';
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
    // Layout (size-2 text = 12px/char × 16px tall; bar height = 24px):
    //   x=2:   BATT (48px) + level bar
    //   x=72:  WiFi (48px) + "AP" when in AP mode
    //   x=158: GPS (36px) + signal bars + sat count + antenna char + HDOP
    tft.setTextSize(2);

    // ---- BATT ---------------------------------------------------------------
    uint16_t battColor;
    if      (pkt.batt_mv == 0)              battColor = COLOR_GRAY;
    else if (pkt.batt_mv >= BATT_MV_GREEN)  battColor = COLOR_GREEN;
    else if (pkt.batt_mv >= BATT_MV_YELLOW) battColor = COLOR_YELLOW;
    else                                     battColor = COLOR_RED;

    tft.setTextColor(battColor, COLOR_BLACK);
    tft.setCursor(2, 4);
    tft.print("BATT");

    static constexpr int BBAR_X = 54;
    static constexpr int BBAR_Y = 5;
    static constexpr int BBAR_W = 4;
    static constexpr int BBAR_H = 14;
    tft.drawRect(BBAR_X, BBAR_Y, BBAR_W, BBAR_H, battColor);
    if (pkt.batt_mv > 0) {
        uint16_t mv = pkt.batt_mv < BATT_MV_EMPTY ? BATT_MV_EMPTY
                    : pkt.batt_mv > BATT_MV_FULL  ? BATT_MV_FULL
                    : pkt.batt_mv;
        int fillH = (int)((mv - BATT_MV_EMPTY) * (BBAR_H - 2) / (BATT_MV_FULL - BATT_MV_EMPTY));
        tft.fillRect(BBAR_X + 1, BBAR_Y + 1, BBAR_W - 2, BBAR_H - 2, COLOR_BLACK);
        if (fillH > 0)
            tft.fillRect(BBAR_X + 1, BBAR_Y + 1 + (BBAR_H - 2 - fillH), BBAR_W - 2, fillH, battColor);
    } else {
        tft.fillRect(BBAR_X + 1, BBAR_Y + 1, BBAR_W - 2, BBAR_H - 2, COLOR_BLACK);
    }

    // ---- WiFi ---------------------------------------------------------------
    // Colors: gray=disabled, yellow=own AP, green=connected to stored AP
    bool wifiEnabled = (pkt.flags  & FLAG_WIFI_ENABLED) != 0;
    bool wifiClient  = (pkt.flags2 & FLAG2_WIFI_CLIENT) != 0;

    uint16_t wifiColor;
    if      (!wifiEnabled) wifiColor = COLOR_GRAY;
    else if (wifiClient)   wifiColor = COLOR_GREEN;
    else                   wifiColor = COLOR_YELLOW;

    tft.setTextColor(wifiColor, COLOR_BLACK);
    tft.setCursor(72, 4);
    tft.print("WiFi");

    // "AP" badge visible only when enabled and in AP mode; cleared otherwise
    if (wifiEnabled && !wifiClient) {
        tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
        tft.setCursor(122, 4);
        tft.print("AP");
    } else {
        tft.fillRect(122, 0, 24, STATUS_BAR_H, COLOR_BLACK);
    }

    // ---- GPS ----------------------------------------------------------------
    // Label: gray when GPS usage is off (dive mode)
    bool gpsEnabled = (pkt.flags & FLAG_GPS_ENABLED) != 0;
    uint16_t gpsBaseColor = gpsEnabled ? COLOR_WHITE : COLOR_GRAY;

    tft.setTextColor(gpsBaseColor, COLOR_BLACK);
    tft.setCursor(158, 4);
    tft.print("GPS");

    // Signal bars (x=198; 4 bars × 3px wide, 2px gap each)
    static constexpr int BAR_X0         = 198;
    static constexpr int BAR_W          = 3;
    static constexpr int BAR_GAP        = 2;
    static constexpr int BAR_BOTTOM_Y   = 21;
    static constexpr int BAR_HEIGHTS[4] = { 5, 9, 13, 17 };

    int numBars = pkt.gps_signal_bars;
    for (int i = 0; i < 4; i++) {
        int x = BAR_X0 + i * (BAR_W + BAR_GAP);
        int h = BAR_HEIGHTS[i];
        int y = BAR_BOTTOM_Y - h + 1;
        uint16_t barColor = (i < numBars) ? gpsBaseColor : COLOR_DIM_GRAY;
        tft.fillRect(x, y, BAR_W, h, barColor);
        if (y > 1) tft.fillRect(x, 1, BAR_W, y - 1, COLOR_BLACK);
    }

    // Satellite count (x=220; 2 chars right-aligned, white when fix active)
    uint16_t satColor = (gpsEnabled && pkt.gps_satellites > 0) ? COLOR_WHITE : COLOR_GRAY;
    tft.setTextColor(satColor, COLOR_BLACK);
    tft.setCursor(220, 4);
    char satBuf[3];
    snprintf(satBuf, sizeof(satBuf), "%2u", (unsigned)pkt.gps_satellites);
    tft.print(satBuf);

    // Antenna indicator (x=248; single char)
    // 0=unknown→gray "?", 1=error→red "!", 2=internal→white "I", 3=external→green "E"
    uint16_t antColor;
    char antChar;
    switch (pkt.gps_antenna) {
        case 1:  antColor = COLOR_RED;    antChar = '!'; break;
        case 2:  antColor = COLOR_WHITE;  antChar = 'I'; break;
        case 3:  antColor = COLOR_GREEN;  antChar = 'E'; break;
        default: antColor = COLOR_GRAY;   antChar = '?'; break;
    }
    tft.setTextColor(antColor, COLOR_BLACK);
    tft.setCursor(248, 4);
    tft.print(antChar);

    // HDOP (x=264; "X.X" colored by quality; "---" when no fix)
    uint16_t hdopColor;
    char hdopBuf[4];
    if (pkt.gps_hdop_x10 == 0) {
        hdopColor = COLOR_GRAY;
        snprintf(hdopBuf, sizeof(hdopBuf), "---");
    } else {
        uint8_t hd = pkt.gps_hdop_x10 > 99 ? 99 : pkt.gps_hdop_x10;
        if      (hd < 15) hdopColor = COLOR_GREEN;   // < 1.5
        else if (hd < 25) hdopColor = COLOR_YELLOW;  // 1.5–2.4
        else              hdopColor = COLOR_RED;      // ≥ 2.5
        snprintf(hdopBuf, sizeof(hdopBuf), "%u.%u", hd / 10, hd % 10);
    }
    tft.setTextColor(hdopColor, COLOR_BLACK);
    tft.setCursor(264, 4);
    tft.print(hdopBuf);
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
        if (gImperialUnits) {
            dist *= 3.28084f;
            if (dist < 1000.0f) {
                snprintf(buf, sizeof(buf), "%4dft", (int)(dist + 0.5f));   // e.g. " 300ft" (6)
            } else {
                snprintf(buf, sizeof(buf), "%5.1fk", (double)(dist / 1000.0f)); // e.g. " 1.0k " → " 1.0k" (6)
            }
        } else {
            if (dist < 1000.0f) {
                snprintf(buf, sizeof(buf), "%4dm ", (int)(dist + 0.5f));   // e.g. " 300m " (6)
            } else {
                snprintf(buf, sizeof(buf), "%5.1fk", (double)(dist / 1000.0f)); // e.g. " 1.0k" (6)
            }
        }
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
    // RAW is an uncorrected bench reading -- colour it and suffix it 'R' so it
    // can never be mistaken for a heading worth navigating on.
    char suffix = gRawHeading ? 'R' : ((pkt.flags & FLAG_TRUE_HEADING) ? 'T' : 'M');

    char digits[4];
    snprintf(digits, sizeof(digits), "%03d", hdg_int);
    tft.setTextSize(4);
    tft.setTextColor(gRawHeading ? COLOR_YELLOW : COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(2, 158);
    tft.print(digits);

    char s[2] = { suffix, '\0' };
    tft.setTextSize(2);
    tft.setTextColor(gRawHeading ? COLOR_YELLOW : COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(80, 165);
    tft.print(s);
}

static void drawSpeed(const NavPacket& pkt) {
    char buf[8];
    int spd_display = gImperialUnits
        ? (int)(pkt.speed_ms * 60.0f * 3.28084f + 0.5f)
        : (int)(pkt.speed_ms * 60.0f + 0.5f);
    snprintf(buf, sizeof(buf), "%4d", spd_display);
    tft.setTextSize(3);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(162, 158);
    tft.print(buf);

    // Meta line: "m/m GPS" or "ft/m FLW"
    const char* src = (pkt.flags & FLAG_GPS_SPEED) ? "GPS" : "FLW";
    char meta[10];
    snprintf(meta, sizeof(meta), gImperialUnits ? "ft/m %s" : "m/m  %s", src);
    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(162, 212);
    tft.print(meta);
}

// Bottom-row depth readout, in the gap between the log-level indicator
// (x=2) and the speed-source meta text (x=162). Blank on boards without a
// depth sensor. Fixed 7-char width in both unit modes for safe in-place
// overwrite (matches drawRange()'s fixed-width convention).
static void drawDepth(const NavPacket& pkt) {
    char buf[9];
    if (pkt.flags2 & FLAG2_DEPTH_PRESENT) {
        if (gImperialUnits) {
            snprintf(buf, sizeof(buf), "%5.1fft", (double)(pkt.depth_m * 3.28084f));
        } else {
            snprintf(buf, sizeof(buf), "%5.1fm ", (double)pkt.depth_m);
        }
    } else {
        snprintf(buf, sizeof(buf), "       ");  // 7 spaces — nothing to show
    }
    tft.setTextSize(2);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(60, 212);
    tft.print(buf);
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
    drawDepth(pkt);

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
        navCache.depthCm      = -999;
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
    uint8_t wifiDispFlags = (pkt.flags & FLAG_WIFI_ENABLED) | (pkt.flags2 & FLAG2_WIFI_CLIENT);
    uint8_t battLevel = (pkt.batt_mv == 0)              ? 0
                      : (pkt.batt_mv >= BATT_MV_GREEN)  ? 3
                      : (pkt.batt_mv >= BATT_MV_YELLOW) ? 2
                      : 1;
    int speedDisp = gImperialUnits
        ? (int)(pkt.speed_ms * 60.0f * 3.28084f + 0.5f)
        : (int)(pkt.speed_ms * 60.0f + 0.5f);
    bool depthPresent = (pkt.flags2 & FLAG2_DEPTH_PRESENT) != 0;
    int  depthCm       = depthPresent ? (int)(pkt.depth_m * 100.0f) : -999;

    // Unit toggling doesn't change distCm/speedDisp/depthCm (stored in SI),
    // so it wouldn't otherwise trigger a redraw of the cells whose *text*
    // needs to flip ft<->m — force it explicitly.
    bool unitsChanged = (gImperialUnits != navCache.imperialUnits);

    // Determine which elements changed — evaluate all before updating cache
    bool statusChanged  = (wifiDispFlags          != navCache.wifiDispFlags
                        || pkt.gps_signal_bars   != navCache.gpsSignalBars
                        || pkt.gps_satellites    != navCache.gpsSatellites
                        || pkt.gps_hdop_x10      != navCache.gpsHdopX10
                        || pkt.gps_antenna       != navCache.gpsAntenna
                        || battLevel             != navCache.battLevel);
    bool brgChanged     = (bearingInt != navCache.bearingInt || hasHome != navCache.hasHome || trueHdg != navCache.trueHeading);
    bool rngChanged     = (distCm != navCache.distCm        || hasHome != navCache.hasHome || unitsChanged);
    // Entering/leaving RAW recolours the cell without necessarily changing the
    // number, so it has to force the redraw itself.
    bool hdgChanged     = (headingInt != navCache.headingInt || trueHdg != navCache.trueHeading
                        || gRawHeading != navCache.rawHeading);
    bool spdChanged     = (speedDisp != navCache.speedDisplay || gpsSpd != navCache.gpsSpeed || unitsChanged);
    bool logChanged     = (logLevel != navCache.logLevel);
    bool depthChanged   = (depthCm != navCache.depthCm || unitsChanged);

    // Redraw only what changed (opaque text background overwrites old content)
    if (statusChanged)  drawStatusBar(pkt);
    if (brgChanged)     drawBearing(pkt);
    if (rngChanged)     drawRange(pkt);
    if (hdgChanged)     drawHeading(pkt);
    if (spdChanged)     drawSpeed(pkt);
    if (logChanged)     drawLogIndicator(pkt);
    if (depthChanged)   drawDepth(pkt);

    // Update cache
    navCache.wifiDispFlags  = wifiDispFlags;
    navCache.gpsSignalBars  = pkt.gps_signal_bars;
    navCache.gpsSatellites  = pkt.gps_satellites;
    navCache.gpsHdopX10     = pkt.gps_hdop_x10;
    navCache.gpsAntenna     = pkt.gps_antenna;
    navCache.battLevel      = battLevel;
    navCache.hasHome       = hasHome;
    navCache.trueHeading   = trueHdg;
    navCache.bearingInt    = bearingInt;
    navCache.distCm        = distCm;
    navCache.headingInt    = headingInt;
    navCache.rawHeading    = gRawHeading;
    navCache.speedDisplay  = speedDisp;
    navCache.gpsSpeed      = gpsSpd;
    navCache.logLevel      = logLevel;
    navCache.depthCm       = depthCm;
    navCache.imperialUnits = gImperialUnits;
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
    uint8_t wifiDispFlags2 = (pkt.flags & FLAG_WIFI_ENABLED) | (pkt.flags2 & FLAG2_WIFI_CLIENT);
    uint8_t battLevel = (pkt.batt_mv == 0)              ? 0
                      : (pkt.batt_mv >= BATT_MV_GREEN)  ? 3
                      : (pkt.batt_mv >= BATT_MV_YELLOW) ? 2
                      : 1;

    if (wifiDispFlags2        != navCache.wifiDispFlags
     || pkt.gps_signal_bars   != navCache.gpsSignalBars
     || pkt.gps_satellites    != navCache.gpsSatellites
     || pkt.gps_hdop_x10      != navCache.gpsHdopX10
     || pkt.gps_antenna       != navCache.gpsAntenna
     || battLevel             != navCache.battLevel) {
        drawStatusBar(pkt);
        navCache.wifiDispFlags  = wifiDispFlags2;
        navCache.gpsSignalBars  = pkt.gps_signal_bars;
        navCache.gpsSatellites  = pkt.gps_satellites;
        navCache.gpsHdopX10     = pkt.gps_hdop_x10;
        navCache.gpsAntenna     = pkt.gps_antenna;
        navCache.battLevel      = battLevel;
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
//   y=28..136  6 status rows    label + dots + ok/FAIL, size 2
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
        { "IMU",       (bool)(boot_flags & BOOT_IMU_OK) },
        { "GPS",       (bool)(boot_flags & BOOT_GPS_OK) },
        { "Link ack",  (bool)(boot_flags & BOOT_DISPLAY_LINK_OK) },
        { "Mag cal",   (bool)(boot_flags & BOOT_MAG_CAL_OK) },
        { "Gyro cal",  (bool)(boot_flags & BOOT_GYRO_CAL_OK) },
        { "Accel cal", (bool)(boot_flags & BOOT_ACCEL_CAL_OK) },
        { "Depth",     (bool)(boot_flags & BOOT_DEPTH_OK) },
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

    tft.setTextSize(1);
    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(4, 210);
    tft.print("Hold BTN2 to force start");
}

// ---------------------------------------------------------------------------
// Manual countdown to force-start speed calibration.
// Layout (320×240):
//   y=  4  "SPEED CAL"          cyan, size 2
//   y= 40  "STARTING IN"        yellow, size 2
//   y= 80  countdown digit      white, size 5 (centered ~x=128)
// ---------------------------------------------------------------------------
void showSpeedCalCountdown(int secondsRemaining) {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 4);
    tft.print("SPEED CAL");

    tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
    tft.setCursor(4, 40);
    tft.print("STARTING IN");

    // Big centred countdown digit
    tft.setTextSize(5);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", secondsRemaining);
    // each char is 5*6=30px wide; centre in 320px
    int charW = 5 * 6;
    int x = (320 - (int)strlen(buf) * charW) / 2;
    tft.setCursor(x, 90);
    tft.print(buf);
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

// ---------------------------------------------------------------------------
// Fourier heading calibration: prompt screen
// Layout (320×240):
//   y=  0  "HDG CAL"                    cyan, size 2
//   y= 22  "Step N/12: Target 000°"     white, size 2
//   y= 44  separator line               cyan
//   y= 50  "Use phone compass:"         yellow, size 1
//   y= 60  "physically face 000°"       yellow, size 1
//   y= 74  "BTN2 when aimed & steady"   gray, size 1
//   y= 86  separator line               cyan
//   y= 92  "SENSOR (expected to differ):" gray, size 1
//   y=104  "XXX.X°"                     white, size 3
// ---------------------------------------------------------------------------
void showHdgFourierCalPrompt(int step, int total, float targetDeg, float indicatedDeg) {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(0, 0);
    tft.print("HDG CAL");

    char buf[48];
    snprintf(buf, sizeof(buf), "Step %d/%d: Tgt %03.0f" "\xF8", step + 1, total, targetDeg);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(0, 22);
    tft.print(buf);

    tft.drawFastHLine(0, 44, SCREEN_WIDTH, COLOR_CYAN);

    tft.setTextSize(1);
    tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
    tft.setCursor(0, 50);
    tft.print("Use phone compass:");
    snprintf(buf, sizeof(buf), "physically face %.0f" "\xF8", targetDeg);
    tft.setCursor(0, 60);
    tft.print(buf);

    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(0, 74);
    tft.print("BTN2 when aimed & steady");

    tft.drawFastHLine(0, 86, SCREEN_WIDTH, COLOR_CYAN);

    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(0, 92);
    tft.print("SENSOR (expected to differ):");

    tft.setTextSize(3);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    snprintf(buf, sizeof(buf), "%.1f" "\xF8", indicatedDeg);
    tft.setCursor(0, 104);
    tft.print(buf);
}

// ---------------------------------------------------------------------------
// Cloud calibration (docs/cloud-calibration-plan.md)
// ---------------------------------------------------------------------------

// Greedy word-wrap for short status text at text size 1. maxCharsPerLine
// should stay comfortably under SCREEN_WIDTH / 6 (the default GFX font is
// ~6px wide per char at size 1); no other screen in this file has needed
// multi-line body text before now, so this is new rather than reused.
static void printWrapped(int16_t x, int16_t y, const char* text, int maxCharsPerLine,
                          int16_t lineHeight, int maxLines) {
    if (!text) return;
    int len = (int)strlen(text);
    int pos = 0;
    for (int line = 0; line < maxLines && pos < len; line++) {
        int remaining = len - pos;
        int take = remaining < maxCharsPerLine ? remaining : maxCharsPerLine;
        if (take < remaining) {
            int lastSpace = -1;
            for (int i = take; i > 0; i--) {
                if (text[pos + i - 1] == ' ') { lastSpace = i - 1; break; }
            }
            if (lastSpace > 0) take = lastSpace;
        }
        char lineBuf[64];
        int n = take < (int)sizeof(lineBuf) - 1 ? take : (int)sizeof(lineBuf) - 1;
        memcpy(lineBuf, text + pos, n);
        lineBuf[n] = '\0';
        tft.setCursor(x, y + line * lineHeight);
        tft.print(lineBuf);
        pos += take;
        while (pos < len && text[pos] == ' ') pos++;  // skip the space we broke on
    }
}

void showCloudCalWaiting(uint32_t secondsWaiting) {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 4);
    tft.print("CLOUD CAL");
    tft.drawFastHLine(0, 28, SCREEN_WIDTH, COLOR_CYAN);

    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(4, 60);
    tft.print("Uploading...");

    tft.setTextSize(1);
    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(4, 92);
    tft.print("Please wait, this can take");
    tft.setCursor(4, 106);
    tft.print("up to 30 seconds.");

    char buf[16];
    snprintf(buf, sizeof(buf), "%lus", (unsigned long)secondsWaiting);
    tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
    tft.setCursor(4, 130);
    tft.print(buf);
}

void showCloudCalOffline() {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 4);
    tft.print("CLOUD CAL");
    tft.drawFastHLine(0, 28, SCREEN_WIDTH, COLOR_CYAN);

    tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
    tft.setCursor(4, 50);
    tft.print("No WiFi");

    tft.setTextSize(1);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(4, 84);
    tft.print("Connect to WiFi to finish");
    tft.setCursor(4, 98);
    tft.print("calibration in the cloud.");
    tft.setCursor(4, 114);
    tft.print("Raw samples are saved --");
    tft.setCursor(4, 128);
    tft.print("nothing is lost.");

    tft.drawFastHLine(0, 152, SCREEN_WIDTH, COLOR_CYAN);
    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(4, 160);
    tft.print("Press BTN2 to exit");
}

void showCloudCalFailed(const char* message) {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 4);
    tft.print("CLOUD CAL");
    tft.drawFastHLine(0, 28, SCREEN_WIDTH, COLOR_CYAN);

    tft.setTextColor(COLOR_RED, COLOR_BLACK);
    tft.setCursor(4, 50);
    tft.print("Upload failed");

    tft.setTextSize(1);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    printWrapped(4, 84, message ? message : "unknown error", 50, 12, 3);

    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(4, 132);
    tft.print("Raw samples are saved --");
    tft.setCursor(4, 146);
    tft.print("retry from tern.local.");

    tft.drawFastHLine(0, 168, SCREEN_WIDTH, COLOR_CYAN);
    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(4, 176);
    tft.print("Press BTN2 to exit");
}

void showCloudCalResult(uint8_t quality, float rmsPct, const char* recommendation,
                         int16_t coverageGaps, uint8_t choice, uint8_t calType) {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 4);
    tft.print("CLOUD CAL");

    uint16_t bandColor  = COLOR_GREEN;
    const char* bandLabel = "GOOD";
    if (quality == 1)      { bandColor = COLOR_YELLOW; bandLabel = "MARGINAL"; }
    else if (quality == 2) { bandColor = COLOR_RED;    bandLabel = "BAD"; }

    // hdg reuses this same numeric field for degrees of max Fourier-fit
    // residual, not percent of radius (heading-cal-cloud-plan.md) -- only the
    // label changes here, not the wire format.
    const bool isHdg = (calType == (uint8_t)CalType::HDG);
    char buf[28];
    if (isHdg) {
        snprintf(buf, sizeof(buf), "%s  %.1f" "\xF8" " max err", bandLabel, (double)rmsPct);
    } else {
        snprintf(buf, sizeof(buf), "%s  %.0f%% RMS", bandLabel, (double)rmsPct);
    }
    tft.setTextColor(bandColor, COLOR_BLACK);
    tft.setCursor(4, 30);
    tft.print(buf);

    tft.drawFastHLine(0, 54, SCREEN_WIDTH, COLOR_CYAN);

    // Recommendation text comes straight from the backend (already short —
    // see device-calibration-plan.md's `recommendation` field); wrap rather
    // than truncate since "recommend rejecting" is the important half.
    tft.setTextSize(1);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    printWrapped(4, 64, recommendation ? recommendation : "", 50, 12, 3);

    // Coverage-gap note (baseline-cal-coverage-feedback-plan.md, step 5): a
    // second, independent axis from RMS quality above -- a "good" fit can
    // still be missing coverage in specific orientations. Terse by design
    // (screen only has this one line of room before the accept/reject rule);
    // full detail lives on the website. Silent when not applicable (mounted,
    // pre-9-axis CSV) or when coverage is already full, rather than clutter
    // the screen with a "0 gaps" non-finding.
    if (coverageGaps > 0) {
        char covBuf[40];
        snprintf(covBuf, sizeof(covBuf), "%d orientation gap%s -- see website",
                  coverageGaps, coverageGaps == 1 ? "" : "s");
        tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
        tft.setCursor(4, 100);
        tft.print(covBuf);
    }

    tft.drawFastHLine(0, 112, SCREEN_WIDTH, COLOR_CYAN);

    static const char* const labels[2] = { "ACCEPT", "REJECT" };
    const int rowY[2] = { 122, 148 };
    for (int i = 0; i < 2; i++) {
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

    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(4, 200);
    tft.print("BTN1:cycle  BTN2:confirm");
}

// Gap-fill's upload result: informational only, no ACCEPT/REJECT. That fit
// covers only the cells just patched -- a handful out of 60 -- and its only
// useful destiny is as a merge candidate on the website. Offering to install
// it standalone (showCloudCalResult's ACCEPT/REJECT, defaulting to ACCEPT)
// would let a diver who presses BTN2 without reading closely silently
// overwrite a good full-sphere baseline with a partial one. See
// divemap/docs/architecture/baseline-cal-coverage-feedback-plan.md.
void showGapFillUploaded(float rmsPct, const char* recommendation, int16_t coverageGaps) {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 4);
    tft.print("GAP-FILL UPLOADED");

    char buf[28];
    snprintf(buf, sizeof(buf), "%.0f%% RMS (this patch)", (double)rmsPct);
    tft.setTextSize(1);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(4, 30);
    tft.print(buf);

    tft.drawFastHLine(0, 44, SCREEN_WIDTH, COLOR_CYAN);

    // Same recommendation text calibration-processor would give a solo
    // baseline -- shown as context, not as a verdict on this patch alone.
    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    printWrapped(4, 52, recommendation ? recommendation : "", 50, 10, 3);

    if (coverageGaps >= 0) {
        char covBuf[48];
        snprintf(covBuf, sizeof(covBuf), "%d cell%s remain thin/empty",
                  coverageGaps, coverageGaps == 1 ? "" : "s");
        tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
        tft.setCursor(4, 90);
        tft.print(covBuf);
    }

    tft.drawFastHLine(0, 104, SCREEN_WIDTH, COLOR_CYAN);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
    tft.setCursor(4, 116);
    tft.print("Go to the website");
    tft.setCursor(4, 138);
    tft.print("to merge & accept.");

    tft.setTextSize(1);
    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(4, 200);
    tft.print("BTN2: OK");
}

void showCloudLinkWaiting(const char* userCode, uint32_t secondsWaiting) {
    if (!tftReady) return;

    const char* code = userCode ? userCode : "";
    bool codeChanged  = strcmp(code, g_cloudLinkWaitingLastCode) != 0;

    // --- Static layout: draw only once per code (STARTING's "" and the real
    // code once CODE_READY arrives are different static content -- the code
    // appears and the instructions/waiting line join it). clear() resets
    // this flag so a fresh link attempt always starts with a full redraw.
    if (!g_cloudLinkWaitingStaticDrawn || codeChanged) {
        invalidateNavCache();
        tft.fillScreen(COLOR_BLACK);

        tft.setTextSize(2);
        tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
        tft.setCursor(4, 4);
        tft.print("LINK ACCOUNT");
        tft.drawFastHLine(0, 28, SCREEN_WIDTH, COLOR_CYAN);

        if (code[0] == '\0') {
            tft.setTextSize(2);
            tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
            tft.setCursor(4, 60);
            tft.print("Requesting code...");
        } else {
            // Bumped from size 1 -> 2: at size 1 this paragraph only used
            // about a third of the screen width, leaving the rest blank.
            tft.setTextSize(2);
            tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
            tft.setCursor(4, 34);
            tft.print("Log in to Dive Map and");
            tft.setCursor(4, 56);
            tft.print("select 'Link a device'");
            tft.setCursor(4, 78);
            tft.print("under My Devices");

            // Size-4 glyphs are 24px wide; center the 4-digit code on the 320px screen.
            tft.setTextSize(4);
            tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
            tft.setCursor((SCREEN_WIDTH - 4 * 24) / 2, 104);
            tft.print(code);

            tft.drawFastHLine(0, 144, SCREEN_WIDTH, COLOR_CYAN);
            tft.setTextSize(1);
            tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
            tft.setCursor(4, 152);
            tft.print("Waiting for approval...");
        }

        tft.setTextSize(1);
        tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
        tft.setCursor(4, 180);
        tft.print("Press BTN2 to cancel");

        strncpy(g_cloudLinkWaitingLastCode, code, sizeof(g_cloudLinkWaitingLastCode) - 1);
        g_cloudLinkWaitingLastCode[sizeof(g_cloudLinkWaitingLastCode) - 1] = '\0';
        g_cloudLinkWaitingStaticDrawn = true;
    }

    // --- Dynamic: elapsed-seconds counter only. Cleared first so a shorter
    // string (there isn't one in practice within a session, but be safe)
    // can't leave stray digits from the previous frame.
    char buf[16];
    snprintf(buf, sizeof(buf), "%lus", (unsigned long)secondsWaiting);
    tft.fillRect(4, 200, 60, 10, COLOR_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
    tft.setCursor(4, 200);
    tft.print(buf);
}

void showCloudLinkOffline() {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 4);
    tft.print("LINK ACCOUNT");
    tft.drawFastHLine(0, 28, SCREEN_WIDTH, COLOR_CYAN);

    tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
    tft.setCursor(4, 50);
    tft.print("No WiFi");

    tft.setTextSize(1);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(4, 84);
    tft.print("Connect to WiFi to link");
    tft.setCursor(4, 98);
    tft.print("this device to your account.");

    tft.drawFastHLine(0, 152, SCREEN_WIDTH, COLOR_CYAN);
    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(4, 160);
    tft.print("Press BTN2 to exit");
}

void showCloudLinkFailed(const char* message) {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 4);
    tft.print("LINK ACCOUNT");
    tft.drawFastHLine(0, 28, SCREEN_WIDTH, COLOR_CYAN);

    tft.setTextColor(COLOR_RED, COLOR_BLACK);
    tft.setCursor(4, 50);
    tft.print("Link failed");

    tft.setTextSize(1);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    printWrapped(4, 84, message ? message : "unknown error", 50, 12, 3);

    tft.drawFastHLine(0, 168, SCREEN_WIDTH, COLOR_CYAN);
    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(4, 176);
    tft.print("Press BTN2 to exit");
}

void showCloudLinkDone() {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 4);
    tft.print("LINK ACCOUNT");
    tft.drawFastHLine(0, 28, SCREEN_WIDTH, COLOR_CYAN);

    tft.setTextColor(COLOR_GREEN, COLOR_BLACK);
    tft.setCursor(4, 60);
    tft.print("Linked!");

    tft.setTextSize(1);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(4, 100);
    tft.print("This device is now linked");
    tft.setCursor(4, 114);
    tft.print("to your divemap account.");

    tft.drawFastHLine(0, 152, SCREEN_WIDTH, COLOR_CYAN);
    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(4, 160);
    tft.print("Press BTN2 to exit");
}

// ---------------------------------------------------------------------------
// Bin-coverage calibration grid — mounted cal only (COLLECT phase). Baseline
// no longer reaches this screen at all; see showBaselineRoughScan() below,
// which is baseline's only screen for its entire session.
// Layout (320×240):
//   y=0..19   — title row
//   y=20..35  — heading labels (N NE E SE S SW W NW N NE E SE)
//   y=36..195 — bin grid (elevation rows × heading columns, color-coded)
//   y=196..215 — elevation labels on left edge
//   y=216..239 — status row ("X/60 bins green" or "DONE — Saving CSV...")
// ---------------------------------------------------------------------------
void showCalGrid(const CalProgressPacket& pkt, const char* title) {
    const bool isMounted = (pkt.cal_type == (uint8_t)CalType::MOUNTED);
    // Gap-fill shares this screen deliberately rather than getting its own.
    // Its whole promise is that the cell it highlights is the cell the website
    // flagged, and the fastest way to break that is two grid renderers with
    // subtly different geometry. One function, one cell layout, one ordering.
    const bool isGapFill = (pkt.phase == (uint8_t)CalPhase::GAP_FILL);
    const int HDG_COLS   = 12;
    const int ELEV_ROWS  = isMounted ? 3 : 5;
    const int GRID_X     = 28;
    const int GRID_Y     = 36;
    const int COL_W      = 24;
    const int ROW_H      = isMounted ? 50 : 30;  // 3 rows→50px, 5 rows→30px
    const int GRID_H     = ROW_H * ELEV_ROWS;
    const int STATUS_Y   = GRID_Y + GRID_H + 8;

    // --- Static layout: draw only once per cal session to avoid full-screen flicker.
    // clear() resets g_calGridStaticDrawn so the next session redraws from scratch.
    if (!g_calGridStaticDrawn) {
        tft.fillScreen(COLOR_BLACK);

        // Title
        tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
        tft.setTextSize(2);
        tft.setCursor(4, 2);
        tft.print(title);

        // Heading labels: only the 4 columns that land exactly on a cardinal
        // heading are labeled (col*30° == 0/90/180/270 at cols 0/3/6/9). The
        // other 8 columns are 30°-wide bins that don't correspond to any
        // standard compass point, so they're left blank rather than mislabeled
        // (an 8-point compass rose doesn't divide evenly into 12 columns —
        // labeling all of them previously produced duplicate, incorrect names).
        // The white current-bin highlight provides orientation between cardinals.
        //
        // These labels are only as good as the live heading estimate behind
        // them. For baseline, that's COLLECT's rough post-ROUGH_SCAN bias —
        // better than raw-uncalibrated, but still approximate; see
        // docs/baseline-cal-two-pass.md.
        const char* hdgLabels[12] = { "N","","","E","","","S","","","W","","" };
        tft.setTextSize(1);
        tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
        for (int c = 0; c < HDG_COLS; c++) {
            tft.setCursor(GRID_X + c * COL_W + 2, 22);
            tft.print(hdgLabels[c]);
        }

        // Elevation labels (left column)
        const char* elevLabels5[5] = { "+60", "+30", "  0", "-30", "-60" };
        const char* elevLabels3[3] = { "+30", "  0", "-30" };
        const char** elevLabels = isMounted ? elevLabels3 : elevLabels5;
        tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
        for (int r = 0; r < ELEV_ROWS; r++) {
            tft.setCursor(0, GRID_Y + r * ROW_H + ROW_H / 2 - 4);
            tft.print(elevLabels[r]);
        }

        g_calGridStaticDrawn = true;
    }

    // --- Bin cells ---
    // Each cell is drawn with a FULL fillRect(cx, cy, COL_W, ROW_H) covering the
    // entire cell rectangle including its border pixels.  This means every frame
    // the previous white highlight border is automatically overwritten by the cell
    // color — no prev-bin tracking needed.  The current-bin white border is then
    // drawn on top after all cells are filled.
    for (int r = 0; r < ELEV_ROWS; r++) {
        for (int c = 0; c < HDG_COLS; c++) {
            int binIdx = r * HDG_COLS + c;
            uint8_t cnt = (binIdx < pkt.bins_total) ? pkt.bin_counts[binIdx] : 0;

            uint16_t color;
            uint8_t  doneAt;   // count at which this cell stops wanting samples
            if (isGapFill) {
                // Colour by the job, not by raw density. An untargeted cell is
                // not "empty and bad", it's "not your problem" -- painting it
                // red would send the diver chasing cells the server is happy
                // with, which is how the over-weighted bins this whole feature
                // exists to fix get made.
                const uint8_t st = pkt.has_targets ? pkt.targets[binIdx] : 0;
                const bool targeted = (st == 1 || st == 2);  // thin or empty
                doneAt = targeted ? MAG_CAL_GAPFILL_TARGET_CAP
                                  : MAG_CAL_GAPFILL_UNTARGETED_CAP;
                if (!targeted)              color = 0x1082;       // very dark gray
                else if (cnt >= doneAt)     color = COLOR_GREEN;
                else if (cnt > 0)           color = COLOR_YELLOW;
                else                        color = COLOR_RED;
            } else {
                doneAt = MAG_CAL_BIN_GREEN_THRESHOLD;
                if (cnt >= MAG_CAL_BIN_GREEN_THRESHOLD)       color = COLOR_GREEN;
                else if (cnt >= MAG_CAL_BIN_YELLOW_THRESHOLD) color = COLOR_YELLOW;
                else if (cnt > 0)                              color = COLOR_RED;
                else                                           color = 0x1082;  // very dark gray
            }

            int cx = GRID_X + c * COL_W;
            int cy = GRID_Y + r * ROW_H;
            tft.fillRect(cx, cy, COL_W, ROW_H, color);  // full cell — erases old border

            // Sample count in cells still wanting samples (drawn inside cell,
            // away from border). Suppressed on the dark untargeted cells in
            // gap-fill: a number there reads as a task, and it isn't one.
            if (isGapFill && color == 0x1082) {
                // nothing to draw
            } else if (cnt > 0 && cnt < doneAt) {
                tft.setTextSize(1);
                tft.setTextColor(COLOR_BLACK, color);
                tft.setCursor(cx + 4, cy + ROW_H / 2 - 4);
                tft.print(cnt);
            }
        }
    }

    // --- Current-orientation highlight: white border on active bin ---
    // Drawn after all cells so it's always on top.  The next frame's cell fillRect
    // for this bin will overwrite this border when/if the device moves.
    if (pkt.current_bin >= 0 && pkt.current_bin < pkt.bins_total) {
        int r = pkt.current_bin / HDG_COLS;
        int c = pkt.current_bin % HDG_COLS;
        tft.drawRect(GRID_X + c * COL_W, GRID_Y + r * ROW_H, COL_W, ROW_H, COLOR_WHITE);
    }

    // --- Status + orientation row (clear then redraw each frame) ---
    tft.fillRect(0, STATUS_Y - 2, 320, 46, COLOR_BLACK);  // clear both status lines

    if (pkt.complete) {
        tft.setTextSize(2);
        tft.setTextColor(COLOR_GREEN, COLOR_BLACK);
        tft.setCursor(4, STATUS_Y);
        tft.print("DONE - Saving CSV...");
    } else {
        // Line 1: progress. In gap-fill the denominator is the TARGET LIST, not
        // the grid -- pkt.bins_total is 60 there because it sizes the wire
        // array and the cell loop, so the display counts targets itself from
        // the map it already has. "3/48 bins" during a session whose whole job
        // is eleven cells would be actively misleading.
        tft.setTextSize(2);
        char sbuf[20];
        if (isGapFill) {
            int targetTotal = 0;
            if (pkt.has_targets) {
                for (int i = 0; i < pkt.bins_total && i < 60; i++) {
                    if (pkt.targets[i] == 1 || pkt.targets[i] == 2) targetTotal++;
                }
            }
            snprintf(sbuf, sizeof(sbuf), "%d/%d cells", pkt.bins_green, targetTotal);
        } else {
            // completion is row-weighted so may finish before all bins green
            snprintf(sbuf, sizeof(sbuf), "%d/%d bins", pkt.bins_green, pkt.bins_total);
        }
        tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
        tft.setCursor(4, STATUS_Y);
        tft.print(sbuf);

        // Line 2: fit quality when available, orientation otherwise.
        // Once fit_valid, the heading error estimate is more actionable than
        // the raw orientation readout (which the bin grid already shows visually).
        if (isGapFill) {
            // Orientation, always -- in gap-fill the diver is steering toward a
            // specific cell, so where they're pointing beats a fit statistic.
            // This pitch/heading pair comes from the same algebraic
            // reconstruction that chose the highlighted cell (see
            // util/mag_cal_orient.h), not from the AHRS, so the readout and the
            // highlight can never disagree.
            float hdg   = pkt.cur_hdg_deg;
            float pitch = pkt.cur_pitch_deg;
            if (hdg < 0.0f) hdg += 360.0f;
            if (hdg >= 360.0f) hdg -= 360.0f;
            static const char* gdirs[8] = { "N","NE","E","SE","S","SW","W","NW" };
            int gdidx = (int)((hdg + 22.5f) / 45.0f) % 8;
            char gbuf[14];
            snprintf(gbuf, sizeof(gbuf), "%s %+.0f\xB0", gdirs[gdidx], pitch);
            tft.setTextSize(2);
            tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
            tft.setCursor(4, STATUS_Y + 20);
            tft.print(gbuf);

            // --- Persistent roll widget: always-visible breakdown of the
            // roll-sector coverage for whichever cell current_bin is
            // pointing at (CalProgressPacket::current_bin_roll_counts/
            // _targeted, populated only in gap-fill -- see config.h's
            // "Roll coverage" block). A small square split into 4 triangles
            // by its diagonals -- top=upright, right=right-side,
            // bottom=upside-down, left=left-side, matching
            // mag_orient::rollSector()'s ordering -- each colored the same
            // green/yellow/red convention as a grid cell, with a white
            // outline on whichever triangle matches the device's LIVE roll
            // (current_roll_sector). Filled whole every frame, same
            // no-flicker trick the grid cells above use, so there's no
            // separate "last drawn sector" to track. Deliberately not a
            // mode swap with the main grid -- rejected in the gap-fill
            // orientation UX plan as visually confusing -- it just sits
            // here, always current, as the diver moves between cells.
            {
                const int WS  = 44;                // widget square side, px
                const int WX  = 320 - WS - 8;       // right-aligned, 8px margin
                const int WY  = STATUS_Y;
                const int cxm = WX + WS / 2;
                const int cym = WY + WS / 2;
                const uint16_t kUnknownColor = 0x1082;  // very dark gray, matches untargeted cells

                for (int k = 0; k < MAG_CAL_ROLL_SECTORS; k++) {
                    uint16_t color;
                    if (pkt.current_bin < 0) {
                        // No live orientation to report -- unknown, not a
                        // false "all satisfied" green.
                        color = kUnknownColor;
                    } else {
                        switch (pkt.current_bin_roll_targeted[k]) {
                            case 0:  color = COLOR_GREEN;  break;  // satisfied
                            case 1:  color = COLOR_YELLOW; break;  // some, under cap
                            default: color = COLOR_RED;    break;  // none yet
                        }
                    }
                    switch (k) {
                        case 0:  tft.fillTriangle(WX, WY, WX + WS, WY, cxm, cym, color); break;              // upright: top
                        case 1:  tft.fillTriangle(WX + WS, WY, WX + WS, WY + WS, cxm, cym, color); break;    // right-side: right
                        case 2:  tft.fillTriangle(WX + WS, WY + WS, WX, WY + WS, cxm, cym, color); break;    // upside-down: bottom
                        default: tft.fillTriangle(WX, WY + WS, WX, WY, cxm, cym, color); break;              // left-side: left
                    }
                }

                // Live-roll outline on the triangle matching the device's
                // current attitude -- skipped when roll is unmappable
                // (device pointing straight up/down, current_roll_sector
                // stays -1; see mag_orient::reconstructRoll).
                if (pkt.current_roll_sector >= 0 && pkt.current_roll_sector < MAG_CAL_ROLL_SECTORS) {
                    int ox0, oy0, ox1, oy1;
                    switch (pkt.current_roll_sector) {
                        case 0:  ox0 = WX;      oy0 = WY;      ox1 = WX + WS; oy1 = WY;      break;
                        case 1:  ox0 = WX + WS; oy0 = WY;      ox1 = WX + WS; oy1 = WY + WS; break;
                        case 2:  ox0 = WX + WS; oy0 = WY + WS; ox1 = WX;      oy1 = WY + WS; break;
                        default: ox0 = WX;      oy0 = WY + WS; ox1 = WX;      oy1 = WY;      break;
                    }
                    tft.drawLine(ox0, oy0, ox1, oy1, COLOR_WHITE);
                    tft.drawLine(ox0, oy0, cxm, cym, COLOR_WHITE);
                    tft.drawLine(ox1, oy1, cxm, cym, COLOR_WHITE);
                }
            }
        } else if (pkt.fit_valid) {
            // Colour-code by estimated heading error
            uint16_t fcol = (pkt.fit_hdg_err_deg < 3.0f) ? COLOR_GREEN
                          : (pkt.fit_hdg_err_deg < 7.0f) ? COLOR_YELLOW
                          : COLOR_RED;
            char fbuf[20];
            snprintf(fbuf, sizeof(fbuf), "Fit:\xB1%.1f\xB0", pkt.fit_hdg_err_deg);
            tft.setTextSize(2);
            tft.setTextColor(fcol, COLOR_BLACK);
            tft.setCursor(4, STATUS_Y + 20);
            tft.print(fbuf);
            // Stability suffix: blank = converging, "~" = nearly stable, "OK" = stable
            if (pkt.fit_delta < 0.5f) {
                tft.setTextColor(COLOR_GREEN, COLOR_BLACK);
                tft.print(" OK");
            } else if (pkt.fit_delta < 2.0f) {
                tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
                tft.print("  ~");
            }
        } else {
            // Orientation readout until enough samples for a valid fit.
            float hdg   = pkt.cur_hdg_deg;
            float pitch = pkt.cur_pitch_deg;
            if (hdg < 0.0f) hdg += 360.0f;
            if (hdg >= 360.0f) hdg -= 360.0f;
            static const char* dirs[8] = { "N","NE","E","SE","S","SW","W","NW" };
            int didx = (int)((hdg + 22.5f) / 45.0f) % 8;
            char obuf[14];
            snprintf(obuf, sizeof(obuf), "%s %+.0f\xB0", dirs[didx], pitch);
            tft.setTextSize(2);
            tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
            tft.setCursor(4, STATUS_Y + 20);
            tft.print(obuf);
        }
    }
}

// ---------------------------------------------------------------------------
// Baseline's only screen, for its entire collection session (see
// divemap/docs/architecture/baseline-cal-coverage-feedback-plan.md; the
// two-pass ROUGH_SCAN->COLLECT design docs/baseline-cal-two-pass.md
// describes is retired). No grid: AHRS heading isn't trustworthy yet
// (magnetometer uncalibrated), so there's nothing spatial worth promising.
// Just raw per-axis spread, sample count, and the live fit-quality readout
// (same incremental fitter showCalGrid uses — it doesn't depend on a bias
// estimate at all, so it's meaningful even here). The diver ends the session
// themselves (BTN2 -> FINISH_BASELINE_COLLECTION); the CSV uploads for real
// grading server-side.
// Layout (320×240):
//   y=0..19    — title row
//   y=22..30   — instruction line
//   y=40..112  — 3 axis-coverage bars (X/Y/Z)
//   y=125..145 — sample count
//   y=150..190 — live fit quality (reuses showCalGrid's color/format)
//   y=205..225 — "BTN2: done" prompt (dim + sample count needed, if under floor;
//                "uploading..." once complete)
// ---------------------------------------------------------------------------
void showBaselineRoughScan(const CalProgressPacket& pkt) {
    const int BAR_X = 40, BAR_W = 260, BAR_H = 16;

    if (!g_roughScanStaticDrawn) {
        tft.fillScreen(COLOR_BLACK);

        tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
        tft.setTextSize(2);
        tft.setCursor(4, 2);
        tft.print("BASELINE CAL");

        tft.setTextSize(1);
        tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
        tft.setCursor(4, 24);
        tft.print("Tumble through all orientations");

        const char* axisLabels[3] = { "X", "Y", "Z" };
        const int barY[3] = { 42, 68, 94 };
        tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
        for (int i = 0; i < 3; i++) {
            tft.setCursor(4, barY[i] + BAR_H / 2 - 4);
            tft.print(axisLabels[i]);
            tft.drawRect(BAR_X, barY[i], BAR_W, BAR_H, COLOR_GRAY);
        }

        g_roughScanStaticDrawn = true;
    }

    // --- Axis bar fills (redrawn each call) ---
    const int barY[3] = { 42, 68, 94 };
    const uint8_t cov[3] = { pkt.cov_x, pkt.cov_y, pkt.cov_z };
    for (int i = 0; i < 3; i++) {
        int fillW = (BAR_W - 2) * (int)cov[i] / 100;
        uint16_t color = (cov[i] >= 80) ? COLOR_GREEN : (cov[i] >= 40) ? COLOR_YELLOW : COLOR_RED;
        tft.fillRect(BAR_X + 1, barY[i] + 1, BAR_W - 2, BAR_H - 2, COLOR_BLACK);
        if (fillW > 0) tft.fillRect(BAR_X + 1, barY[i] + 1, fillW, BAR_H - 2, color);
    }

    // --- Sample count ---
    tft.fillRect(0, 122, 320, 20, COLOR_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(4, 124);
    char sbuf[24];
    snprintf(sbuf, sizeof(sbuf), "Samples: %u", (unsigned)pkt.sample_count);
    tft.print(sbuf);

    // --- Live fit quality (same reading as COLLECT's grid screen) ---
    tft.fillRect(0, 148, 320, 20, COLOR_BLACK);
    if (pkt.fit_valid) {
        uint16_t fcol = (pkt.fit_hdg_err_deg < 3.0f) ? COLOR_GREEN
                      : (pkt.fit_hdg_err_deg < 7.0f) ? COLOR_YELLOW
                      : COLOR_RED;
        char fbuf[20];
        snprintf(fbuf, sizeof(fbuf), "Fit:\xB1%.1f\xB0", pkt.fit_hdg_err_deg);
        tft.setTextSize(2);
        tft.setTextColor(fcol, COLOR_BLACK);
        tft.setCursor(4, 150);
        tft.print(fbuf);
        if (pkt.fit_delta < 0.5f) {
            tft.setTextColor(COLOR_GREEN, COLOR_BLACK);
            tft.print(" OK");
        } else if (pkt.fit_delta < 2.0f) {
            tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
            tft.print("  ~");
        }
    }

    // --- Prompt ---
    tft.fillRect(0, 205, 320, 30, COLOR_BLACK);
    tft.setTextSize(1);
    if (pkt.complete) {
        tft.setTextColor(COLOR_GREEN, COLOR_BLACK);
        tft.setCursor(4, 210);
        if (pkt.max_samples_reached) {
            tft.print("Max samples reached,");
            tft.setCursor(4, 222);
            tft.print("uploading now.");
        } else {
            tft.print("Done -- uploading for grading...");
        }
    } else if (pkt.sample_count >= MAG_CAL_ROUGH_SCAN_MIN_SAMPLES) {
        tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
        tft.setCursor(4, 210);
        tft.print("BTN2: done, upload for grading");
    } else {
        tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
        tft.setCursor(4, 210);
        char pbuf[40];
        snprintf(pbuf, sizeof(pbuf), "Keep going... (%d more samples)",
                 MAG_CAL_ROUGH_SCAN_MIN_SAMPLES - (int)pkt.sample_count);
        tft.print(pbuf);
    }
}

// ---------------------------------------------------------------------------
// Shown the instant BTN2 requests FINISH_BASELINE_COLLECTION, before nav has
// confirmed. nav does a synchronous CSV dump (can take real time for a big
// run) before it even sends the completion packet, so without this screen
// the diver sees the exact same rough-scan bars/prompt they were already
// looking at -- indistinguishable from the press not having registered.
// ---------------------------------------------------------------------------
void showBaselineFinishing() {
    tft.fillScreen(COLOR_BLACK);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setTextSize(2);
    tft.setCursor(4, 2);
    tft.print("BASELINE CAL");

    tft.setTextSize(2);
    tft.setTextColor(COLOR_GREEN, COLOR_BLACK);
    tft.setCursor(20, 110);
    tft.print("Finishing up...");

    tft.setTextSize(1);
    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(20, 140);
    tft.print("Uploading and processing CSV");
}

}  // namespace display
