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

#define SPI_CLOCK_HZ 1000000  // 2 MHz SPI clock

// Hardware SPI — uses ESP32 VSPI peripheral
static Adafruit_SSD1351 oled(SCREEN_WIDTH, SCREEN_HEIGHT,
                             &SPI, OLED_CS, OLED_DC, OLED_RST);

// Offscreen framebuffer — all drawing targets this, single SPI push per frame
static GFXcanvas16 canvas(SCREEN_WIDTH, SCREEN_HEIGHT);  // 24,576 bytes
static bool canvasReady = false;

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

namespace display {

// ===========================================================================
// Core API
// ===========================================================================

bool init() {
    // Hardware reset pulse
    pinMode(OLED_RST, OUTPUT);
    digitalWrite(OLED_RST, LOW);
    delay(10);
    digitalWrite(OLED_RST, HIGH);
    delay(10);

    // Initialize SPI and OLED controller
    SPI.begin(DISP_SCK, DISP_MISO, DISP_MOSI);
    Serial.println("Initializing OLED display (hardware SPI)...");
    oled.begin(SPI_CLOCK_HZ);
    delay(50);
    oled.fillScreen(COLOR_BLACK);
    delay(50);

    // Verify canvas allocation
    if (canvas.getBuffer() == nullptr) {
        Serial.println("ERROR: GFXcanvas16 allocation failed (24KB)");
        canvasReady = false;
        return false;
    }
    canvasReady = true;
    canvas.fillScreen(COLOR_BLACK);

    Serial.println("OLED display initialized (hardware SPI, canvas framebuffer)");
    return true;
}

void flush() {
    if (!canvasReady) return;
    oled.drawRGBBitmap(0, 0, canvas.getBuffer(), SCREEN_WIDTH, SCREEN_HEIGHT);
}

void selfTest() {
    // Boot-time self-test draws directly to oled for immediate visibility
    Serial.println("Display self-test: color cycle...");
    oled.fillScreen(COLOR_RED);   delay(1000);
    oled.fillScreen(COLOR_GREEN); delay(1000);
    oled.fillScreen(COLOR_BLUE);  delay(1000);
    oled.fillScreen(COLOR_BLACK); delay(1000);

    oled.setTextColor(COLOR_WHITE);
    oled.setTextSize(2);
    oled.setCursor(10, 20);
    oled.println("DPV-Nav");
    delay(50);

    oled.setTextSize(1);
    oled.setTextColor(COLOR_CYAN);
    oled.setCursor(10, 50);
    oled.println("Display OK");
    delay(50);
    Serial.println("Display self-test complete");
}

void invalidateCache() {
    // No-op: canvas is redrawn from scratch every frame
}

void clear() {
    if (canvasReady) {
        canvas.fillScreen(COLOR_BLACK);
    }
    oled.fillScreen(COLOR_BLACK);
    delay(50);
    bootLineY = 0;
}

void reinit() {
    oled.begin(SPI_CLOCK_HZ);
    delay(50);
    // Re-push existing canvas content to restore display
    if (canvasReady) {
        flush();
    }
    Serial.println("[DISP] reinit complete");
}

void statusLine(const char* label, bool ok) {
    // Boot-time function — draws directly to oled
    oled.setTextSize(1);
    oled.setCursor(0, bootLineY);
    oled.setTextColor(COLOR_WHITE, COLOR_BLACK);
    oled.print(label);
    delay(50);
    int labelLen = strlen(label);
    for (int i = labelLen; i < 17; i++) { oled.print('.'); delay(50); }
    if (ok) {
        oled.setTextColor(COLOR_GREEN, COLOR_BLACK);
        oled.print("ok");
    } else {
        oled.setTextColor(COLOR_RED, COLOR_BLACK);
        oled.print("FAIL");
    }
    delay(50);
    bootLineY += 9;
}

// ---------------------------------------------------------------------------
// Drawing primitives — target canvas (RAM), no SPI, no delays
// ---------------------------------------------------------------------------

void drawPixel(int x, int y, uint16_t color) {
    canvas.drawPixel(x, y, color);
}

void drawText(int x, int y, const char* text, uint16_t color, uint8_t size) {
    canvas.setTextSize(size);
    canvas.setTextColor(color, COLOR_BLACK);
    canvas.setCursor(x, y);
    canvas.print(text);
}

void fillRect(int x, int y, int w, int h, uint16_t color) {
    canvas.fillRect(x, y, w, h, color);
}

void drawHLine(int x, int y, int w, uint16_t color) {
    canvas.drawFastHLine(x, y, w, color);
}

// ===========================================================================
// Nav mode internals — full-frame redraw to canvas each call
// ===========================================================================

static void drawStatusBar(const NavPacket& pkt) {
    canvas.setTextSize(1);

    // Layout: BATT  CAL  WiFi  GPS  P  S
    // 6px per char at size 1.  Positions chosen for even spacing.

    // BATT (stubbed — no battery data yet)
    canvas.setTextColor(COLOR_GRAY, COLOR_BLACK);
    canvas.setCursor(0, 2);
    canvas.print("BATT");

    // CAL (stubbed — no calibration quality metric yet)
    canvas.setTextColor(COLOR_GRAY, COLOR_BLACK);
    canvas.setCursor(27, 2);
    canvas.print("CAL");

    // WiFi: white = enabled, gray = disabled
    uint16_t wifiColor = (pkt.flags & FLAG_WIFI_ENABLED) ? COLOR_WHITE : COLOR_GRAY;
    canvas.setTextColor(wifiColor, COLOR_BLACK);
    canvas.setCursor(48, 2);
    canvas.print("WiFi");

    // GPS: quality indicator (red/yellow/green)
    uint16_t gpsColor;
    if (pkt.gps_fix_quality >= 2) {
        gpsColor = COLOR_GREEN;   // DGPS
    } else if (pkt.gps_fix_quality == 1) {
        gpsColor = COLOR_YELLOW;  // GPS fix, no DGPS
    } else {
        gpsColor = COLOR_RED;     // no fix
    }
    canvas.setTextColor(gpsColor, COLOR_BLACK);
    canvas.setCursor(75, 2);
    canvas.print("GPS");

    // P: GPS position enabled — white = enabled, gray = disabled
    uint16_t posColor = (pkt.flags & FLAG_GPS_POS_ENABLED) ? COLOR_WHITE : COLOR_GRAY;
    canvas.setTextColor(posColor, COLOR_BLACK);
    canvas.setCursor(99, 2);
    canvas.print("P");

    // S: GPS speed enabled — white = enabled, gray = disabled
    uint16_t spdColor = (pkt.flags & FLAG_GPS_SPD_ENABLED) ? COLOR_WHITE : COLOR_GRAY;
    canvas.setTextColor(spdColor, COLOR_BLACK);
    canvas.setCursor(111, 2);
    canvas.print("S");
}

static void drawNavGrid() {
    // Horizontal dividers
    canvas.drawFastHLine(0, DIV_Y_TOP, SCREEN_WIDTH, COLOR_CYAN);
    canvas.drawFastHLine(0, DIV_Y_MID, SCREEN_WIDTH, COLOR_CYAN);
    // Vertical divider
    canvas.drawFastVLine(DIV_X, DIV_Y_TOP + 1, SCREEN_HEIGHT - DIV_Y_TOP - 1, COLOR_CYAN);
    // Cell labels
    canvas.setTextSize(1);
    canvas.setTextColor(COLOR_CYAN, COLOR_BLACK);
    canvas.setCursor(1, CELL_UL_Y + 1);  canvas.print("BRG");
    canvas.setCursor(65, CELL_UR_Y + 1); canvas.print("RNG");
    canvas.setCursor(1, CELL_LL_Y + 1);  canvas.print("HDG");
    canvas.setCursor(65, CELL_LR_Y + 1); canvas.print("SPD");
}

static void drawBearing(const NavPacket& pkt) {
    if (pkt.flags & FLAG_HAS_HOME) {
        int brg_int = (int)(pkt.bearing_home_deg + 0.5f) % 360;
        char suffix = (pkt.flags & FLAG_TRUE_HEADING) ? 'T' : 'M';

        // Digits (size 3)
        char digits[4];
        snprintf(digits, sizeof(digits), "%03d", brg_int);
        canvas.setTextSize(2);
        canvas.setTextColor(COLOR_WHITE, COLOR_BLACK);
        canvas.setCursor(1, 25);
        canvas.print(digits);

        // Suffix (size 2)
        char s[2] = { suffix, '\0' };
        canvas.setTextSize(2);
        canvas.setTextColor(COLOR_CYAN, COLOR_BLACK);
        canvas.setCursor(55, 27);
        canvas.print(s);
    } else {
        canvas.setTextSize(3);
        canvas.setTextColor(COLOR_WHITE, COLOR_BLACK);
        canvas.setCursor(1, 25);
        canvas.print("---");

        canvas.setTextSize(2);
        canvas.setTextColor(COLOR_CYAN, COLOR_BLACK);
        canvas.setCursor(55, 27);
        canvas.print(" ");
    }
}

static void drawRange(const NavPacket& pkt) {
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

    canvas.setTextSize(2);
    canvas.setTextColor(COLOR_WHITE, COLOR_BLACK);
    canvas.setCursor(65, 25);
    canvas.print(buf);
}

static void drawHeading(const NavPacket& pkt) {
    int hdg_int = (int)(pkt.heading_deg + 0.5f) % 360;
    char suffix = (pkt.flags & FLAG_TRUE_HEADING) ? 'T' : 'M';

    // Digits (size 2)
    char digits[4];
    snprintf(digits, sizeof(digits), "%03d", hdg_int);
    canvas.setTextSize(2);
    canvas.setTextColor(COLOR_WHITE, COLOR_BLACK);
    canvas.setCursor(1, 68);
    canvas.print(digits);

    // Suffix (size 1)
    char s[2] = { suffix, '\0' };
    canvas.setTextSize(1);
    canvas.setTextColor(COLOR_CYAN, COLOR_BLACK);
    canvas.setCursor(38, 72);
    canvas.print(s);
}

static void drawSpeed(const NavPacket& pkt) {
    // Speed value
    char buf[8];
#if DISPLAY_UNITS_IMPERIAL
    int spd_display = (int)(pkt.speed_ms * 60.0f * 3.28084f + 0.5f);
    snprintf(buf, sizeof(buf), "%4d", spd_display);
#else
    int spd_display = (int)(pkt.speed_ms * 60.0f + 0.5f);
    snprintf(buf, sizeof(buf), "%4d", spd_display);
#endif
    canvas.setTextSize(2);
    canvas.setTextColor(COLOR_WHITE, COLOR_BLACK);
    canvas.setCursor(65, 68);
    canvas.print(buf);

    // Meta line: "m/m GPS" or "ft/m FLW"
    const char* src = (pkt.flags & FLAG_GPS_SPEED) ? "GPS" : "FLW";
#if DISPLAY_UNITS_IMPERIAL
    char meta[8];
    snprintf(meta, sizeof(meta), "ft/m%s", src);
#else
    char meta[8];
    snprintf(meta, sizeof(meta), "m/m %s", src);
#endif
    canvas.setTextSize(1);
    canvas.setTextColor(COLOR_CYAN, COLOR_BLACK);
    canvas.setCursor(65, 87);
    canvas.print(meta);
}

void showNav(const NavPacket& pkt) {
    if (!canvasReady) return;

    canvas.fillScreen(COLOR_BLACK);
    drawStatusBar(pkt);
    drawNavGrid();
    drawBearing(pkt);
    drawRange(pkt);
    drawHeading(pkt);
    drawSpeed(pkt);
    flush();
}

void showNavTop(const NavPacket& pkt) {
    if (!canvasReady) return;

    // Clear top portion only (status bar + top cells)
    canvas.fillRect(0, 0, SCREEN_WIDTH, DIV_Y_MID, COLOR_BLACK);

    drawStatusBar(pkt);
    canvas.drawFastHLine(0, DIV_Y_TOP, SCREEN_WIDTH, COLOR_CYAN);

    // Top row labels
    canvas.setTextSize(1);
    canvas.setTextColor(COLOR_CYAN, COLOR_BLACK);
    canvas.setCursor(1, CELL_UL_Y + 1);  canvas.print("BRG");
    canvas.setCursor(65, CELL_UR_Y + 1); canvas.print("RNG");

    drawBearing(pkt);
    drawRange(pkt);

    // Do NOT flush — caller will add menu content then call flush()
}

// ===========================================================================
// Debug mode — full-frame redraw to canvas
// ===========================================================================

void showDebug(const DebugPacket& pkt) {
    if (!canvasReady) return;

    canvas.fillScreen(COLOR_BLACK);
    char buf[24];

    // Static labels
    canvas.setTextSize(1);
    canvas.setTextColor(COLOR_CYAN, COLOR_BLACK);
    canvas.setCursor(0, 0);  canvas.print("MAG");
    canvas.setCursor(0, 18); canvas.print("ACC");
    canvas.setCursor(0, 36); canvas.print("GYR");
    canvas.setCursor(0, 54); canvas.print("AHRS HDG:");
    canvas.setCursor(0, 63); canvas.print("MAG  HDG:");
    canvas.setCursor(0, 72); canvas.print("P:");
    canvas.setCursor(60, 72); canvas.print("R:");

    // Mag XYZ
    snprintf(buf, sizeof(buf), "%6.1f %6.1f %6.1f",
             (double)pkt.mag_x, (double)pkt.mag_y, (double)pkt.mag_z);
    canvas.setTextColor(COLOR_WHITE, COLOR_BLACK);
    canvas.setCursor(24, 0);
    canvas.print(buf);

    // Mag magnitude
    float magMag = sqrtf(pkt.mag_x * pkt.mag_x + pkt.mag_y * pkt.mag_y + pkt.mag_z * pkt.mag_z);
    snprintf(buf, sizeof(buf), "|M|=%6.1f uT  ", (double)magMag);
    canvas.setTextColor(COLOR_GRAY, COLOR_BLACK);
    canvas.setCursor(24, 9);
    canvas.print(buf);

    // Accel XYZ
    snprintf(buf, sizeof(buf), "%5.2f %5.2f %5.2f",
             (double)pkt.accel_x, (double)pkt.accel_y, (double)pkt.accel_z);
    canvas.setTextColor(COLOR_WHITE, COLOR_BLACK);
    canvas.setCursor(24, 18);
    canvas.print(buf);

    // Accel magnitude
    float accMag = sqrtf(pkt.accel_x * pkt.accel_x + pkt.accel_y * pkt.accel_y + pkt.accel_z * pkt.accel_z);
    snprintf(buf, sizeof(buf), "|A|=%5.2f g    ", (double)accMag);
    canvas.setTextColor(COLOR_GRAY, COLOR_BLACK);
    canvas.setCursor(24, 27);
    canvas.print(buf);

    // Gyro XYZ
    snprintf(buf, sizeof(buf), "%6.3f %6.3f %6.3f",
             (double)pkt.gyro_x, (double)pkt.gyro_y, (double)pkt.gyro_z);
    canvas.setTextColor(COLOR_WHITE, COLOR_BLACK);
    canvas.setCursor(24, 36);
    canvas.print(buf);

    // Fused heading
    snprintf(buf, sizeof(buf), "%5.1f ", (double)pkt.fused_heading_deg);
    canvas.setTextColor(COLOR_GREEN, COLOR_BLACK);
    canvas.setCursor(60, 54);
    canvas.print(buf);

    // Raw mag heading
    snprintf(buf, sizeof(buf), "%5.1f ", (double)pkt.raw_mag_heading_deg);
    canvas.setTextColor(COLOR_YELLOW, COLOR_BLACK);
    canvas.setCursor(60, 63);
    canvas.print(buf);

    // Pitch
    snprintf(buf, sizeof(buf), "%6.1f", (double)pkt.pitch_deg);
    canvas.setTextColor(COLOR_WHITE, COLOR_BLACK);
    canvas.setCursor(12, 72);
    canvas.print(buf);

    // Roll
    snprintf(buf, sizeof(buf), "%6.1f", (double)pkt.roll_deg);
    canvas.setCursor(72, 72);
    canvas.print(buf);

    flush();
}

// ===========================================================================
// Random-rect self-test
// ===========================================================================

static bool     rectTestActive  = false;
static uint8_t  rectTestCoverage = 25;
static uint32_t rectTestLastMs  = 0;

void startRandomRectTest(uint8_t coveragePct) {
    rectTestCoverage = constrain(coveragePct, 1, 100);
    rectTestActive = true;
    rectTestLastMs = 0;
    canvas.fillScreen(COLOR_BLACK);
    flush();
    Serial.printf("[DISP] random-rect test started (%u%% coverage)\n", rectTestCoverage);
}

void stopRandomRectTest() {
    rectTestActive = false;
    canvas.fillScreen(COLOR_BLACK);
    flush();
    Serial.println("[DISP] random-rect test stopped");
}

bool isRandomRectTestActive() { return rectTestActive; }

void tickRandomRectTest() {
    if (!rectTestActive) return;
    uint32_t now = millis();
    if (now - rectTestLastMs < 1000) return;
    rectTestLastMs = now;

    // Compute rect dimensions from coverage percentage
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

    canvas.fillRect(x, y, w, h, color);
    flush();
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
    canvas.fillScreen(COLOR_BLACK);
    flush();
    Serial.printf("[DISP] random-text test started (%u%% coverage)\n", textTestCoverage);
}

void stopRandomTextTest() {
    textTestActive = false;
    canvas.fillScreen(COLOR_BLACK);
    flush();
    Serial.println("[DISP] random-text test stopped");
}

bool isRandomTextTestActive() { return textTestActive; }

void tickRandomTextTest() {
    if (!textTestActive) return;
    uint32_t now = millis();
    if (now - textTestLastMs < 1000) return;
    textTestLastMs = now;

    int stringlen = 10;
    char str[stringlen];
    for (int i = 0; i < stringlen - 1; i++) {
        str[i] = (char)random(0x21, 0x7F);
    }
    str[stringlen - 1] = '\0';

    canvas.setTextSize(2);
    canvas.setTextColor(COLOR_WHITE, COLOR_BLACK);
    canvas.setCursor(5, 5);
    canvas.print(str);
    flush();

    Serial.printf("[DISP] text '%s'\n", str);
}

}  // namespace display
