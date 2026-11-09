#include "display.h"
#include "logo_bitmap.h"
#include "../board_pins.h"
#include "../config.h"
#include "../version.h"
#include <dpvlink.h>
#include "../util/mounted_dwell.h"
#include "../util/level_gate.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <driver/rtc_io.h>
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

// Cloud-link waiting screen: the static
// header/instructions/code are drawn once, and each subsequent call (this
// screen is redrawn at 4 Hz while the elapsed-seconds counter ticks) only
// repaints the small counter region. Reset by clear() so each new session
// starts fresh; also forced by a userCode change (STARTING's "" -> the real
// code once CODE_READY arrives changes the static content itself).
static bool g_cloudLinkWaitingStaticDrawn = false;
static char g_cloudLinkWaitingLastCode[16] = "";

// The cal frame's per-band caches (header, status, keys, work area); see
// "The calibration frame" below. Reset by clear() and reinit().
namespace display { static void resetCalFrame(); }

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

// --- Backlight --------------------------------------------------------------
// TFT_BL drives the EyeSPI "Lite" pin, which the breakout pulls high -- so the
// backlight is lit by default and stays lit unless something drives this low.
// That is the whole reason a "powered off" unit used to glow: the panel was
// blanked and the ESP32 was in deep sleep at ~10 uA, next to a backlight still
// drawing ~25 mA, which is essentially the entire power-off budget.
//
// GPIO4 is RTC-capable, which is what makes the off state survivable: an
// ordinary pad goes high-Z in deep sleep, the breakout's pull-up wins, and the
// light comes straight back on. The RTC pad latch below holds the level with no
// core running. Deliberately NOT gpio_deep_sleep_hold_en() -- that latches every
// digital pad, including LINK_TX, which has to carry the wake byte to the nav
// device on the way back up.
void setBacklight(bool on) {
    rtc_gpio_hold_dis(static_cast<gpio_num_t>(TFT_BL));
    rtc_gpio_deinit(static_cast<gpio_num_t>(TFT_BL));
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, on ? HIGH : LOW);
}

void sleepForPowerOff() {
    // Panel controller: blank, then sleep-in (~10 uA). Skipped when init() has
    // not run -- the unconfirmed-wake path in setup() reaches here before the
    // SPI bus exists, and talking to the ST7789 then would hang.
    if (tftReady) {
        tft.enableDisplay(false);
        tft.enableSleep(true);
        tftReady = false;     // later draws become no-ops until init() runs again
    }
    // Backlight off, latched off through deep sleep. Nothing releases this
    // latch except setBacklight(), which init() calls -- so a wake that is not
    // confirmed by the both-buttons hold never lights the screen at all.
    rtc_gpio_init(static_cast<gpio_num_t>(TFT_BL));
    rtc_gpio_set_direction(static_cast<gpio_num_t>(TFT_BL), RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(static_cast<gpio_num_t>(TFT_BL), 0);
    rtc_gpio_hold_en(static_cast<gpio_num_t>(TFT_BL));
    Serial.println("[DISP] panel asleep, backlight off and latched");
}

bool init() {
    // Backlight on first: this also releases the deep-sleep latch left by
    // sleepForPowerOff(), without which the screen would come up dark.
    setBacklight(true);

    // Hardware reset pulse. TFT_RST reaches the panel as of 2026-09-18 (it was
    // pointed at an unrouted GPIO before), so this is now a real reset and not
    // just a wiggle on a floating pin.
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
    g_cloudLinkWaitingStaticDrawn = false;
    g_cloudLinkWaitingLastCode[0] = '\0';
    resetCalFrame();   // every cal screen redraws in full
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
    // The display is blank after reinit; force a full redraw.
    g_cloudLinkWaitingStaticDrawn = false;
    g_cloudLinkWaitingLastCode[0] = '\0';
    resetCalFrame();
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
        // Wire values (logging.h): 1=LOW, 2=MID, 3=HIGH. L0/L1/L2 keep the
        // meanings divers already know, so HIGH is still "L2".
        case 1:  color = COLOR_YELLOW; label = "L1"; break;
        case 2:  color = COLOR_YELLOW; label = "LM"; break;
        case 3:  color = COLOR_RED;    label = "L2"; break;
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
void showBootStatus(uint8_t boot_flags, const char* nav_fw, const char* update_version) {
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

    // Firmware footer. The two boards are released as a pair, so one line is
    // the normal case; a split means an OTA updated one board and not the other.
    y += 6;
    tft.drawFastHLine(0, y, SCREEN_WIDTH, COLOR_GRAY);
    y += 8;
    tft.setTextSize(2);
    bool navKnown = (nav_fw != nullptr && nav_fw[0] != '\0');
    if (navKnown && strcmp(nav_fw, FW_VERSION) == 0) {
        tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
        tft.setCursor(0, y);
        tft.printf("fw %s", FW_VERSION);
    } else {
        tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
        tft.setCursor(0, y);
        tft.printf("nav  %s", navKnown ? nav_fw : "?");
        tft.setCursor(0, y + 18);
        tft.printf("disp %s", FW_VERSION);
    }

    if (update_version && update_version[0]) {
        tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
        tft.setCursor(0, 222);
        tft.printf("Update %s: tern.local", update_version);
    }
}

// ===========================================================================
// Firmware update screens
// ===========================================================================
static void printWrapped(int16_t x, int16_t y, const char* text, int maxCharsPerLine,
                         int16_t lineHeight, int maxLines);  // defined with the cloud-cal screens

static constexpr int FW_BAR_Y = 130;
static constexpr int FW_BAR_H = 16;

void showFirmwareUpdate(const char* version, int pct, const char* message, bool error, bool full) {
    if (!tftReady) return;
    if (full) {
        invalidateNavCache();
        tft.fillScreen(COLOR_BLACK);

        tft.setTextSize(2);
        tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
        tft.setCursor(4, 4);
        tft.print("FIRMWARE UPDATE");
        tft.drawFastHLine(0, 28, SCREEN_WIDTH, COLOR_CYAN);

        tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
        tft.setCursor(4, 44);
        tft.printf("Display %s", FW_VERSION);
        tft.setCursor(4, 64);
        tft.printf("     -> %s", version ? version : "?");

        tft.setTextSize(1);
        tft.setTextColor(error ? COLOR_RED : COLOR_WHITE, COLOR_BLACK);
        printWrapped(4, 164, message ? message : "", 50, 12, 3);

        tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
        tft.setCursor(4, 222);
        tft.print(error ? "Still running the old firmware."
                        : "Keep the unit powered. Buttons are off.");
        if (pct >= 0) tft.drawRect(4, FW_BAR_Y, SCREEN_WIDTH - 8, FW_BAR_H, COLOR_GRAY);
    }
    if (pct < 0) return;
    if (pct > 100) pct = 100;

    int inner = SCREEN_WIDTH - 10;
    int fill  = inner * pct / 100;
    tft.fillRect(5, FW_BAR_Y + 1, fill, FW_BAR_H - 2, COLOR_CYAN);
    tft.fillRect(5 + fill, FW_BAR_Y + 1, inner - fill, FW_BAR_H - 2, COLOR_BLACK);

    tft.setTextSize(3);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(4, 96);
    tft.printf("%3d%%", pct);
}

void showUpdateAvailable(const char* version) {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 4);
    tft.print("FIRMWARE");
    tft.drawFastHLine(0, 28, SCREEN_WIDTH, COLOR_CYAN);

    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(4, 70);
    tft.printf("Version %s", version ? version : "?");
    tft.setCursor(4, 94);
    tft.print("is available.");

    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(4, 140);
    tft.print("Install it from");
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 164);
    tft.print("tern.local");
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
// Current hold — "point upstream and stop swimming for 60 s"
//
// The running screen shows the live flow reading beside the clock on purpose.
// Station-keeping against a current is hard, and a diver who is slipping needs
// to watch the number wander while there is still time to fix it, rather than
// find out afterwards that they measured their own swimming.
// ---------------------------------------------------------------------------
void showCurrentHoldCountdown(int secondsRemaining) {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 4);
    tft.print("CURRENT");

    tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
    tft.setCursor(4, 36);
    tft.print("POINT UPSTREAM");
    tft.setCursor(4, 60);
    tft.print("HOLD STATION");

    tft.setTextSize(5);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", secondsRemaining);
    int charW = 5 * 6;
    int x = (320 - (int)strlen(buf) * charW) / 2;
    tft.setCursor(x, 110);
    tft.print(buf);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(4, 210);
    tft.print("BTN1=cancel");
}

void showCurrentHoldRunning(uint8_t remaining_s, float heading_deg, char hdg_suffix, float flow_ms) {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 4);
    tft.print("CURRENT");

    tft.setTextColor(COLOR_GREEN, COLOR_BLACK);
    tft.setCursor(4, 32);
    tft.print("HOLDING");

    // The clock is the thing to read at a glance: size 8 (48x64 px per char).
    tft.setTextSize(8);
    tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", (unsigned)remaining_s);
    int charW = 8 * 6;
    int x = (320 - (int)strlen(buf) * charW) / 2;
    tft.setCursor(x, 64);
    tft.print(buf);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(4, 160);
    tft.printf("Flow %.2f m/s", (double)flow_ms);
    tft.setCursor(4, 184);
    tft.printf("Hdg  %03d%c", (int)(heading_deg + 0.5f) % 360, hdg_suffix);

    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(4, 212);
    tft.print("BTN1=finish now");
}

void showCurrentHoldResult(float current_ms, float toward_deg, char toward_suffix, uint8_t averaged_s) {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 4);
    tft.print("CURRENT");

    if (averaged_s == 0) {
        tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
        tft.setCursor(4, 60);
        tft.print("Not held long");
        tft.setCursor(4, 84);
        tft.print("enough to measure.");
        tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
        tft.setCursor(4, 180);
        tft.print("Nothing saved.");
        tft.setCursor(4, 210);
        tft.print("Any btn=done");
        return;
    }

    tft.setTextSize(3);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(4, 48);
    tft.printf("%.2f m/s", (double)current_ms);
    tft.setCursor(4, 88);
    tft.printf("%.1f m/min", (double)(current_ms * 60.0f));

    tft.setTextSize(2);
    tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
    tft.setCursor(4, 140);
    tft.printf("Toward %03d%c", (int)(toward_deg + 0.5f) % 360, toward_suffix);

    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(4, 180);
    tft.printf("Avg of %us. Saved.", (unsigned)averaged_s);
    tft.setCursor(4, 210);
    tft.print("Any btn=done");
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
// The calibration frame (docs/cal-screens-unified-plan.md)
//
// Every cal screen -- each step's collecting screen and the shared upload,
// result and ending screens -- is drawn in one frame:
//
//   y   0-23   header: step strip 1 1a 2 3 (current step on a cyan box), the
//              step's name, a counter right-aligned; cyan rule at y 23
//   y  24-183  work area: the only band that differs between screens
//   y 186-209  status: one instruction at size 2; dim rule at y 184
//   y 212-239  softkeys: BTN1 left, BTN2 right
//
// Each band keeps its own last-drawn cache, all reset by clear() and reinit(),
// so a screen calls the helpers on every render and nothing is repainted
// unless it changed. The cal screens render at up to 10 Hz, and anything
// repainted that often flickers.
// ---------------------------------------------------------------------------
static constexpr int CAL_WORK_Y   = 24;
static constexpr int CAL_WORK_H   = 160;      // y 24-183
static constexpr int CAL_STATUS_Y = 190;
static constexpr int CAL_KEY_Y    = 212;
static constexpr int CAL_KEY_W    = 156;
static constexpr int CAL_KEY_H    = 26;
static constexpr int CAL_KEY_R    = 5;
static constexpr int CAL_KEY_X[2] = { 2, 162 };   // BTN1 is physically on the left
static constexpr uint16_t COLOR_STEP_TEXT = 0x52AA;  // the steps not being worked
static constexpr uint16_t COLOR_DIM_TEXT  = 0x4208;  // a key that can't act yet
static constexpr uint16_t COLOR_OPEN_CELL = 0x1082;  // a grid cell nothing is asked of

enum class KeyStyle : uint8_t {
    BLANK,   // dim outline, no label: the button does nothing here
    DIM,     // dim label: the action isn't available yet
    NORMAL,  // gray outline, white label
    GO,      // green fill: MARK once the level gate is ready
    HOLD,    // gray label; fills red while BTN1 is held (QUIT, REJECT)
};
struct CalKey  { const char* label; KeyStyle style; };
struct CalText { const char* text; uint16_t color; };

static const CalKey KEY_BLANK  = { "", KeyStyle::BLANK };
static const CalKey KEY_QUIT   = { "hold: QUIT", KeyStyle::HOLD };
static const CalKey KEY_REJECT = { "hold: REJECT", KeyStyle::HOLD };
static const CalKey KEY_OK     = { "OK", KeyStyle::NORMAL };

static int textW(const char* s, int size) { return (int)strlen(s) * 6 * size; }

// --- Softkey overlay, pushed in by display_main before each render ----------
static uint8_t g_calHoldPct = 0;
static bool    g_calHint    = false;

void setCalKeysOverlay(uint8_t holdPct, bool hint) {
    g_calHoldPct = holdPct > 100 ? 100 : holdPct;
    g_calHint    = hint;
}

// --- Header -----------------------------------------------------------------
static int8_t g_calHdrStep = -1;
static char   g_calHdrCounter[12] = "";
static int    g_calHdrCounterW = 0;

static const char* const kCalStepIds[4]   = { "1", "1a", "2", "3" };
static const char* const kCalStepNames[4] = { "BASELINE", "FILL GAPS", "MOUNTED", "HEADING" };

// counter counts toward whatever ends the step, or names the phase on the
// closing screens; "" draws none.
static void drawCalHeader(CalStep step, const char* counter) {
    const int s = (int)step;
    if (s != g_calHdrStep) {
        g_calHdrStep       = (int8_t)s;
        g_calHdrCounter[0] = '\0';
        g_calHdrCounterW   = 0;
        invalidateNavCache();
        tft.fillRect(0, 0, SCREEN_WIDTH, 23, COLOR_BLACK);
        tft.setTextSize(2);
        int x = 2;
        for (int i = 0; i < 4; i++) {
            const int w = textW(kCalStepIds[i], 2) + 6;
            if (i == s) {
                tft.fillRoundRect(x, 1, w, 20, 3, COLOR_CYAN);
                tft.setTextColor(COLOR_BLACK, COLOR_CYAN);
            } else {
                tft.setTextColor(COLOR_STEP_TEXT, COLOR_BLACK);
            }
            tft.setCursor(x + 3, 3);
            tft.print(kCalStepIds[i]);
            x += w + 5;
        }
        tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
        tft.setCursor(x + 1, 3);
        tft.print(kCalStepNames[s]);
        tft.drawFastHLine(0, 23, SCREEN_WIDTH, COLOR_CYAN);
    }
    if (strcmp(counter, g_calHdrCounter) != 0) {
        const int w = textW(counter, 2);
        if (g_calHdrCounterW > w) {
            tft.fillRect(318 - g_calHdrCounterW, 3, g_calHdrCounterW - w, 16, COLOR_BLACK);
        }
        tft.setTextSize(2);
        tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
        tft.setCursor(318 - w, 3);
        tft.print(counter);
        strncpy(g_calHdrCounter, counter, sizeof(g_calHdrCounter) - 1);
        g_calHdrCounter[sizeof(g_calHdrCounter) - 1] = '\0';
        g_calHdrCounterW = w;
    }
}

// --- Status line --------------------------------------------------------------
static char g_calStatusSig[96] = "";   // the parts as last drawn, colours included
static int  g_calStatusW = 0;

// Parts are drawn left to right, each in its own colour ("Next: " gray, then
// what's next in white). The line holds 26 characters.
static void drawCalStatus(const CalText* parts, int n) {
    char sig[sizeof(g_calStatusSig)];
    int k = 0;
    sig[0] = '\0';
    for (int i = 0; i < n && k < (int)sizeof(sig); i++) {
        k += snprintf(sig + k, sizeof(sig) - k, "%04x%s|", parts[i].color, parts[i].text);
    }
    if (strcmp(sig, g_calStatusSig) == 0) return;
    strncpy(g_calStatusSig, sig, sizeof(g_calStatusSig) - 1);
    g_calStatusSig[sizeof(g_calStatusSig) - 1] = '\0';

    tft.drawFastHLine(0, 184, SCREEN_WIDTH, COLOR_DIM_GRAY);
    tft.setTextSize(2);
    int x = 4;
    for (int i = 0; i < n; i++) {
        tft.setTextColor(parts[i].color, COLOR_BLACK);
        tft.setCursor(x, CAL_STATUS_Y);
        tft.print(parts[i].text);
        x += textW(parts[i].text, 2);
    }
    if (g_calStatusW > x) tft.fillRect(x, CAL_STATUS_Y, g_calStatusW - x, 16, COLOR_BLACK);
    g_calStatusW = x;
}

// --- Softkeys -----------------------------------------------------------------
struct CalKeyCache { char label[16]; int8_t style; int fillPx; };
static CalKeyCache g_calKeyCache[2] = { { "", -1, 0 }, { "", -1, 0 } };

// Columns [from, to) of the rounded key shape, for the hold fill. Same corner
// test as the key's outline, so the fill stays inside it.
static void fillKeyColumns(int x, int from, int to, uint16_t color) {
    for (int i = from; i < to; i++) {
        const int ccx = i < CAL_KEY_R ? CAL_KEY_R
                      : i > CAL_KEY_W - 1 - CAL_KEY_R ? CAL_KEY_W - 1 - CAL_KEY_R : i;
        const int dx = i - ccx;
        const int inset = dx ? CAL_KEY_R - (int)sqrtf((float)(CAL_KEY_R * CAL_KEY_R - dx * dx)) : 0;
        tft.drawFastVLine(x + i, CAL_KEY_Y + inset, CAL_KEY_H - 2 * inset, color);
    }
}

static void drawCalKey(int side, const CalKey& k, uint8_t holdPct) {
    CalKeyCache& c = g_calKeyCache[side];
    const int x = CAL_KEY_X[side];
    const int fillPx = (k.style == KeyStyle::HOLD) ? (int)holdPct * CAL_KEY_W / 100 : 0;
    const bool sameKey = c.style == (int8_t)k.style && strcmp(c.label, k.label) == 0;
    if (sameKey && fillPx == c.fillPx) return;

    const int lx = x + (CAL_KEY_W - textW(k.label, 2)) / 2;
    const int ly = CAL_KEY_Y + 6;
    tft.setTextSize(2);
    if (sameKey && fillPx > c.fillPx) {
        // The hold is growing: paint only the new columns, then the label back
        // over them. Transparent, because part of it sits on red.
        fillKeyColumns(x, c.fillPx, fillPx, COLOR_RED);
        tft.setTextColor(COLOR_WHITE);
        tft.setCursor(lx, ly);
        tft.print(k.label);
        c.fillPx = fillPx;
        return;
    }

    // A different key, or a hold let go: repaint the whole key.
    uint16_t outline = COLOR_GRAY, ink = COLOR_WHITE, bg = COLOR_BLACK;
    switch (k.style) {
        case KeyStyle::BLANK: outline = COLOR_DIM_GRAY; break;
        case KeyStyle::DIM:   outline = COLOR_DIM_GRAY; ink = COLOR_DIM_TEXT; break;
        case KeyStyle::GO:    outline = COLOR_GREEN; ink = COLOR_BLACK; bg = COLOR_GREEN; break;
        case KeyStyle::HOLD:  ink = fillPx > 0 ? COLOR_WHITE : COLOR_GRAY; break;
        default:              break;
    }
    tft.fillRoundRect(x, CAL_KEY_Y, CAL_KEY_W, CAL_KEY_H, CAL_KEY_R, bg);
    tft.drawRoundRect(x, CAL_KEY_Y, CAL_KEY_W, CAL_KEY_H, CAL_KEY_R, outline);
    if (fillPx > 0) fillKeyColumns(x, 0, fillPx, COLOR_RED);
    if (k.style != KeyStyle::BLANK) {
        if (fillPx > 0) tft.setTextColor(ink);
        else            tft.setTextColor(ink, bg);
        tft.setCursor(lx, ly);
        tft.print(k.label);
    }
    strncpy(c.label, k.label, sizeof(c.label) - 1);
    c.label[sizeof(c.label) - 1] = '\0';
    c.style  = (int8_t)k.style;
    c.fillPx = fillPx;
}

// Status line and both keys. A short BTN1 press on a screen whose left key is
// a hold puts the hint in the status line for a moment (display_main times it).
static void drawCalFooter(const CalText* parts, int n, const CalKey& left, const CalKey& right) {
    if (g_calHint && left.style == KeyStyle::HOLD) {
        const CalText hint = { strstr(left.label, "REJECT") ? "Hold BTN1 to reject"
                                                            : "Hold BTN1 to quit",
                               COLOR_WHITE };
        drawCalStatus(&hint, 1);
    } else {
        drawCalStatus(parts, n);
    }
    drawCalKey(0, left, g_calHoldPct);
    drawCalKey(1, right, 0);
}

// --- Work area ----------------------------------------------------------------
// The work area belongs to one screen at a time. A screen passes a signature
// of its static content; a different one clears the band and the screen draws
// that content again. This is also what keeps the endings from repainting the
// whole screen on every render, which they used to do at 4 Hz.
static uint32_t g_calWorkSig = 0;   // 0 = nothing drawn

enum class CalWork : uint8_t {
    BARS = 1, GRID, MOUNTED, HDG_PROMPT, HDG_MARKED,
    UPLOAD, RESULT, SENT, OFFLINE, FAILED, DISCARDED, INSTALLED,
};

static uint32_t calSig(uint32_t h, const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
static uint32_t calSig(CalWork id) { return calSig(2166136261u, &id, sizeof(id)); }
static uint32_t calSigStr(uint32_t h, const char* s) { return calSig(h, s, s ? strlen(s) : 0); }

// True when the band was just cleared for `sig`, i.e. the caller should draw
// its static content now.
static bool claimCalWork(uint32_t sig) {
    if (sig == 0) sig = 1;
    if (sig == g_calWorkSig) return false;
    g_calWorkSig = sig;
    tft.fillRect(0, CAL_WORK_Y, SCREEN_WIDTH, CAL_WORK_H, COLOR_BLACK);
    return true;
}

static void resetCalFrame() {
    g_calHdrStep       = -1;
    g_calHdrCounter[0] = '\0';
    g_calHdrCounterW   = 0;
    g_calStatusSig[0]  = '\0';
    g_calStatusW       = 0;
    for (auto& k : g_calKeyCache) { k.label[0] = '\0'; k.style = -1; k.fillPx = 0; }
    g_calWorkSig = 0;
}

static void drawWorkText(int x, int y, const char* s, uint8_t size, uint16_t color) {
    tft.setTextSize(size);
    tft.setTextColor(color, COLOR_BLACK);
    tft.setCursor(x, y);
    tft.print(s);
}

// --- Bars -----------------------------------------------------------------------
// A gray-outlined bar filled from the left, with an optional label inside it.
// Repaints only the columns that changed, then the label over them
// (transparent, since part of it can sit on the fill); a new colour or label
// repaints it whole. px counts interior columns (w - 2 at full).
struct CalBarCache { int px = -1; uint16_t color = 0; char label[10] = ""; uint16_t labelColor = 0; };

static void drawCalBar(CalBarCache& c, int x, int y, int w, int h, int px, uint16_t color,
                       const char* label = "", uint16_t labelColor = COLOR_WHITE) {
    const int inner = w - 2;
    if (px < 0) px = 0;
    if (px > inner) px = inner;
    const bool whole = c.px < 0 || color != c.color || labelColor != c.labelColor ||
                       strcmp(label, c.label) != 0;
    if (!whole && px == c.px) return;
    if (whole) {
        if (c.px < 0) tft.drawRect(x, y, w, h, COLOR_GRAY);
        if (px > 0) tft.fillRect(x + 1, y + 1, px, h - 2, color);
        tft.fillRect(x + 1 + px, y + 1, inner - px, h - 2, COLOR_BLACK);
    } else if (px > c.px) {
        tft.fillRect(x + 1 + c.px, y + 1, px - c.px, h - 2, color);
    } else {
        tft.fillRect(x + 1 + px, y + 1, c.px - px, h - 2, COLOR_BLACK);
    }
    if (label[0]) {
        tft.setTextSize(2);
        tft.setTextColor(labelColor);
        tft.setCursor(x + 6, y + (h - 16) / 2);
        tft.print(label);
    }
    c.px = px;
    c.color = color;
    c.labelColor = labelColor;
    strncpy(c.label, label, sizeof(c.label) - 1);
    c.label[sizeof(c.label) - 1] = '\0';
}

// --- Grids (Baseline, Fill gaps, Mounted) ---------------------------------------
// 12 heading columns of 24 px from x 26; rows of CAL_GRID_ROW_H (Baseline and
// Fill gaps) or MC_ROW_H (Mounted). Each cell keeps a 1 px black gutter, so
// the current cell's 2 px border and the position dot can be painted over it
// and wiped by repainting the cells.
static constexpr int CAL_GRID_X     = 26;
static constexpr int CAL_GRID_Y     = 36;
static constexpr int CAL_GRID_COL_W = 24;
static constexpr int CAL_GRID_ROW_H = 24;

// N, E, S and W over the four columns that start on them (the other eight
// are 30 deg bins no compass point names), and the row labels on the left.
static void drawGridTicks(const char* const* rowLabels, int rows, int rowH) {
    tft.setTextSize(1);
    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    const char* const cardinals[4] = { "N", "E", "S", "W" };
    for (int k = 0; k < 4; k++) {
        tft.setCursor(CAL_GRID_X + k * 3 * CAL_GRID_COL_W + 10, 27);
        tft.print(cardinals[k]);
    }
    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    for (int r = 0; r < rows; r++) {
        tft.setCursor(2, CAL_GRID_Y + r * rowH + rowH / 2 - 4);
        tft.print(rowLabels[r]);
    }
}

// One cell, gutter included. count > 0 prints it in the cell (size 1).
static void drawGridCell(int bin, int rowH, uint16_t color, int count) {
    const int cx = CAL_GRID_X + (bin % 12) * CAL_GRID_COL_W;
    const int cy = CAL_GRID_Y + (bin / 12) * rowH;
    tft.drawRect(cx, cy, CAL_GRID_COL_W, rowH, COLOR_BLACK);
    tft.fillRect(cx + 1, cy + 1, CAL_GRID_COL_W - 2, rowH - 2, color);
    if (count > 0) {
        tft.setTextSize(1);
        tft.setTextColor(COLOR_BLACK, color);
        tft.setCursor(cx + 4, cy + rowH / 2 - 4);
        tft.print(count);
    }
}

static void drawGridHighlight(int bin, int rowH, uint16_t color) {
    const int cx = CAL_GRID_X + (bin % 12) * CAL_GRID_COL_W;
    const int cy = CAL_GRID_Y + (bin / 12) * rowH;
    tft.drawRect(cx, cy, CAL_GRID_COL_W, rowH, color);
    tft.drawRect(cx + 1, cy + 1, CAL_GRID_COL_W - 2, rowH - 2, color);
}

// The live sub-cell position: a black ring under a white dot, since any single
// colour vanishes into one of the cell colours or the white border.
static void drawGridDot(int dx, int dy) {
    tft.fillCircle(dx, dy, 4, COLOR_BLACK);
    tft.fillCircle(dx, dy, 3, COLOR_WHITE);
}

// --- Collecting screens' shared status and keys -----------------------------------
// Whether FINISH can end this collection yet: the floors nav applies in
// magBinCalFinishBaseline(), so a FINISH the screen offers is one nav takes.
bool calCanFinish(const CalProgressPacket& pkt) {
    if (pkt.cal_type == (uint8_t)CalType::MOUNTED) return pkt.sample_count > 0;
    const int need = pkt.phase == (uint8_t)CalPhase::GAP_FILL ? MAG_CAL_GAPFILL_MIN_SAMPLES
                                                              : MAG_CAL_ROUGH_SCAN_MIN_SAMPLES;
    return pkt.sample_count >= need;
}

// The step's own instruction while collecting; "Finishing..." once FINISH is
// pressed and nav hasn't confirmed; "Done - saving" for the hold after it has.
static void drawCollectingFooter(const CalProgressPacket& pkt, bool finishing,
                                 const CalText& instruction) {
    static const CalKey kQuitDim   = { "hold: QUIT", KeyStyle::DIM };
    static const CalKey kFinish    = { "FINISH", KeyStyle::NORMAL };
    static const CalKey kFinishDim = { "FINISH", KeyStyle::DIM };
    if (pkt.complete) {
        const CalText done = { pkt.max_samples_reached ? "Buffer full - saving" : "Done - saving",
                               COLOR_GREEN };
        drawCalFooter(&done, 1, KEY_BLANK, KEY_BLANK);
    } else if (finishing) {
        static const CalText kFinishing = { "Finishing...", COLOR_GREEN };
        drawCalFooter(&kFinishing, 1, kQuitDim, kFinishDim);
    } else {
        drawCalFooter(&instruction, 1, KEY_QUIT, calCanFinish(pkt) ? kFinish : kFinishDim);
    }
}

// ---------------------------------------------------------------------------
// 3 Heading: the 12-point prompt, and the MARKED beat after each mark
//
// Work area, under the frame's header (counter = marks done of 12):
//   y  28  "Align to external compass"                     gray, size 2
//   y  48  TARGET (label at y 55)                           white, size 4, right
//   y  82  INDICATED HDG (label at y 86)                    gray, size 3, right
//   y 104  horizon, 80 px, left: the level gate's attitude at 6x
//          HOLD bar (170, 106); "P+0.8 R-0.4" at (92, 133); "Avg err" at (92, 159)
// The status line carries the level gate's verdict, and the MARK key goes
// green when it's ready.
//
// The target is the big number. The unit's own reading is smaller, gray and
// labelled INDICATED HDG, because the diver aligns the scooter to an external
// compass; a diver who tries to make the two numbers match is fitting the
// heading cal to itself.
//
// Degree sign is "\xF7", not "\xB0" -- and plus-minus is "\xF0", not "\xB1".
// The GFX classic font is CP437 (degree = 0xF8, plus-minus = 0xF1) and, with
// cp437() left off, GFX bumps every code >= 176 by one. The Latin-1 codes
// land on shade blocks -- the checkerboard that read as a fourth digit.
// Holds for every screen in this file.
// ---------------------------------------------------------------------------

// Drawn by the mounted cal's renderer further down, into its 80 px canvas.
static void drawHorizon(int16_t x0, int16_t y0, float pitchErrDeg, float rollDeg,
                        float tolDeg, float gain, bool inWindow);
static constexpr int   HDGCAL_AI_X    = 4;
static constexpr int   HDGCAL_AI_Y    = 104;
// px per degree. +-3 deg must read from the far end of the scooter: at the
// mounted screen's 2x it would be a 12 px band.
static constexpr float HDGCAL_AI_GAIN = 6.0f;
static constexpr int   HDGCAL_PANEL_X = 92;
// The right-hand bars on Heading and Mounted.
static constexpr int   CAL_BAR_X = 170;
static constexpr int   CAL_BAR_W = 146;
static constexpr int   CAL_BAR_H = 18;

void showHdgFourierCalPrompt(int step, int total, float targetDeg, float indicatedDeg,
                              float avgErrDeg, int errCount, uint8_t levelState,
                              uint8_t holdPct, float pitchDeg, float rollDeg) {
    if (!tftReady) return;
    static int         sStep      = -1;
    static int         sIndInt    = INT32_MIN;
    static int         sHorizSig  = INT32_MIN;
    static int         sAttSig    = INT32_MIN;
    static int         sAvgInt    = INT32_MIN;   // INT32_MIN + 1 = "--"
    static CalBarCache sHold;

    char counter[8];
    snprintf(counter, sizeof(counter), "%d/%d", step, total);
    drawCalHeader(CalStep::HEADING, counter);
    if (claimCalWork(calSig(CalWork::HDG_PROMPT))) {
        drawWorkText(4, 28, "Align to external compass", 2, COLOR_GRAY);
        drawWorkText(4, 55, "TARGET", 2, COLOR_GRAY);
        drawWorkText(4, 86, "INDICATED HDG", 2, COLOR_GRAY);
        drawWorkText(HDGCAL_PANEL_X, 107, "HOLD", 2, COLOR_GRAY);
        sStep = -1;  sIndInt = INT32_MIN;  sHorizSig = INT32_MIN;
        sAttSig = INT32_MIN;  sAvgInt = INT32_MIN;  sHold = CalBarCache{};
    }

    char buf[24];
    if (step != sStep) {
        sStep = step;
        const int d = ((int)lroundf(targetDeg) % 360 + 360) % 360;
        snprintf(buf, sizeof(buf), "%03d" "\xF7", d);
        drawWorkText(SCREEN_WIDTH - 4 - textW(buf, 4), 48, buf, 4, COLOR_WHITE);
    }
    const int indInt = ((int)lroundf(indicatedDeg) % 360 + 360) % 360;
    if (indInt != sIndInt) {
        sIndInt = indInt;
        snprintf(buf, sizeof(buf), "%03d" "\xF7", indInt);
        drawWorkText(SCREEN_WIDTH - 4 - textW(buf, 3), 82, buf, 3, COLOR_GRAY);
    }

    // INACTIVE: a nav build too old to run the gate. Nothing to show, and the
    // display doesn't gate MARK either (display_main.cpp).
    const bool gateLive = levelState != (uint8_t)LevelGateState::INACTIVE;
    if (gateLive) {
        const int horizSig = ((int)lroundf(pitchDeg * 4.0f) & 0x3FF) |
                             (((int)lroundf(rollDeg * 4.0f) & 0x3FF) << 10);
        if (horizSig != sHorizSig) {
            sHorizSig = horizSig;
            drawHorizon(HDGCAL_AI_X, HDGCAL_AI_Y, pitchDeg, rollDeg, HDG_CAL_LEVEL_TOL_DEG,
                        HDGCAL_AI_GAIN, level_gate::isLevel(pitchDeg, rollDeg));
        }
        const int attSig = ((int)lroundf(pitchDeg * 10.0f) & 0xFFFF) |
                           (((int)lroundf(rollDeg * 10.0f) & 0xFFFF) << 16);
        if (attSig != sAttSig) {
            sAttSig = attSig;
            snprintf(buf, sizeof(buf), "P%+.1f R%+.1f  ", (double)pitchDeg, (double)rollDeg);
            drawWorkText(HDGCAL_PANEL_X, 133, buf, 2, COLOR_GRAY);
        }
    }
    drawCalBar(sHold, CAL_BAR_X, 106, CAL_BAR_W, CAL_BAR_H,
               gateLive ? (int)holdPct * (CAL_BAR_W - 2) / 100 : 0, COLOR_GREEN);

    // Average error: changes only when a point is marked.
    const int avgInt = errCount > 0 ? (int)lroundf(avgErrDeg) : INT32_MIN + 1;
    if (avgInt != sAvgInt) {
        sAvgInt = avgInt;
        if (errCount > 0) snprintf(buf, sizeof(buf), "Avg err %+d" "\xF7" "  ", avgInt);
        else              snprintf(buf, sizeof(buf), "Avg err --   ");
        drawWorkText(HDGCAL_PANEL_X, 159, buf, 2, COLOR_YELLOW);
    }

    CalText status = { "Press MARK when aligned", COLOR_WHITE };   // no gate data
    KeyStyle mark = KeyStyle::GO;
    if (!HDG_CAL_LEVEL_GATE) {
        // Test build: the gate is shown but not enforced. Say so, so a run
        // marked off-level is never mistaken for a normal one.
        status = { "GATE OFF - test build", COLOR_RED };
    } else {
        switch ((LevelGateState)levelState) {
            case LevelGateState::NOT_LEVEL: status = { "Not level", COLOR_RED };   mark = KeyStyle::DIM; break;
            case LevelGateState::MOVING:
            case LevelGateState::SETTLING:  status = { "Hold still", COLOR_YELLOW }; mark = KeyStyle::DIM; break;
            case LevelGateState::READY:     status = { "Ready - press MARK", COLOR_GREEN }; break;
            default:                        break;
        }
    }
    drawCalFooter(&status, 1, KEY_QUIT, { "MARK", mark });
}

// MARKED: drawn the moment MARK is pressed (from display_main's button
// handler, not the next render) and held for HDG_CAL_ACK_HOLD_MS, because the
// prompt alone gives no feedback until it happens to redraw. The counter has
// already advanced. nextTargetDeg < 0 = that was the last point.
void showHdgFourierCalAck(int marksDone, int total, float targetDeg, float indicatedDeg,
                           float errDeg, float avgErrDeg, float nextTargetDeg) {
    if (!tftReady) return;
    char counter[8];
    snprintf(counter, sizeof(counter), "%d/%d", marksDone, total);
    drawCalHeader(CalStep::HEADING, counter);
    if (claimCalWork(calSig(calSig(CalWork::HDG_MARKED), &marksDone, sizeof(marksDone)))) {
        const char* marked = "MARKED";
        drawWorkText((SCREEN_WIDTH - textW(marked, 5)) / 2, 44, marked, 5, COLOR_GREEN);
        char line[32];
        snprintf(line, sizeof(line), "TGT %03.0f" "\xF7" "  IND %03.0f" "\xF7",
                 (double)targetDeg, (double)indicatedDeg);
        drawWorkText((SCREEN_WIDTH - textW(line, 2)) / 2, 108, line, 2, COLOR_WHITE);
        snprintf(line, sizeof(line), "err %+.0f" "\xF7" "  avg %+.0f" "\xF7",
                 (double)errDeg, (double)avgErrDeg);
        drawWorkText((SCREEN_WIDTH - textW(line, 2)) / 2, 134, line, 2, COLOR_YELLOW);
    }
    static char next[8];
    if (nextTargetDeg < 0.0f) snprintf(next, sizeof(next), "upload");
    else                      snprintf(next, sizeof(next), "%03.0f" "\xF7", (double)nextTargetDeg);
    const CalText parts[] = { { "Next: ", COLOR_GRAY }, { next, COLOR_WHITE } };
    static const CalKey kQuitDim = { "hold: QUIT", KeyStyle::DIM };
    static const CalKey kMarkDim = { "MARK", KeyStyle::DIM };
    drawCalFooter(parts, 2, kQuitDim, kMarkDim);
}

// ---------------------------------------------------------------------------
// Cloud calibration (docs/cloud-calibration-plan.md)
// ---------------------------------------------------------------------------

// Greedy word-wrap for short status text, at whatever text size the caller
// set. maxCharsPerLine must fit SCREEN_WIDTH at that size (6 px a character
// per unit of size).
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

// Upload in progress. Nav is blocked in the upload and fit and sends
// nothing, so only the seconds count moves; it redraws when it changes.
void showCloudCalWaiting(CalStep step, uint32_t secondsWaiting) {
    if (!tftReady) return;
    static uint32_t sShownS = UINT32_MAX;
    drawCalHeader(step, "UPLOAD");
    if (claimCalWork(calSig(CalWork::UPLOAD))) {
        drawWorkText(8, 44, "Uploading", 3, COLOR_WHITE);
        drawWorkText(8, 128, "Takes up to 30 s.", 2, COLOR_GRAY);
        drawWorkText(8, 150, "The unit is busy.", 2, COLOR_GRAY);
        sShownS = UINT32_MAX;
    }
    if (secondsWaiting != sShownS) {
        sShownS = secondsWaiting;
        char buf[12];
        snprintf(buf, sizeof(buf), "%lu s  ", (unsigned long)secondsWaiting);
        drawWorkText(8, 80, buf, 3, COLOR_YELLOW);
    }
    static const CalText kStatus[] = { { "Please wait", COLOR_GRAY } };
    drawCalFooter(kStatus, 1, KEY_BLANK, KEY_BLANK);
}

void showCloudCalOffline(CalStep step) {
    if (!tftReady) return;
    drawCalHeader(step, "OFFLINE");
    if (claimCalWork(calSig(CalWork::OFFLINE))) {
        drawWorkText(8, 44, "No WiFi", 3, COLOR_YELLOW);
        drawWorkText(8, 84, "Samples are saved", 2, COLOR_WHITE);
        drawWorkText(8, 104, "on the unit.", 2, COLOR_WHITE);
    }
    static const CalText kStatus[] = { { "Next: ", COLOR_GRAY },
                                       { "upload at tern.local", COLOR_WHITE } };
    drawCalFooter(kStatus, 2, KEY_BLANK, KEY_OK);
}

void showCloudCalFailed(CalStep step, CalFailKind kind, const char* message) {
    if (!tftReady) return;
    const char* msg = (message && message[0]) ? message : "unknown error";
    drawCalHeader(step, "FAILED");
    uint32_t sig = calSig(CalWork::FAILED);
    sig = calSig(sig, &kind, sizeof(kind));
    sig = calSigStr(sig, msg);
    if (claimCalWork(sig)) {
        const char* head = kind == CalFailKind::REJECTED      ? "Fit rejected"
                         : kind == CalFailKind::REFUSED       ? "Can't start"
                         : kind == CalFailKind::NOT_INSTALLED ? "Not installed"
                                                              : "Upload failed";
        drawWorkText(8, 44, head, 3, COLOR_RED);
        // 26 characters a line at size 2; the wire's 64-character error fits
        // in three. nav cuts a longer one at a sentence or word boundary.
        tft.setTextSize(2);
        tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
        printWrapped(8, 84, msg, 26, 20, 3);
    }
    // A rejection is the server's verdict on samples it received, and a
    // refusal means nothing was collected: retrying at tern.local helps
    // neither, so only a transport failure points there.
    static const CalText kTransport[] = { { "Next: ", COLOR_GRAY }, { "retry at tern.local", COLOR_WHITE } };
    static const CalText kRejected[]  = { { "Next: ", COLOR_GRAY }, { "collect again", COLOR_WHITE } };
    static const CalText kRefused[]   = { { "Nothing was collected", COLOR_GRAY } };
    static const CalText kNotInst[]   = { { "Your old cal stays on", COLOR_GRAY } };
    switch (kind) {
        case CalFailKind::REJECTED:      drawCalFooter(kRejected, 2, KEY_BLANK, KEY_OK); break;
        case CalFailKind::REFUSED:       drawCalFooter(kRefused, 1, KEY_BLANK, KEY_OK);  break;
        case CalFailKind::NOT_INSTALLED: drawCalFooter(kNotInst, 1, KEY_BLANK, KEY_OK);  break;
        default:                         drawCalFooter(kTransport, 2, KEY_BLANK, KEY_OK); break;
    }
}

// After a hold on BTN1: QUIT on a collecting screen, REJECT on a result.
void showCalDiscarded(CalStep step, bool rejected) {
    if (!tftReady) return;
    drawCalHeader(step, "");
    if (claimCalWork(calSig(calSig(CalWork::DISCARDED), &rejected, sizeof(rejected)))) {
        drawWorkText(8, 52, rejected ? "Rejected" : "Quit", 4, COLOR_WHITE);
        drawWorkText(8, 104, rejected ? "Your old cal stays on." : "Nothing was uploaded.", 2,
                     COLOR_GRAY);
    }
    static const CalText kStatus[] = { { "Back to the nav screen", COLOR_GRAY } };
    drawCalFooter(kStatus, 1, KEY_BLANK, KEY_BLANK);
}

// ---------------------------------------------------------------------------
// Automatic dive-log upload progress.
// ---------------------------------------------------------------------------
void showLogUpload(uint8_t done, uint8_t total, uint32_t secondsWaiting) {
    if (!tftReady) return;
    invalidateNavCache();
    tft.fillScreen(COLOR_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(COLOR_CYAN, COLOR_BLACK);
    tft.setCursor(4, 4);
    tft.print("DIVE LOGS");
    tft.drawFastHLine(0, 28, SCREEN_WIDTH, COLOR_CYAN);

    tft.setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft.setCursor(4, 60);
    tft.print("Uploading...");

    if (total > 0) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%u of %u", (unsigned)(done + 1 > total ? total : done + 1),
                 (unsigned)total);
        tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
        tft.setCursor(4, 92);
        tft.print(buf);
    }

    tft.setTextSize(1);
    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(4, 132);
    tft.print("Hang on -- the unit is busy");
    tft.setCursor(4, 146);
    tft.print("until this finishes.");

    char buf[16];
    snprintf(buf, sizeof(buf), "%lus", (unsigned long)secondsWaiting);
    tft.setTextColor(COLOR_GRAY, COLOR_BLACK);
    tft.setCursor(4, 174);
    tft.print(buf);
}

// The fit's result: one number in the band colour with GOOD/OK/POOR beside
// it, and the decision. Baseline and mounted show rms_pct; heading shows the
// server's leave-one-out worst case, which arrives in the same field (< 0:
// the server didn't send it, shown as "--" rather than falling back to the
// RMS). The server's recommendation text is not shown: the band says it,
// and on heading it once quoted a second, different "max error".
void showCloudCalResult(CalStep step, uint8_t quality, float value, int16_t coverageGaps,
                        bool saving) {
    if (!tftReady) return;
    const bool isHdg = (step == CalStep::HEADING);
    char num[12];
    if (!isHdg)            snprintf(num, sizeof(num), "%.0f%%", (double)value);
    else if (value < 0.0f) snprintf(num, sizeof(num), "--" "\xF7");
    else                   snprintf(num, sizeof(num), "%.1f" "\xF7", (double)value);

    uint16_t bandColor = COLOR_GREEN;
    const char* band = "GOOD";
    if (quality == (uint8_t)CalCloudQuality::WARN)     { bandColor = COLOR_YELLOW; band = "OK"; }
    else if (quality == (uint8_t)CalCloudQuality::BAD) { bandColor = COLOR_RED;    band = "POOR"; }

    drawCalHeader(step, saving ? "SAVING" : "RESULT");
    // A tern.local retry-upload can deliver a new result while this is up.
    uint32_t sig = calSig(CalWork::RESULT);
    sig = calSig(sig, &step, sizeof(step));
    sig = calSig(sig, &quality, sizeof(quality));
    sig = calSigStr(sig, num);
    sig = calSig(sig, &coverageGaps, sizeof(coverageGaps));
    if (claimCalWork(sig)) {
        drawWorkText(8, 36, num, 6, bandColor);
        drawWorkText(SCREEN_WIDTH - 4 - textW(band, 4), 44, band, 4, bandColor);
        drawWorkText(8, 96, isHdg ? "likely worst error" : "RMS error", 2, COLOR_GRAY);
        // Baseline only: empty cells in the coverage grid, an axis of its own
        // apart from the RMS. Silent when there are none, or when it doesn't
        // apply (-1: mounted, heading, or a pre-9-axis CSV).
        if (coverageGaps > 0) {
            char gbuf[28];
            snprintf(gbuf, sizeof(gbuf), "%d orientation gap%s", coverageGaps,
                     coverageGaps == 1 ? "" : "s");
            drawWorkText(8, 120, gbuf, 2, COLOR_GRAY);
        }
    }

    static const CalKey kAccept = { "ACCEPT", KeyStyle::NORMAL };
    if (saving) {
        static const CalText kSaving = { "Saving...", COLOR_GREEN };
        drawCalFooter(&kSaving, 1, KEY_BLANK, KEY_BLANK);
    } else if (quality == (uint8_t)CalCloudQuality::BAD) {
        static char again[16];
        snprintf(again, sizeof(again), "run %s again", kCalStepIds[(int)step]);
        const CalText parts[] = { { "Next: ", COLOR_GRAY }, { again, COLOR_RED } };
        drawCalFooter(parts, 2, KEY_REJECT, kAccept);
    } else {
        static const CalText kStatus[] = { { "Press ACCEPT to install", COLOR_WHITE } };
        drawCalFooter(kStatus, 1, KEY_REJECT, kAccept);
    }
}

// After ACCEPT, once nav reports back. What comes next isn't known until
// then: for a baseline it depends on the gap-fill map that came with it.
void showCalInstalled(CalStep step, int16_t targetCount, const char* mapError) {
    if (!tftReady) return;
    const bool isBase = (step == CalStep::BASELINE);
    const bool mapOk  = isBase && targetCount >= 0;
    const char* err   = mapError ? mapError : "";
    drawCalHeader(step, "SAVED");
    uint32_t sig = calSig(calSig(CalWork::INSTALLED), &step, sizeof(step));
    sig = calSig(sig, &targetCount, sizeof(targetCount));
    sig = calSigStr(sig, err);
    if (claimCalWork(sig)) {
        drawWorkText(8, 44, "Installed", 3, COLOR_GREEN);
        char line[28];
        if (mapOk && targetCount > 0) {
            snprintf(line, sizeof(line), "Gap-fill map: %d cell%s", targetCount,
                     targetCount == 1 ? "" : "s");
            drawWorkText(8, 84, line, 2, COLOR_WHITE);
            drawWorkText(8, 104, "ready on the unit.", 2, COLOR_WHITE);
        } else if (mapOk) {
            drawWorkText(8, 84, "Coverage is complete:", 2, COLOR_WHITE);
            drawWorkText(8, 104, "no gaps to fill.", 2, COLOR_WHITE);
        } else if (isBase) {
            drawWorkText(8, 84, "Gap-fill map not updated:", 2, COLOR_YELLOW);
            tft.setTextSize(2);
            tft.setTextColor(COLOR_YELLOW, COLOR_BLACK);
            printWrapped(8, 104, err, 26, 20, 3);
        } else if (step == CalStep::MOUNTED) {
            drawWorkText(8, 84, "Mounted cal is on.", 2, COLOR_WHITE);
        } else if (step == CalStep::HEADING) {
            drawWorkText(8, 84, "Heading cal is on.", 2, COLOR_WHITE);
        }
    }

    static const CalText kNextGaps[]  = { { "Next: ", COLOR_GRAY }, { "1a FILL GAPS", COLOR_WHITE } };
    static const CalText kNextMount[] = { { "Next: ", COLOR_GRAY }, { "2 MOUNTED", COLOR_WHITE } };
    static const CalText kNextSync[]  = { { "Next: ", COLOR_GRAY }, { "sync at tern.local", COLOR_WHITE } };
    static const CalText kNextHdg[]   = { { "Next: ", COLOR_GRAY }, { "3 HEADING", COLOR_WHITE } };
    static const CalText kAllDone[]   = { { "All 3 steps done", COLOR_GREEN } };
    if (isBase && !mapOk)            drawCalFooter(kNextSync, 2, KEY_BLANK, KEY_OK);
    else if (isBase && targetCount)  drawCalFooter(kNextGaps, 2, KEY_BLANK, KEY_OK);
    else if (isBase)                 drawCalFooter(kNextMount, 2, KEY_BLANK, KEY_OK);
    else if (step == CalStep::MOUNTED) drawCalFooter(kNextHdg, 2, KEY_BLANK, KEY_OK);
    else                             drawCalFooter(kAllDone, 1, KEY_BLANK, KEY_OK);
}

// Gap-fill's upload result: nothing to accept on the unit. That fit covers
// only the cells just patched -- a handful out of 60 -- and its only useful
// destiny is as a merge candidate on the website. Offering it as an install
// would let a diver who presses BTN2 without reading overwrite a good
// full-sphere baseline with a partial one. See
// divemap/docs/architecture/baseline-cal-coverage-feedback-plan.md.
// coverageGaps counts the patch's own thin or empty cells; -1 = not sent.
void showGapFillUploaded(float rmsPct, int16_t coverageGaps) {
    if (!tftReady) return;
    drawCalHeader(CalStep::FILL_GAPS, "SENT");
    char rbuf[20];
    snprintf(rbuf, sizeof(rbuf), "Patch RMS %.0f%%", (double)rmsPct);
    uint32_t sig = calSigStr(calSig(CalWork::SENT), rbuf);
    sig = calSig(sig, &coverageGaps, sizeof(coverageGaps));
    if (claimCalWork(sig)) {
        drawWorkText(8, 44, "Uploaded", 3, COLOR_GREEN);
        drawWorkText(8, 84, rbuf, 2, COLOR_WHITE);
        if (coverageGaps >= 0) {
            char gbuf[28];
            snprintf(gbuf, sizeof(gbuf), "%d cell%s still thin", coverageGaps,
                     coverageGaps == 1 ? "" : "s");
            drawWorkText(8, 108, gbuf, 2, COLOR_GRAY);
        }
    }
    static const CalText kStatus[] = { { "Next: ", COLOR_GRAY }, { "merge on website", COLOR_WHITE } };
    drawCalFooter(kStatus, 2, KEY_BLANK, KEY_OK);
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
// 1 Baseline (once its in-session bootstrap has locked, CalPhase::BASELINE_GRID;
// before that it shows showBaselineRoughScan()) and 1a Fill gaps: the 5 x 12
// coverage grid. Mounted has its own, showMountedCal().
//
// Work area, under the frame's header:
//   y  27      N E S W ticks; row labels +60 ... -60 on the left (size 1)
//   y  36-155  grid, 24 px rows; current cell in a 2 px white border, and a
//              dot at the live (heading, pitch) inside it
//   y 162      "38/60 cells" (Baseline) or "5 cells left" (Fill gaps), white;
//              "284° +41°" yellow; the roll widget at (287, 157)
//
// Gap-fill shares this screen deliberately rather than getting its own. Its
// whole promise is that the cell it highlights is the cell the website
// flagged, and the fastest way to break that is two grid renderers with
// subtly different geometry. One function, one cell layout, one ordering.
// ---------------------------------------------------------------------------
static constexpr int GRID_ROWS = 5;

// The roll widget: the cell under the highlight's roll coverage, a square
// split by its diagonals into upright (top), right side, upside down (bottom)
// and left side -- mag_orient::rollSector()'s order -- each coloured like a
// cell, with a white outline on the live roll sector. Persistent, not a mode
// swap with the grid (rejected in the gap-fill orientation UX plan as
// confusing): it sits here, always current, as the diver moves between cells.
static void drawRollWidget(const CalProgressPacket& pkt) {
    const int WX = 287, WY = 157, E = 25;   // 26 px square
    const int cx = WX + E / 2, cy = WY + E / 2;
    for (int k = 0; k < MAG_CAL_ROLL_SECTORS; k++) {
        uint16_t color;
        if (pkt.current_bin < 0) {
            color = COLOR_OPEN_CELL;   // no live orientation: unknown, not "all satisfied"
        } else {
            switch (pkt.current_bin_roll_targeted[k]) {
                case 0:  color = COLOR_GREEN;  break;   // satisfied
                case 1:  color = COLOR_YELLOW; break;   // some, under cap
                default: color = COLOR_RED;    break;   // none yet
            }
        }
        switch (k) {
            case 0:  tft.fillTriangle(WX, WY, WX + E, WY, cx, cy, color); break;
            case 1:  tft.fillTriangle(WX + E, WY, WX + E, WY + E, cx, cy, color); break;
            case 2:  tft.fillTriangle(WX + E, WY + E, WX, WY + E, cx, cy, color); break;
            default: tft.fillTriangle(WX, WY + E, WX, WY, cx, cy, color); break;
        }
    }
    // Skipped when current_roll_sector is -1 (unmappable). Near-vertical roll
    // IS reported, not withheld: reconstructRoll()'s degenerate guard never
    // fires in practice and returns 0.0 = upright there, which with only
    // cos(elevation) of gravity across the tracked axis is largely sensor
    // noise. See docs/calibration-guide.md's "aim for the edge of the band".
    if (pkt.current_roll_sector >= 0 && pkt.current_roll_sector < MAG_CAL_ROLL_SECTORS) {
        int x0, y0, x1, y1;
        switch (pkt.current_roll_sector) {
            case 0:  x0 = WX;     y0 = WY;     x1 = WX + E; y1 = WY;     break;
            case 1:  x0 = WX + E; y0 = WY;     x1 = WX + E; y1 = WY + E; break;
            case 2:  x0 = WX + E; y0 = WY + E; x1 = WX;     y1 = WY + E; break;
            default: x0 = WX;     y0 = WY + E; x1 = WX;     y1 = WY;     break;
        }
        tft.drawLine(x0, y0, x1, y1, COLOR_WHITE);
        tft.drawLine(x0, y0, cx, cy, COLOR_WHITE);
        tft.drawLine(x1, y1, cx, cy, COLOR_WHITE);
    }
}

void showCalGrid(const CalProgressPacket& pkt, bool finishing) {
    if (!tftReady) return;
    const bool isGapFill      = (pkt.phase == (uint8_t)CalPhase::GAP_FILL);
    // The baseline live grid: every cell is a target, judged by the same
    // roll-aware "finished" verdict as gap-fill.
    const bool isBaselineGrid = !isGapFill;
    const int  nBins = pkt.bins_total < 60 ? pkt.bins_total : 60;

    static uint32_t sGridSig = 0;
    static char     sCells[20] = "";
    static char     sOrient[16] = "";
    static uint32_t sRollSig = 0;

    // In gap-fill the job is the target list, not the grid: pkt.bins_total is
    // 60 there because it sizes the wire array and the cell loop, so the
    // display counts targets itself from the map it already has.
    int targetTotal = 0;
    if (isGapFill && pkt.has_targets) {
        for (int i = 0; i < nBins; i++) {
            if (pkt.targets[i] == 1 || pkt.targets[i] == 2) targetTotal++;
        }
    }

    // Counter: what ends the step. Baseline ends when the sample buffer
    // fills (or on FINISH), never on a full grid; gap-fill when every target
    // is finished.
    char counter[12];
    if (isGapFill) snprintf(counter, sizeof(counter), "%d/%d", pkt.bins_green, targetTotal);
    else           snprintf(counter, sizeof(counter), "%u/%d", (unsigned)pkt.sample_count,
                            MAG_CAL_BIN_MAX_SAMPLES);
    drawCalHeader(isGapFill ? CalStep::FILL_GAPS : CalStep::BASELINE, counter);
    if (claimCalWork(calSig(CalWork::GRID))) {
        // These labels are magnetic and only as good as the bias behind them:
        // the installed baseline for gap-fill, the in-session sphere fit for
        // the baseline grid (within a few degrees of the real fit; see
        // docs/baseline-live-grid-plan.md).
        static const char* const kRows[GRID_ROWS] = { "+60", "+30", "  0", "-30", "-60" };
        drawGridTicks(kRows, GRID_ROWS, CAL_GRID_ROW_H);
        sGridSig = 0;  sCells[0] = '\0';  sOrient[0] = '\0';  sRollSig = 0;
    }

    // --- Cells ---------------------------------------------------------------
    uint16_t colors[60];
    for (int i = 0; i < nBins; i++) {
        const uint8_t cnt = pkt.bin_counts[i];
        if (isGapFill) {
            // Colour by the job, not by raw density. An untargeted cell is not
            // "empty and bad", it's "not your problem" -- painting it red would
            // send the diver chasing cells the server is happy with, which is
            // how the over-weighted bins this whole feature exists to fix get
            // made.
            const uint8_t st = pkt.has_targets ? pkt.targets[i] : 0;
            const bool targeted = (st == 1 || st == 2);   // thin or empty
            // "Finished" is the device's binRollSatisfied() verdict, not a
            // sample count: a cell hits MAG_CAL_GAPFILL_TARGET_CAP on 15
            // upright samples alone but isn't finished until the other three
            // roll sectors have their 5 each. Falls back to the count only
            // against a nav build too old to send the map.
            const bool done = pkt.has_cell_satisfied ? pkt.cell_satisfied[i]
                                                     : cnt >= MAG_CAL_GAPFILL_TARGET_CAP;
            colors[i] = !targeted ? COLOR_OPEN_CELL
                      : done      ? COLOR_GREEN
                      : cnt > 0   ? COLOR_YELLOW
                                  : COLOR_RED;
        } else {
            const bool done = pkt.has_cell_satisfied ? pkt.cell_satisfied[i]
                                                     : cnt >= MAG_CAL_BIN_GREEN_THRESHOLD;
            colors[i] = done ? COLOR_GREEN : cnt > 0 ? COLOR_YELLOW : COLOR_RED;
        }
    }

    // The dot: where inside the cell the unit is pointing. The border alone
    // quantizes attitude to 30 deg before the diver sees it, which gives them
    // nothing continuous to steer on. Elevation band edges, top row first,
    // MUST match mag_orient::elevBand(); the level band is 60 deg tall and the
    // rest 30, which is why this is a table and not arithmetic.
    const bool haveBin = pkt.current_bin >= 0 && pkt.current_bin < nBins;
    int dx = 0, dy = 0;
    if (haveBin) {
        static const float edges[6] = { 90.0f, MAG_CAL_ELEV_H2, MAG_CAL_ELEV_H1,
                                        MAG_CAL_ELEV_L1, MAG_CAL_ELEV_L2, -90.0f };
        float hdg = fmodf(pkt.cur_hdg_deg, 360.0f);
        if (hdg < 0.0f) hdg += 360.0f;
        float pitch = pkt.cur_pitch_deg;
        if (pitch >  90.0f) pitch =  90.0f;
        if (pitch < -90.0f) pitch = -90.0f;
        int dr = 0;
        while (dr < GRID_ROWS - 1 && pitch < edges[dr + 1]) dr++;
        const float span = edges[dr] - edges[dr + 1];
        float frac = (span > 0.0f) ? (edges[dr] - pitch) / span : 0.5f;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        const int gridW = 12 * CAL_GRID_COL_W, gridH = GRID_ROWS * CAL_GRID_ROW_H;
        dx = CAL_GRID_X + (int)(hdg / 360.0f * (float)gridW);
        dy = CAL_GRID_Y + (int)(((float)dr + frac) * (float)CAL_GRID_ROW_H);
        // Keep the whole dot inside the grid at the 360->0 wrap and the poles.
        if (dx < CAL_GRID_X + 3)             dx = CAL_GRID_X + 3;
        if (dx > CAL_GRID_X + gridW - 4)     dx = CAL_GRID_X + gridW - 4;
        if (dy < CAL_GRID_Y + 3)             dy = CAL_GRID_Y + 3;
        if (dy > CAL_GRID_Y + gridH - 4)     dy = CAL_GRID_Y + gridH - 4;
    }

    // Repaint the grid only when something on it changed. Every cell is
    // repainted then, gutter included, which wipes the old border and dot.
    uint32_t gridSig = calSig(2166136261u, colors, nBins * sizeof(colors[0]));
    gridSig = calSig(gridSig, pkt.bin_counts, nBins);
    const int pos[3] = { haveBin ? pkt.current_bin : -1, dx, dy };
    gridSig = calSig(gridSig, pos, sizeof(pos));
    if (gridSig != sGridSig) {
        sGridSig = gridSig;
        for (int i = 0; i < nBins; i++) {
            // Counts in cells still wanting samples. Not in gap-fill's dark
            // untargeted cells: a number there reads as a task, and it isn't.
            const bool showCount = colors[i] != COLOR_GREEN && colors[i] != COLOR_OPEN_CELL;
            drawGridCell(i, CAL_GRID_ROW_H, colors[i], showCount ? pkt.bin_counts[i] : 0);
        }
        if (haveBin) {
            drawGridHighlight(pkt.current_bin, CAL_GRID_ROW_H, COLOR_WHITE);
            drawGridDot(dx, dy);
        }
    }

    // --- Readout row -----------------------------------------------------------
    // Fixed widths, padded so a shorter value erases a longer one: the count
    // gets 13 characters (x 4-159), the orientation 9 (x 166-273), and the
    // roll widget starts at x 287.
    char text[20];
    char cbuf[20];
    if (isGapFill) snprintf(text, sizeof(text), "%d cells left", targetTotal - pkt.bins_green);
    else           snprintf(text, sizeof(text), "%d/%d cells", pkt.bins_green, nBins);
    snprintf(cbuf, sizeof(cbuf), "%-13s", text);
    if (strcmp(cbuf, sCells) != 0) {
        strncpy(sCells, cbuf, sizeof(sCells) - 1);
        drawWorkText(4, 162, cbuf, 2, COLOR_WHITE);
    }
    // Numeric, not an 8-point compass name: the grid is 12 columns of 30 deg
    // and "NE" spans parts of two, so it can't answer the one question this
    // readout exists for -- which column am I in. Truncated with (int),
    // matching hdgSector()'s floor; a rounded 360 would name a column that
    // isn't there. The same algebraic reconstruction chose the highlighted
    // cell (util/mag_cal_orient.h), so the two can never disagree.
    char obuf[16];
    text[0] = '\0';
    if (haveBin) {
        float hdg = fmodf(pkt.cur_hdg_deg, 360.0f);
        if (hdg < 0.0f) hdg += 360.0f;
        snprintf(text, sizeof(text), "%03d\xF7 %+.0f\xF7", (int)hdg, (double)pkt.cur_pitch_deg);
    }
    snprintf(obuf, sizeof(obuf), "%-9s", text);
    if (strcmp(obuf, sOrient) != 0) {
        strncpy(sOrient, obuf, sizeof(sOrient) - 1);
        drawWorkText(166, 162, obuf, 2, COLOR_YELLOW);
    }
    uint32_t rollSig = calSig(2166136261u, pkt.current_bin_roll_targeted,
                              sizeof(pkt.current_bin_roll_targeted));
    const int rollPos[2] = { pkt.current_bin < 0 ? -1 : 0, pkt.current_roll_sector };
    rollSig = calSig(rollSig, rollPos, sizeof(rollPos));
    if (rollSig != sRollSig) {
        sRollSig = rollSig;
        drawRollWidget(pkt);
    }

    // --- Status and keys ---------------------------------------------------------
    // Baseline's last 10% before the buffer fills and ends it.
    static char nearFullText[24];
    snprintf(nearFullText, sizeof(nearFullText), "Auto-finish at %d", MAG_CAL_BIN_MAX_SAMPLES);
    const CalText fill     = { "Fill red & yellow cells", COLOR_YELLOW };
    const CalText nearFull = { nearFullText, COLOR_YELLOW };
    const bool isNearFull = isBaselineGrid &&
                            (int)pkt.sample_count * 10 >= MAG_CAL_BIN_MAX_SAMPLES * 9;
    drawCollectingFooter(pkt, finishing, isNearFull ? nearFull : fill);
}

// ---------------------------------------------------------------------------
// 2 Mounted: the dwell-capture screen. See src/util/mounted_dwell.h for the
// rule it is showing, and divemap's mounted-cal-dwell-plan.md for why.
//
// The collector records a cell only once the unit has sat still inside that
// row's attitude window, so this screen's job is to show the diver how far
// they are from that: an attitude indicator for "is it level / tilted enough",
// a motion bar for "is it still", and a hold bar for "is it recording".
//
// Work area, under the frame's header (counter = cells filled of 36):
//   y  27      N E S W ticks; row labels +30, 0, -30 on the left
//   y  36-95   3 x 12 grid, 20 px rows. Every empty cell is red; the current
//              cell is outlined white inside the window, yellow outside it
//   y 100-179  attitude indicator, 80 px, left
//              MOTION (92, 102), bar (170, 101) with the cause inside it
//              HOLD   (92, 126), bar (170, 125)
//              heading, pitch, roll at (92, 152)
//
// Everything is redrawn only when its own inputs change, and the indicator is
// composed off-screen and blitted, because this is repainted every loop pass
// and a horizon that flickers is harder to level than no horizon at all.
// ---------------------------------------------------------------------------
static constexpr int MC_ROW_H   = 20;
static constexpr int MC_AI_X    = 4;     // attitude indicator, top-left
static constexpr int MC_AI_Y    = 100;
static constexpr int MC_AI_S    = 80;    // square; Mounted and Heading both
static constexpr int MC_PANEL_X = 92;
static constexpr uint16_t MC_COLOR_SKY    = 0x2AD4;
static constexpr uint16_t MC_COLOR_GROUND = 0x7A83;

static GFXcanvas16* g_mcHorizon = nullptr;
static_assert(MC_AI_X + MC_AI_S < MC_PANEL_X, "mounted cal horizon overlaps its panel");
static_assert(MC_AI_Y + MC_AI_S <= CAL_WORK_Y + CAL_WORK_H, "mounted cal horizon leaves the work area");
static_assert(HDGCAL_AI_X + MC_AI_S < HDGCAL_PANEL_X, "heading cal horizon overlaps its panel");
static_assert(HDGCAL_AI_Y + MC_AI_S <= CAL_WORK_Y + CAL_WORK_H, "heading cal horizon leaves the work area");

// Horizon tilts with roll and slides with pitch, as an aircraft's does: nose
// up shows more sky, right side down drops the horizon's right end (the sign
// was confirmed on the bench 2026-10-11; the first build had roll backwards).
//
// It is drawn as the ERROR from the row being worked, exaggerated
// MC_AI_GAIN times, not as true attitude: the brackets stay in the middle and
// the line sits between them when the unit is at that row's pitch and level
// in roll. At true scale the +-10 deg window was a 20 px band and the line 2 px
// thick, too fine to read from the far end of the scooter. The cost is that
// the line jumps when the nearest row changes, halfway between two rows.
static constexpr float MC_AI_GAIN   = 2.0f;   // px per degree of pitch; also multiplies roll
static constexpr int   MC_AI_LINE_W = 5;      // horizon thickness, px

// Shared by the mounted cal and the 12-point heading cal's level gate:
// pitchErrDeg is pitch minus the target pitch, gain is px per degree (and the
// roll multiplier), and the brackets mark +-tolDeg, green when inWindow.
static void drawHorizon(int16_t x0, int16_t y0, float pitchErrDeg, float rollDeg,
                        float tolDeg, float gain, bool inWindow) {
    if (!g_mcHorizon) g_mcHorizon = new GFXcanvas16(MC_AI_S, MC_AI_S);
    if (!g_mcHorizon || !g_mcHorizon->getBuffer()) return;  // no heap: skip, never crash
    GFXcanvas16& c = *g_mcHorizon;
    const int mid = MC_AI_S / 2;

    const float offset = pitchErrDeg * gain;
    float drawnRoll = rollDeg * gain;
    if (drawnRoll >  60.0f) drawnRoll =  60.0f;   // tan() runs away; past here the
    if (drawnRoll < -60.0f) drawnRoll = -60.0f;   // message is "nowhere near" anyway
    const float slope = tanf(drawnRoll * 0.0174532925f);
    for (int x = 0; x < MC_AI_S; x++) {
        float yh = (float)mid + offset + (float)(x - mid) * slope;
        if (yh < 0.0f) yh = 0.0f;
        if (yh > (float)MC_AI_S) yh = (float)MC_AI_S;
        const int y = (int)yh;
        if (y > 0)       c.drawFastVLine(x, 0, y, MC_COLOR_SKY);
        if (y < MC_AI_S) c.drawFastVLine(x, y, MC_AI_S - y, MC_COLOR_GROUND);
    }
    const int yl = (int)((float)mid + offset - (float)mid * slope);
    const int yr = (int)((float)mid + offset + (float)(MC_AI_S - 1 - mid) * slope);
    for (int t = -MC_AI_LINE_W / 2; t <= MC_AI_LINE_W / 2; t++) {
        c.drawLine(0, yl + t, MC_AI_S - 1, yr + t, COLOR_WHITE);
    }

    const uint16_t mark = inWindow ? COLOR_GREEN : COLOR_YELLOW;
    const int tol = (int)(tolDeg * gain);
    for (int side = 0; side < 2; side++) {
        const int xOuter = side ? MC_AI_S - 4 : 3;
        const int xArm   = side ? MC_AI_S - 14 : 3;   // the bracket's arms run 11 px inward
        for (int t = 0; t < 3; t++) {   // 3 px thick
            c.drawFastVLine(xOuter + (side ? -t : t), mid - tol, 2 * tol + 1, mark);
            c.drawFastHLine(xArm, mid - tol + t, 11, mark);
            c.drawFastHLine(xArm, mid + tol - t, 11, mark);
        }
    }
    // Fixed centre mark: where the unit's nose is pointing.
    c.drawFastHLine(mid - 12, mid, 8, COLOR_BLACK);
    c.drawFastHLine(mid + 5,  mid, 8, COLOR_BLACK);
    c.fillCircle(mid, mid, 2, COLOR_BLACK);
    c.drawRect(0, 0, MC_AI_S, MC_AI_S, COLOR_GRAY);

    tft.drawRGBBitmap(x0, y0, c.getBuffer(), MC_AI_S, MC_AI_S);
}

static void drawMountedHorizon(float pitchDeg, float rollDeg, int row, bool inWindow) {
    const float tolDeg = (row == mounted_dwell::kRowLevel) ? MAG_CAL_DWELL_LEVEL_TOL_DEG
                                                           : MAG_CAL_DWELL_TILT_PITCH_TOL_DEG;
    drawHorizon(MC_AI_X, MC_AI_Y, pitchDeg - mounted_dwell::rowTargetPitchDeg(row), rollDeg,
                tolDeg, MC_AI_GAIN, inWindow);
}

// One line saying what to do next, for the status line.
static const char* mountedCalInstruction(const CalProgressPacket& pkt, int row, uint16_t& colorOut) {
    colorOut = COLOR_YELLOW;
    switch ((CalDwellState)pkt.capture_state) {
        case CalDwellState::EDGE:      return "Turn a bit more";
        case CalDwellState::MOVING:    return "Hold still";
        case CalDwellState::SETTLING:
        case CalDwellState::CAPTURING: colorOut = COLOR_GREEN; return "Holding...";
        case CalDwellState::CELL_DONE:
            colorOut = COLOR_GREEN;
            if (!mounted_dwell::rowComplete(pkt.bin_counts, row)) return "Got it - turn";
            return (row == mounted_dwell::kRowLevel) ? "Row done - tilt" : "Row done";
        case CalDwellState::OUT_OF_WINDOW:
        default: {
            const float off = fabsf(pkt.cur_pitch_deg - mounted_dwell::rowTargetPitchDeg(row));
            if (row == mounted_dwell::kRowLevel) {
                return off <= MAG_CAL_DWELL_LEVEL_TOL_DEG ? "Level the roll" : "Level it";
            }
            if (off <= MAG_CAL_DWELL_TILT_PITCH_TOL_DEG) return "Level the roll";
            return (row == mounted_dwell::kRowUp) ? "Nose up to 30" : "Nose down to 30";
        }
    }
}

void showMountedCal(const CalProgressPacket& pkt, bool finishing) {
    if (!tftReady) return;

    // What was last drawn, per element.
    static uint32_t    sGridSig  = 0;
    static int         sHorizSig = INT32_MIN;
    static CalBarCache sMotion, sHold;
    static char        sReadout[20] = "";

    char counter[8];
    snprintf(counter, sizeof(counter), "%d/%d", pkt.bins_green, mounted_dwell::kNeedTotal);
    drawCalHeader(CalStep::MOUNTED, counter);
    if (claimCalWork(calSig(CalWork::MOUNTED))) {
        static const char* const kRows[3] = { "+30", "  0", "-30" };
        drawGridTicks(kRows, 3, MC_ROW_H);
        drawWorkText(MC_PANEL_X, 102, "MOTION", 2, COLOR_GRAY);
        drawWorkText(MC_PANEL_X, 126, "HOLD", 2, COLOR_GRAY);
        sGridSig = 0;  sHorizSig = INT32_MIN;  sMotion = CalBarCache{};  sHold = CalBarCache{};
        sReadout[0] = '\0';
    }

    float hdg = fmodf(pkt.cur_hdg_deg, 360.0f);
    if (hdg < 0.0f) hdg += 360.0f;
    const float pitch = pkt.cur_pitch_deg;
    const float roll  = pkt.cur_roll_deg;
    const int   row   = mounted_dwell::nearestRow(pitch);
    const bool  inWindow = pkt.capture_state != (uint8_t)CalDwellState::OUT_OF_WINDOW;

    // --- Grid, highlight and position dot -----------------------------------
    // The rows are 30 deg apart and each is drawn 30 deg tall, so the dot's
    // height is plain pitch: +45 at the top of the grid, -45 at the bottom.
    const int gridW = mounted_dwell::kSectors * CAL_GRID_COL_W;
    const int gridH = mounted_dwell::kRows * MC_ROW_H;
    int dx = CAL_GRID_X + (int)(hdg / 360.0f * (float)gridW);
    int dy = CAL_GRID_Y + (int)((45.0f - pitch) / 90.0f * (float)gridH);
    if (dx < CAL_GRID_X + 3)         dx = CAL_GRID_X + 3;
    if (dx > CAL_GRID_X + gridW - 4) dx = CAL_GRID_X + gridW - 4;
    if (dy < CAL_GRID_Y + 3)         dy = CAL_GRID_Y + 3;
    if (dy > CAL_GRID_Y + gridH - 4) dy = CAL_GRID_Y + gridH - 4;

    uint32_t gridSig = (uint32_t)dx | ((uint32_t)dy << 9) |
                       ((uint32_t)(pkt.current_bin & 0x3F) << 17) |
                       ((uint32_t)(inWindow ? 1 : 0) << 23);
    for (int i = 0; i < mounted_dwell::kCells; i++) {
        if (pkt.bin_counts[i]) gridSig = gridSig * 31u + (uint32_t)i + 1u;
    }
    if (gridSig != sGridSig) {
        sGridSig = gridSig;
        // Every cell the diver still has to fill is red, not just "open".
        for (int i = 0; i < mounted_dwell::kCells; i++) {
            drawGridCell(i, MC_ROW_H, pkt.bin_counts[i] ? COLOR_GREEN : COLOR_RED, 0);
        }
        if (pkt.current_bin >= 0 && pkt.current_bin < mounted_dwell::kCells) {
            // White once the attitude is inside the row's window, yellow while
            // it is only the nearest row: a cell cannot fill from out here.
            drawGridHighlight(pkt.current_bin, MC_ROW_H, inWindow ? COLOR_WHITE : COLOR_YELLOW);
        }
        drawGridDot(dx, dy);
    }

    // --- Attitude indicator -------------------------------------------------
    const int horizSig = ((int)lroundf(pitch * 2.0f) & 0x3FF) |
                         (((int)lroundf(roll * 2.0f) & 0x3FF) << 10) |
                         (row << 20) | ((inWindow ? 1 : 0) << 22);
    if (horizSig != sHorizSig) {
        sHorizSig = horizSig;
        drawMountedHorizon(pitch, roll, row, inWindow);
    }

    // --- Motion bar: empty at the stillness limit, full at 5x it ------------
    // The cause sits inside the bar. If it won't empty with the scooter on a
    // stool, this names the sensor whose calibration is off: "turning" is
    // the gyro, the other two the accelerometer.
    const int barIn = CAL_BAR_W - 2;
    int motionPx = 0;
    if (pkt.motion > CAL_MOTION_AT_LIMIT) {
        motionPx = (int)(pkt.motion - CAL_MOTION_AT_LIMIT) * barIn / (255 - CAL_MOTION_AT_LIMIT);
        if (motionPx < 2) motionPx = 2;   // over the limit must be visible
    }
    const char* why = motionPx == 0 ? "still"
                    : pkt.motion_term == (uint8_t)CalMotionTerm::GYRO      ? "turning"
                    : pkt.motion_term == (uint8_t)CalMotionTerm::ACCEL_MAG ? "pushed"
                                                                           : "shaking";
    drawCalBar(sMotion, CAL_BAR_X, 101, CAL_BAR_W, CAL_BAR_H, motionPx, COLOR_RED, why,
               motionPx == 0 ? COLOR_GREEN : COLOR_WHITE);

    // --- Hold bar: settle, then capture -------------------------------------
    const int pct = pkt.dwell_pct > 100 ? 100 : pkt.dwell_pct;
    drawCalBar(sHold, CAL_BAR_X, 125, CAL_BAR_W, CAL_BAR_H, pct * barIn / 100,
               pct >= 100 ? COLOR_GREEN : COLOR_CYAN);

    // --- Readout: heading, pitch, roll --------------------------------------
    // Heading truncated with (int), matching the sector arithmetic; a rounded
    // 360 would name a column that is not there.
    char rbuf[20];
    snprintf(rbuf, sizeof(rbuf), "%03d\xF7 P%+03d R%+03d", (int)hdg,
             (int)lroundf(pitch), (int)lroundf(roll));
    if (strcmp(rbuf, sReadout) != 0) {
        strncpy(sReadout, rbuf, sizeof(sReadout) - 1);
        sReadout[sizeof(sReadout) - 1] = '\0';
        char padded[18];
        snprintf(padded, sizeof(padded), "%-17s", rbuf);   // a 3-digit roll is one wider
        drawWorkText(MC_PANEL_X, 152, padded, 2, COLOR_WHITE);
    }

    // --- Status and keys ----------------------------------------------------
    uint16_t statusColor;
    const char* status = mountedCalInstruction(pkt, row, statusColor);
    drawCollectingFooter(pkt, finishing, { status, statusColor });
}

// ---------------------------------------------------------------------------
// 1 Baseline, before its in-session bootstrap locks (CalPhase::ROUGH_SCAN):
// per-axis raw-range bars. A few seconds of tumbling, normally, and then
// showCalGrid() takes over. If the lock never comes (a wrong
// MAG_CAL_EXPECTED_FIELD_UT for this site, or spinning about one axis only)
// this stays up, and the session still uploads for grading as before.
//
// Work area, under the frame's header (counter = samples of 2160, which
// carries straight over to the grid at lock):
//   y  34, 62, 90  X, Y and Z bars, 292 x 22 from x 24
//   y 126          "Finding north..."; under it, that the grid will follow
//
// The 2-D "Fit:" readout that used to sit here is gone: it settles 20-30 deg
// wrong on good data and would read like a grade for the grid.
// ---------------------------------------------------------------------------
void showBaselineRoughScan(const CalProgressPacket& pkt, bool finishing) {
    if (!tftReady) return;
    static CalBarCache sBar[3];

    char counter[12];
    snprintf(counter, sizeof(counter), "%u/%d", (unsigned)pkt.sample_count, MAG_CAL_BIN_MAX_SAMPLES);
    drawCalHeader(CalStep::BASELINE, counter);
    if (claimCalWork(calSig(CalWork::BARS))) {
        static const char* const kAxes[3] = { "X", "Y", "Z" };
        for (int i = 0; i < 3; i++) drawWorkText(4, 34 + i * 28 + 4, kAxes[i], 2, COLOR_GRAY);
        drawWorkText(4, 126, "Finding north...", 2, COLOR_WHITE);
        drawWorkText(4, 150, "The grid appears", 2, COLOR_GRAY);
        drawWorkText(4, 166, "once it locks.", 2, COLOR_GRAY);
        for (auto& b : sBar) b = CalBarCache{};
    }

    const uint8_t cov[3] = { pkt.cov_x, pkt.cov_y, pkt.cov_z };
    for (int i = 0; i < 3; i++) {
        const int c = cov[i] > 100 ? 100 : cov[i];
        const uint16_t color = (c >= 80) ? COLOR_GREEN : (c >= 40) ? COLOR_YELLOW : COLOR_RED;
        drawCalBar(sBar[i], 24, 34 + i * 28, 292, 22, 290 * c / 100, color);
    }

    static const CalText kTumble = { "Tumble the unit randomly", COLOR_YELLOW };
    drawCollectingFooter(pkt, finishing, kTumble);
}

}  // namespace display
