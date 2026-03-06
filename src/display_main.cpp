// display_main.cpp — Display device entry point
// Receives NavPacket from nav device over Serial1, renders on SSD1351 OLED.
// Reads buttons and sends DisplayCmd back to nav device.
// Menu system: BTN1 opens/cycles, BTN2 selects, 15s idle timeout.
// Both buttons held 2s: display reinit (SPI re-init + clear, no reboot).

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MCP23X17.h>

#include "board_pins.h"
#include "config.h"
#include "drivers/display.h"
#include "menu/menu.h"
#include <dpvlink.h>

// ---- Hardware --------------------------------------------------------------
static Adafruit_MCP23X17 mcp;
static bool mcpOk = false;

// ---- Link receive state ----------------------------------------------------
static char rxBuf[256];
static size_t rxPos = 0;
static NavPacket lastNav{};
static bool navValid = false;
static uint32_t lastNavMs = 0;

static DebugPacket lastDebug{};
static bool debugValid = false;

// ---- Link transmit buffer --------------------------------------------------
static char txBuf[64];

// ---- Timing ----------------------------------------------------------------
static constexpr uint32_t DISPLAY_INTERVAL_MS = 250;  // 4 Hz refresh
static constexpr uint32_t NAV_TIMEOUT_MS      = 5000; // show "NO LINK" after 5 s
static constexpr uint32_t COUNTER_INTERVAL_MS = 1000; // 1 Hz idle counter
static constexpr uint32_t REINIT_INTERVAL_MS  = 30000; // periodic display reinit
static uint32_t lastDisplayMs  = 0;
static uint32_t lastCounterMs  = 0;
static uint32_t lastReinitMs   = 0;
static uint32_t idleCounter    = 0;
static bool     everConnected  = false;
static bool     wasLinkDead    = false;

// Track whether we need to redraw nav after menu closes
static bool menuWasOpen = false;

// ---- Button debounce -------------------------------------------------------
static constexpr uint32_t DEBOUNCE_MS   = 50;
static constexpr uint32_t LONG_PRESS_MS = 2000;

struct ButtonState {
    uint8_t  pin;
    bool     lastRaw;       // last raw digitalRead (HIGH = released)
    bool     pressed;       // debounced: true while held
    uint32_t lastEdgeMs;    // time of last raw-level change
    uint32_t pressStartMs;  // millis() when pressed went true
    bool     fired;         // action already dispatched for this press
};

static ButtonState btn1{BUTTON1_PIN, true, false, 0, 0, false};
static ButtonState btn2{BUTTON2_PIN, true, false, 0, 0, false};

// ---- USB serial command buffer ---------------------------------------------
static char usbBuf[64];
static size_t usbPos = 0;

// ---- Forward declarations --------------------------------------------------
static void processNavLine();
static void processUsbCmd();
static void sendCmd(DisplayCmd cmd);
static void updateButton(ButtonState& b);
static void handleButtons();

// ===========================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n=== DPV-NAV (display device) ===");

    // Serial link from nav device
    Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);

    // Buttons (direct GPIO, external pull-up)
    pinMode(BUTTON1_PIN, INPUT);
    pinMode(BUTTON2_PIN, INPUT);

    // OLED display — init before I2C (matches working init order)
    bool displayOk = display::init();
    Serial.print("Display init: ");
    Serial.println(displayOk ? "OK" : "FAIL");

    // I2C bus + MCP23017 backlight
    Wire.begin(SDA_PIN, SCL_PIN);
    mcpOk = mcp.begin_I2C(MCP23017_ADDR, &Wire);
    if (mcpOk) {
        mcp.pinMode(MCP_BACKLIGHT_PIN, OUTPUT);
        mcp.digitalWrite(MCP_BACKLIGHT_PIN, HIGH);
        Serial.println("Backlight ON");
    } else {
        Serial.println("WARNING: MCP23017 not found — skipping backlight");
    }

    // Self-test: R/G/B color fills then summary text
    display::reinit(); 
    display::clear();
    display::selfTest();
    delay(500);

    // Boot status lines
    display::clear();
    display::statusLine("Display", displayOk);
    display::statusLine("Backlight", mcpOk);
    display::statusLine("Link", false);
    delay(500);

    // Initialize menu system
    menu::init(sendCmd);

    // Re-init display after all hardware setup (I2C/MCP init can disrupt SPI state)
    display::reinit();
    display::clear();
    Serial.println("Display device ready — waiting for nav data");
}

// ===========================================================================
void loop() {
    // --- Read serial link (non-blocking, line-buffered) ---------------------
    while (Serial1.available()) {
        char c = Serial1.read();
        if (c == '\n' || rxPos >= sizeof(rxBuf) - 1) {
            rxBuf[rxPos] = '\0';
            processNavLine();
            rxPos = 0;
        } else {
            rxBuf[rxPos++] = c;
        }
    }

    // --- Read USB serial commands (non-blocking, line-buffered) -------------
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r' || usbPos >= sizeof(usbBuf) - 1) {
            if (usbPos > 0) {
                usbBuf[usbPos] = '\0';
                processUsbCmd();
                usbPos = 0;
            }
        } else {
            usbBuf[usbPos++] = c;
        }
    }

    // --- Self-test ticks ------------------------------------------------------
    display::tickRandomRectTest();
    display::tickRandomTextTest();

    // --- Buttons ------------------------------------------------------------
    updateButton(btn1);
    updateButton(btn2);
    handleButtons();

    // --- Menu timeout check -------------------------------------------------
    menu::tick();

    // --- If menu just closed, force full redraw -----------------------------
    if (menuWasOpen && !menu::isOpen()) {
        menuWasOpen = false;
        display::clear();
    }
    if (menu::isOpen()) {
        menuWasOpen = true;
    }

    // --- Periodic display reinit (workaround for SPI blank-out) --------------
    uint32_t now = millis();
    if (now - lastReinitMs >= REINIT_INTERVAL_MS) {
        lastReinitMs = now;
        display::reinit();
        // reinit re-pushes the canvas buffer, so display content is restored
    }

    // --- Update display -------------------------------------------------------
    // Skip normal rendering while any self-test is running
    if (display::isRandomRectTestActive() || display::isRandomTextTestActive()) return;

    bool linkAlive = navValid && (now - lastNavMs < NAV_TIMEOUT_MS);

    if (linkAlive) {
        // Transitioning from dead → alive: wipe the "NO LINK" screen
        if (wasLinkDead) {
            wasLinkDead = false;
        }
        // Live data — render at 4 Hz
        if (now - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
            lastDisplayMs = now;

            if (menu::isOpen()) {
                // Menu is open — draw top row to canvas, then menu, then flush once
                display::showNavTop(lastNav);
                menu::render();
                display::flush();
            } else {
                // Normal full-screen nav or debug
                if (menu::settings().debugMode && debugValid) {
                    display::showDebug(lastDebug);
                } else {
                    display::showNav(lastNav);
                }
            }
        }
    } else if (!everConnected) {
        // Never received nav data — show idle uptime counter at 1 Hz
        if (now - lastCounterMs >= COUNTER_INTERVAL_MS) {
            lastCounterMs = now;
            idleCounter++;
            char buf[22];
            snprintf(buf, sizeof(buf), "Waiting... %lus", (unsigned long)idleCounter);
            display::drawText(4, 44, buf, 0x07E0, 1);
            display::flush();
        }
    } else {
        // Was connected but link dropped
        if (!wasLinkDead) {
            wasLinkDead = true;
            display::clear();
            display::drawText(10, 40, "NO LINK", 0xF800, 2);
            display::flush();
        }
    }
}

// ===========================================================================
// Helpers
// ===========================================================================

static void processNavLine() {
    if (rxPos == 0) return;

    PacketType ptype = identifyPacket(rxBuf, rxPos);
    if (ptype == PacketType::NAV) {
        if (bytesToNavPacket(rxBuf, rxPos, lastNav)) {
            navValid  = true;
            lastNavMs = millis();
            if (!everConnected) {
                everConnected = true;
                display::clear();
            }
        }
    } else if (ptype == PacketType::DEBUG) {
        if (bytesToDebugPacket(rxBuf, rxPos, lastDebug)) {
            debugValid = true;
            lastNavMs = millis();
            if (!everConnected) {
                everConnected = true;
                display::clear();
            }
        }
    }
}

static void processUsbCmd() {
    Serial.printf("[CMD] received: '%s' (len=%d)\n", usbBuf, (int)usbPos);
    // "selftest_rect_start"  or  "selftest_rect_start 50"  (coverage %)
    // "selftest_rect_stop"
    if (strncmp(usbBuf, "selftest_rect_start", 19) == 0) {
        uint8_t pct = 25;  // default
        if (usbBuf[19] == ' ') {
            int v = atoi(&usbBuf[20]);
            if (v >= 1 && v <= 100) pct = (uint8_t)v;
        }
        display::startRandomRectTest(pct);
    } else if (strcmp(usbBuf, "selftest_rect_stop") == 0) {
        display::stopRandomRectTest();
    } else if (strncmp(usbBuf, "selftest_text_start", 19) == 0) {
        uint8_t pct = 25;
        if (usbBuf[19] == ' ') {
            int v = atoi(&usbBuf[20]);
            if (v >= 1 && v <= 100) pct = (uint8_t)v;
        }
        display::startRandomTextTest(pct);
    } else if (strcmp(usbBuf, "selftest_text_stop") == 0) {
        display::stopRandomTextTest();
    } else {
        Serial.printf("[CMD] unknown: '%s'\n", usbBuf);
    }
}

static void sendCmd(DisplayCmd cmd) {
    size_t n = displayCmdToBytes(cmd, txBuf, sizeof(txBuf));
    if (n > 0) {
        Serial1.write(txBuf, n);
    }
}

// ---------------------------------------------------------------------------
// Debounce a single button.  Call once per loop iteration.
// ---------------------------------------------------------------------------
static void updateButton(ButtonState& b) {
    bool raw = digitalRead(b.pin);          // HIGH = released, LOW = pressed
    uint32_t now = millis();

    if (raw != b.lastRaw) {
        b.lastEdgeMs = now;
        b.lastRaw = raw;
    }

    if ((now - b.lastEdgeMs) >= DEBOUNCE_MS) {
        bool nowPressed = (b.lastRaw == LOW);
        if (nowPressed && !b.pressed) {
            b.pressed      = true;
            b.pressStartMs = now;
            b.fired        = false;
        } else if (!nowPressed && b.pressed) {
            b.pressed = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Map button events to menu actions or commands.
//   Both buttons held 2s  → reinit display
//   BTN1 short press      → open menu / cycle to next item
//   BTN2 short press      → select menu item
// ---------------------------------------------------------------------------
static void handleButtons() {
    uint32_t now = millis();

    // --- Both buttons held 2s → reset display --------------------------------
    if (btn1.pressed && btn2.pressed && !btn1.fired && !btn2.fired) {
        uint32_t held1 = now - btn1.pressStartMs;
        uint32_t held2 = now - btn2.pressStartMs;
        uint32_t minHeld = (held1 < held2) ? held1 : held2;
        if (minHeld >= LONG_PRESS_MS) {
            Serial.println("BTN1+BTN2 long: display reinit");
            display::reinit();
            btn1.fired = true;
            btn2.fired = true;
            return;
        }
    }

    // --- BTN1: short press on release ----------------------------------------
    if (!btn1.pressed && !btn1.fired && btn1.pressStartMs > 0) {
        btn1.fired = true;
        if (menu::isOpen()) {
            menu::next();
        } else {
            menu::open();
            display::clear();  // force full redraw with menu
        }
    }

    // --- BTN2: short press on release ----------------------------------------
    if (!btn2.pressed && !btn2.fired && btn2.pressStartMs > 0) {
        btn2.fired = true;
        if (menu::isOpen()) {
            menu::select();
        }
        // When menu is closed, BTN2 has no action
    }
}
