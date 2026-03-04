// display_main.cpp — Display device entry point
// Receives NavPacket from nav device over Serial1, renders on SSD1351 OLED.
// Reads buttons and sends DisplayCmd back to nav device.

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MCP23X17.h>

#include "board_pins.h"
#include "config.h"
#include "drivers/display.h"
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
static constexpr uint32_t REINIT_INTERVAL_MS  = 3500; // periodic display reinit (workaround for blank-out)
static uint32_t lastDisplayMs  = 0;
static uint32_t lastCounterMs  = 0;
static uint32_t lastReinitMs   = 0;
static uint32_t idleCounter    = 0;
static bool     everConnected  = false;  // true once first NavPacket arrives
static bool     wasLinkDead    = false;  // true while showing "NO LINK" screen

// ---- Button debounce -------------------------------------------------------
// Buttons are direct GPIO, active LOW (external pull-up to 3.3 V).
// BTN1 short press: SET_HOME / CLEAR_HOME toggle
// BTN2 short press: (reserved / future)
// BTN2 long press:  START_MAG_CAL
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

// ---- Forward declarations --------------------------------------------------
static void processNavLine();
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
    display::selfTest();
    delay(1500);

    // Boot status lines
    display::clear();
    display::statusLine("Display", displayOk);
    display::statusLine("Backlight", mcpOk);
    display::statusLine("Link", false);  // will update when first packet arrives
    delay(1500);

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

    // --- Buttons ------------------------------------------------------------
    updateButton(btn1);
    updateButton(btn2);
    handleButtons();

    // --- Periodic display reinit (workaround for SPI blank-out) --------------
    uint32_t now = millis();
    if (now - lastReinitMs >= REINIT_INTERVAL_MS) {
        lastReinitMs = now;
        display::reinit();
    }

    // --- Update display -------------------------------------------------------
    bool linkAlive = navValid && (now - lastNavMs < NAV_TIMEOUT_MS);

    if (linkAlive) {
        // Transitioning from dead → alive: wipe the "NO LINK" screen
        if (wasLinkDead) {
            wasLinkDead = false;
            display::clear();
        }
        // Live data — render at 10 Hz
        if (now - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
            lastDisplayMs = now;
#if DISPLAY_MODE == 1
            if (debugValid) {
                display::showDebug(lastDebug);
            }
#else
            display::showNav(lastNav);
#endif
        }
    } else if (!everConnected) {
        // Never received nav data — show idle uptime counter at 1 Hz
        if (now - lastCounterMs >= COUNTER_INTERVAL_MS) {
            lastCounterMs = now;
            idleCounter++;
            char buf[22];
            snprintf(buf, sizeof(buf), "Waiting... %lus", (unsigned long)idleCounter);
            display::drawText(4, 44, buf, 0x07E0, 1);  // green, size 1
        }
    } else {
        // Was connected but link dropped
        if (!wasLinkDead) {
            wasLinkDead = true;
            display::clear();
            display::drawText(10, 40, "NO LINK", 0xF800, 2);  // red
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
            // Also count as link activity
            lastNavMs = millis();
            if (!everConnected) {
                everConnected = true;
                display::clear();
            }
        }
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
            // rising edge (pressed)
            b.pressed      = true;
            b.pressStartMs = now;
            b.fired        = false;
        } else if (!nowPressed && b.pressed) {
            // falling edge (released)
            b.pressed = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Map button events to DisplayCmd.
//   BTN1 short press  → SET_HOME / CLEAR_HOME toggle
//   BTN2 short press  → (reserved)
//   BTN2 long press   → START_MAG_CAL
// ---------------------------------------------------------------------------
static void handleButtons() {
    // --- BTN1: short press on release → toggle home --------------------------
    if (!btn1.pressed && !btn1.fired && btn1.pressStartMs > 0) {
        // just released without having fired a long-press action
        bool hasHome = navValid && (lastNav.flags & FLAG_HAS_HOME);
        sendCmd(hasHome ? DisplayCmd::CLEAR_HOME : DisplayCmd::SET_HOME);
        Serial.println(hasHome ? "BTN1: CLEAR_HOME" : "BTN1: SET_HOME");
        btn1.fired = true;
    }

    // --- BTN2: long press while held → mag cal ------------------------------
    if (btn2.pressed && !btn2.fired) {
        uint32_t held = millis() - btn2.pressStartMs;
        if (held >= LONG_PRESS_MS) {
            sendCmd(DisplayCmd::START_MAG_CAL);
            Serial.println("BTN2 long: START_MAG_CAL");
            btn2.fired = true;
        }
    }
}
