#include "display.h"
#include "../board_pins.h"
#include "../config.h"
#include <dpvlink.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1351.h>
#include <SPI.h>
#include <Arduino.h>
#include <math.h>

// Display dimensions
static constexpr int SCREEN_WIDTH  = 128;
static constexpr int SCREEN_HEIGHT = 96;

// 16-bit 565 colors
static constexpr uint16_t COLOR_BLACK  = 0x0000;
static constexpr uint16_t COLOR_WHITE  = 0xFFFF;
static constexpr uint16_t COLOR_RED    = 0xF800;
static constexpr uint16_t COLOR_GREEN  = 0x07E0;
static constexpr uint16_t COLOR_BLUE   = 0x001F;
static constexpr uint16_t COLOR_CYAN   = 0x07FF;
static constexpr uint16_t COLOR_YELLOW = 0xFFE0;
static constexpr uint16_t COLOR_GRAY   = 0x7BEF;

#define SPI_CLOCK_HZ 250000  // 1 MHz SPI clock (default 8 MHz causes glitches)

// Hardware SPI — uses ESP32 VSPI peripheral
static Adafruit_SSD1351 oled(SCREEN_WIDTH, SCREEN_HEIGHT,
                             &SPI, OLED_CS, OLED_DC, OLED_RST);

// Boot status line Y cursor
static int bootLineY = 0;

// ---------------------------------------------------------------------------
// Nav mode layout constants
// ---------------------------------------------------------------------------
static constexpr int STATUS_BAR_H  = 12;
static constexpr int DIV_Y_TOP     = 12;
static constexpr int DIV_Y_MID     = 55;
static constexpr int DIV_X         = 63;

static constexpr int CELL_UL_Y     = 13;
static constexpr int CELL_UR_Y     = 13;
static constexpr int CELL_LL_Y     = 56;
static constexpr int CELL_LR_Y     = 56;

// ---------------------------------------------------------------------------
// Nav mode cache
// ---------------------------------------------------------------------------
static bool    nav_static_drawn = false;
static uint8_t nav_frame = 0;
static char    prev_hdg_buf[8]    = "";   // "063M" fixed 4 chars
static char    prev_brg_buf[8]    = "";   // "180M" or "---" fixed 4 chars
static char    prev_rng_buf[8]    = "";   // fixed-width padded
static char    prev_spd_buf[8]    = "";   // fixed-width padded
static char    prev_spd_meta[8]   = "";   // "m/m GPS" or "ft/m FLW"
// Status bar: cache each indicator separately
static uint8_t prev_st_state = 0xFF;
static uint8_t prev_st_gps   = 0xFF;
static uint8_t prev_st_home  = 0xFF;

// ---------------------------------------------------------------------------
// Debug mode cache
// ---------------------------------------------------------------------------
static bool    debug_static_drawn = false;
static uint8_t debug_frame = 0;
static char    prev_dbg_mag[24]   = "";
static char    prev_dbg_magm[24]  = "";
static char    prev_dbg_acc[24]   = "";
static char    prev_dbg_accm[24]  = "";
static char    prev_dbg_gyr[24]   = "";
static char    prev_dbg_fhdg[24]  = "";
static char    prev_dbg_mhdg[24]  = "";
static char    prev_dbg_pit[12]   = "";
static char    prev_dbg_rol[12]   = "";

namespace display {

// ===========================================================================
// Core API
// ===========================================================================

bool init() {
    SPI.begin(DISP_SCK, DISP_MISO, DISP_MOSI);
    Serial.println("Initializing OLED display (hardware SPI)...");
    oled.begin(SPI_CLOCK_HZ);
    oled.fillScreen(COLOR_BLACK);
    Serial.println("OLED display initialized (hardware SPI)");
    return true;
}

void selfTest() {
    Serial.println("Display self-test: color cycle...");
    oled.fillScreen(COLOR_RED);   delay(1000);
    oled.fillScreen(COLOR_GREEN); delay(1000);
    oled.fillScreen(COLOR_BLUE);  delay(1000);
    oled.fillScreen(COLOR_BLACK); delay(1000);

    oled.setTextColor(COLOR_WHITE);
    oled.setTextSize(2);
    oled.setCursor(10, 20);
    oled.println("DPV-Nav");

    oled.setTextSize(1);
    oled.setTextColor(COLOR_CYAN);
    oled.setCursor(10, 50);
    oled.println("Display OK");
    Serial.println("Display self-test complete");
}

void invalidateCache() {
    nav_static_drawn = false;
    nav_frame = 0;
    prev_hdg_buf[0] = '\0';
    prev_brg_buf[0] = '\0';
    prev_rng_buf[0] = '\0';
    prev_spd_buf[0] = '\0';
    prev_spd_meta[0] = '\0';
    prev_st_state = 0xFF;
    prev_st_gps   = 0xFF;
    prev_st_home  = 0xFF;

    debug_static_drawn = false;
    debug_frame = 0;
    prev_dbg_mag[0]  = '\0';
    prev_dbg_magm[0] = '\0';
    prev_dbg_acc[0]  = '\0';
    prev_dbg_accm[0] = '\0';
    prev_dbg_gyr[0]  = '\0';
    prev_dbg_fhdg[0] = '\0';
    prev_dbg_mhdg[0] = '\0';
    prev_dbg_pit[0]  = '\0';
    prev_dbg_rol[0]  = '\0';
}

void clear() {
    oled.fillScreen(COLOR_BLACK);
    bootLineY = 0;
    invalidateCache();
}

void reinit() {
    oled.begin(SPI_CLOCK_HZ);
    invalidateCache();
    Serial.println("[DISP] reinit complete");
}

void statusLine(const char* label, bool ok) {
    oled.setTextSize(1);
    oled.setCursor(0, bootLineY);
    oled.setTextColor(COLOR_WHITE, COLOR_BLACK);
    oled.print(label);
    int labelLen = strlen(label);
    for (int i = labelLen; i < 17; i++) oled.print('.');
    if (ok) {
        oled.setTextColor(COLOR_GREEN, COLOR_BLACK);
        oled.print("ok");
    } else {
        oled.setTextColor(COLOR_RED, COLOR_BLACK);
        oled.print("FAIL");
    }
    bootLineY += 9;
}

void drawPixel(int x, int y, uint16_t color) {
    oled.drawPixel(x, y, color);
}

void drawText(int x, int y, const char* text, uint16_t color, uint8_t size) {
    oled.setTextSize(size);
    oled.setTextColor(color, COLOR_BLACK);
    oled.setCursor(x, y);
    oled.print(text);
}

// ===========================================================================
// Nav mode internals
//
// KEY PRINCIPLE: Never use fillRect to clear areas before drawing text.
// Instead, use opaque text (setTextColor with bg=BLACK) and pad all strings
// to fixed width.  This means the GFX library only pushes pixels for the
// character bounding boxes — far fewer SPI bytes than a fillRect.
// ===========================================================================

// Draw the static grid lines and cell labels (called once after clear/reinit)
static void drawNavStatic() {
    // Horizontal dividers
    oled.drawFastHLine(0, DIV_Y_TOP, SCREEN_WIDTH, COLOR_CYAN);
    oled.drawFastHLine(0, DIV_Y_MID, SCREEN_WIDTH, COLOR_CYAN);

    // Vertical divider
    oled.drawFastVLine(DIV_X, DIV_Y_TOP + 1, SCREEN_HEIGHT - DIV_Y_TOP - 1, COLOR_CYAN);

    // Cell labels (size 1, cyan, opaque)
    oled.setTextSize(1);
    oled.setTextColor(COLOR_CYAN, COLOR_BLACK);

    oled.setCursor(1, CELL_UL_Y + 1);  oled.print("HDG");
    oled.setCursor(65, CELL_UR_Y + 1); oled.print("RNG");
    oled.setCursor(1, CELL_LL_Y + 1);  oled.print("BRG");
    oled.setCursor(65, CELL_LR_Y + 1); oled.print("SPD");

    nav_static_drawn = true;
}

// Status bar: draw each indicator independently, skip unchanged ones.
// Each indicator is a small fixed-width opaque text — no fillRect needed.
static void updateStatusBar(const NavPacket& pkt) {
    oled.setTextSize(1);

    // System state (3 chars at x=0)
    if (pkt.system_state != prev_st_state) {
        prev_st_state = pkt.system_state;
        const char* s;
        uint16_t c;
        switch (pkt.system_state) {
            case 2: s = "RDY"; c = COLOR_CYAN;   break;
            case 3: s = "NAV"; c = COLOR_GREEN;  break;
            case 4: s = "CAL"; c = COLOR_YELLOW; break;
            case 5: s = "ERR"; c = COLOR_RED;    break;
            default: s = "..."; c = COLOR_GRAY;  break;
        }
        oled.setTextColor(c, COLOR_BLACK);
        oled.setCursor(0, 2);
        oled.print(s);
    }

    // GPS (5 chars at x=30)
    if (pkt.gps_fix_quality != prev_st_gps) {
        prev_st_gps = pkt.gps_fix_quality;
        const char* s;
        uint16_t c;
        if (pkt.gps_fix_quality >= 2) {
            s = "GP:DG"; c = COLOR_GREEN;
        } else if (pkt.gps_fix_quality == 1) {
            s = "GP:OK"; c = COLOR_GREEN;
        } else {
            s = "GP:--"; c = COLOR_RED;
        }
        oled.setTextColor(c, COLOR_BLACK);
        oled.setCursor(30, 2);
        oled.print(s);
    }

    // Home (6 chars at x=72)
    uint8_t homeFlag = (pkt.flags & FLAG_HAS_HOME) ? 1 : 0;
    if (homeFlag != prev_st_home) {
        prev_st_home = homeFlag;
        if (homeFlag) {
            oled.setTextColor(COLOR_GREEN, COLOR_BLACK);
            oled.setCursor(72, 2);
            oled.print("HM:SET");
        } else {
            oled.setTextColor(COLOR_GRAY, COLOR_BLACK);
            oled.setCursor(72, 2);
            oled.print("HM:---");
        }
    }
}

// Heading cell (upper-left).  "063M" as fixed-width string.
// Size 3 heading (3 chars = 54px) + size 2 suffix (1 char = 12px) = 66px.
// To keep SPI minimal, combine heading+suffix into one cached string and
// only issue draws when the formatted result changes.
static void updateHeading(const NavPacket& pkt) {
    int hdg_int = (int)(pkt.heading_deg + 0.5f) % 360;
    char suffix = (pkt.flags & FLAG_TRUE_HEADING) ? 'T' : 'M';
    char buf[8];
    snprintf(buf, sizeof(buf), "%03d%c", hdg_int, suffix);

    if (strcmp(buf, prev_hdg_buf) == 0) return;

    // Only redraw the digits if they changed (first 3 chars)
    if (prev_hdg_buf[0] == '\0' ||
        buf[0] != prev_hdg_buf[0] || buf[1] != prev_hdg_buf[1] || buf[2] != prev_hdg_buf[2]) {
        char digits[4] = { buf[0], buf[1], buf[2], '\0' };
        oled.setTextSize(3);
        oled.setTextColor(COLOR_WHITE, COLOR_BLACK);
        oled.setCursor(1, 25);
        oled.print(digits);
    }

    // Only redraw suffix if it changed
    if (prev_hdg_buf[0] == '\0' || buf[3] != prev_hdg_buf[3]) {
        oled.setTextSize(2);
        oled.setTextColor(COLOR_CYAN, COLOR_BLACK);
        oled.setCursor(55, 27);
        char s[2] = { buf[3], '\0' };
        oled.print(s);
    }

    strncpy(prev_hdg_buf, buf, sizeof(prev_hdg_buf));
}

// Bearing cell (lower-left).  "180M" or "---" padded to 4 chars.
static void updateBearing(const NavPacket& pkt) {
    char buf[8];
    if (pkt.flags & FLAG_HAS_HOME) {
        int brg_int = (int)(pkt.bearing_home_deg + 0.5f) % 360;
        char suffix = (pkt.flags & FLAG_TRUE_HEADING) ? 'T' : 'M';
        snprintf(buf, sizeof(buf), "%03d%c", brg_int, suffix);
    } else {
        snprintf(buf, sizeof(buf), "--- ");
    }

    if (strcmp(buf, prev_brg_buf) == 0) return;

    // Digits (size 2, 3 chars)
    if (prev_brg_buf[0] == '\0' ||
        buf[0] != prev_brg_buf[0] || buf[1] != prev_brg_buf[1] || buf[2] != prev_brg_buf[2]) {
        char digits[4] = { buf[0], buf[1], buf[2], '\0' };
        oled.setTextSize(2);
        oled.setTextColor(COLOR_WHITE, COLOR_BLACK);
        oled.setCursor(1, 68);
        oled.print(digits);
    }

    // Suffix (size 1, 1 char)
    if (prev_brg_buf[0] == '\0' || buf[3] != prev_brg_buf[3]) {
        char s[2] = { buf[3], '\0' };
        oled.setTextSize(1);
        oled.setTextColor(COLOR_CYAN, COLOR_BLACK);
        oled.setCursor(38, 72);
        oled.print(s);
    }

    strncpy(prev_brg_buf, buf, sizeof(prev_brg_buf));
}

// Range cell (upper-right).  Fixed-width padded string, opaque text.
static void updateRange(const NavPacket& pkt) {
    char buf[8];

    if (pkt.flags & FLAG_HAS_HOME) {
        float dist = pkt.distance_home_m;
#if DISPLAY_UNITS_IMPERIAL
        dist *= 3.28084f;
        if (dist < 1000.0f) {
            snprintf(buf, sizeof(buf), "%4dft", (int)(dist + 0.5f));
        } else {
            snprintf(buf, sizeof(buf), "%4.1fk", (double)(dist / 1000.0f));
        }
#else
        if (dist < 1000.0f) {
            snprintf(buf, sizeof(buf), "%4dm ", (int)(dist + 0.5f));
        } else {
            snprintf(buf, sizeof(buf), "%4.1fk", (double)(dist / 1000.0f));
        }
#endif
    } else {
        snprintf(buf, sizeof(buf), " --- ");
    }

    if (strcmp(buf, prev_rng_buf) == 0) return;
    strncpy(prev_rng_buf, buf, sizeof(prev_rng_buf));

    // Opaque text overwrites old characters — no fillRect needed
    oled.setTextSize(2);
    oled.setTextColor(COLOR_WHITE, COLOR_BLACK);
    oled.setCursor(65, 25);
    oled.print(buf);
}

// Speed cell (lower-right).  Value + meta line.
static void updateSpeed(const NavPacket& pkt) {
    // Speed value — padded to 4 chars at size 2
    char buf[8];
#if DISPLAY_UNITS_IMPERIAL
    int spd_display = (int)(pkt.speed_ms * 60.0f * 3.28084f + 0.5f);
    snprintf(buf, sizeof(buf), "%4d", spd_display);
#else
    int spd_display = (int)(pkt.speed_ms * 60.0f + 0.5f);
    snprintf(buf, sizeof(buf), "%4d", spd_display);
#endif

    if (strcmp(buf, prev_spd_buf) != 0) {
        strncpy(prev_spd_buf, buf, sizeof(prev_spd_buf));
        oled.setTextSize(2);
        oled.setTextColor(COLOR_WHITE, COLOR_BLACK);
        oled.setCursor(65, 68);
        oled.print(buf);
    }

    // Meta line: "m/m GPS" or "ft/m FLW" — fixed 7 chars at size 1
    const char* src = (pkt.flags & FLAG_GPS_SPEED) ? "GPS" : "FLW";
#if DISPLAY_UNITS_IMPERIAL
    char meta[8];
    snprintf(meta, sizeof(meta), "ft/m%s", src);
#else
    char meta[8];
    snprintf(meta, sizeof(meta), "m/m %s", src);
#endif

    if (strcmp(meta, prev_spd_meta) != 0) {
        strncpy(prev_spd_meta, meta, sizeof(prev_spd_meta));
        oled.setTextSize(1);
        oled.setTextColor(COLOR_CYAN, COLOR_BLACK);
        oled.setCursor(65, 87);
        oled.print(meta);
    }
}

void showNav(const NavPacket& pkt) {
    if (!nav_static_drawn) {
        drawNavStatic();
        return;
    }

    // 5-slot frame rotation — only ONE region per call
    uint8_t slot = nav_frame % 5;
    switch (slot) {
        case 0: updateHeading(pkt); break;
        case 1: updateBearing(pkt); break;
        case 2: updateRange(pkt);   break;
        case 3: updateSpeed(pkt);   break;
        case 4: updateStatusBar(pkt); break;
    }
    nav_frame++;
}

// ===========================================================================
// Debug mode internals
//
// Same principle: opaque text, fixed-width strings, no fillRect.
// ===========================================================================

static void drawDebugStatic() {
    oled.setTextSize(1);
    oled.setTextColor(COLOR_CYAN, COLOR_BLACK);
    oled.setCursor(0, 0);  oled.print("MAG");
    oled.setCursor(0, 18); oled.print("ACC");
    oled.setCursor(0, 36); oled.print("GYR");
    oled.setCursor(0, 54); oled.print("AHRS HDG:");
    oled.setCursor(0, 63); oled.print("MAG  HDG:");
    oled.setCursor(0, 72); oled.print("P:");
    oled.setCursor(60, 72); oled.print("R:");
    debug_static_drawn = true;
}

// Helper: draw fixed-width opaque text only if string changed.
// No fillRect — the opaque background on each character erases old content.
static void drawIfChanged(int x, int y, const char* buf, char* prev, size_t prevSize,
                           uint16_t color) {
    if (strcmp(buf, prev) == 0) return;
    strncpy(prev, buf, prevSize);
    oled.setTextSize(1);
    oled.setTextColor(color, COLOR_BLACK);
    oled.setCursor(x, y);
    oled.print(buf);
}

void showDebug(const DebugPacket& pkt) {
    if (!debug_static_drawn) {
        drawDebugStatic();
        return;
    }

    uint8_t slot = debug_frame % 5;
    char buf[24];

    switch (slot) {
        case 0: {
            // Mag XYZ — fixed width 18 chars
            snprintf(buf, sizeof(buf), "%6.1f %6.1f %6.1f",
                     (double)pkt.mag_x, (double)pkt.mag_y, (double)pkt.mag_z);
            drawIfChanged(24, 0, buf, prev_dbg_mag, sizeof(prev_dbg_mag), COLOR_WHITE);
            break;
        }
        case 1: {
            // Mag magnitude
            float magMag = sqrtf(pkt.mag_x * pkt.mag_x + pkt.mag_y * pkt.mag_y + pkt.mag_z * pkt.mag_z);
            snprintf(buf, sizeof(buf), "|M|=%6.1f uT  ", (double)magMag);
            drawIfChanged(24, 9, buf, prev_dbg_magm, sizeof(prev_dbg_magm), COLOR_GRAY);
            // Accel XYZ — fixed width
            snprintf(buf, sizeof(buf), "%5.2f %5.2f %5.2f",
                     (double)pkt.accel_x, (double)pkt.accel_y, (double)pkt.accel_z);
            drawIfChanged(24, 18, buf, prev_dbg_acc, sizeof(prev_dbg_acc), COLOR_WHITE);
            break;
        }
        case 2: {
            // Accel magnitude
            float accMag = sqrtf(pkt.accel_x * pkt.accel_x + pkt.accel_y * pkt.accel_y + pkt.accel_z * pkt.accel_z);
            snprintf(buf, sizeof(buf), "|A|=%5.2f g    ", (double)accMag);
            drawIfChanged(24, 27, buf, prev_dbg_accm, sizeof(prev_dbg_accm), COLOR_GRAY);
            // Gyro XYZ — fixed width
            snprintf(buf, sizeof(buf), "%6.3f %6.3f %6.3f",
                     (double)pkt.gyro_x, (double)pkt.gyro_y, (double)pkt.gyro_z);
            drawIfChanged(24, 36, buf, prev_dbg_gyr, sizeof(prev_dbg_gyr), COLOR_WHITE);
            break;
        }
        case 3: {
            // Fused heading — fixed 5 chars
            snprintf(buf, sizeof(buf), "%5.1f ", (double)pkt.fused_heading_deg);
            drawIfChanged(60, 54, buf, prev_dbg_fhdg, sizeof(prev_dbg_fhdg), COLOR_GREEN);
            // Raw mag heading — fixed 5 chars
            snprintf(buf, sizeof(buf), "%5.1f ", (double)pkt.raw_mag_heading_deg);
            drawIfChanged(60, 63, buf, prev_dbg_mhdg, sizeof(prev_dbg_mhdg), COLOR_YELLOW);
            break;
        }
        case 4: {
            // Pitch — fixed 6 chars
            snprintf(buf, sizeof(buf), "%6.1f", (double)pkt.pitch_deg);
            drawIfChanged(12, 72, buf, prev_dbg_pit, sizeof(prev_dbg_pit), COLOR_WHITE);
            // Roll — fixed 6 chars (now properly cached)
            snprintf(buf, sizeof(buf), "%6.1f", (double)pkt.roll_deg);
            drawIfChanged(72, 72, buf, prev_dbg_rol, sizeof(prev_dbg_rol), COLOR_WHITE);
            break;
        }
    }
    debug_frame++;
}

}  // namespace display
