// display_main.cpp — Display device entry point
// Receives NavPacket from nav device over Serial1, renders on ST7789 TFT (320x240).
// Reads buttons and sends DisplayCmd back to nav device.
// Menu system: BTN1 opens/cycles, BTN2 selects, 15s idle timeout.
// Both buttons held 2s: display reinit (SPI re-init + clear, no reboot).

#include <Arduino.h>
#include <esp_sleep.h>

#include "board_pins.h"
#include "config.h"
#include "drivers/display.h"
#include "menu/menu.h"
#include "nav/state.h"
#include <dpvlink.h>

// ---- Link receive state ----------------------------------------------------
static char rxBuf[4096];  // sized for WaypointListPacket (50 waypoints ≈ 3 KB)
static size_t rxPos = 0;
static NavPacket lastNav{};
static bool navValid = false;
static uint32_t lastNavMs = 0;

static DebugPacket lastDebug{};
static bool debugValid = false;

static CalProgressPacket lastCalProgress{};
static bool calProgressValid  = false;
static uint32_t lastCalProgressMs = 0;
static constexpr uint32_t CAL_COMPLETE_HOLD_MS = 3000;  // show "DONE" for 3 s then return
static uint32_t calCompleteShownMs = 0;
static bool calCompleteHolding = false;

// ---- Link transmit buffer --------------------------------------------------
static char txBuf[64];

// ---- Timing ----------------------------------------------------------------
static constexpr uint32_t DISPLAY_INTERVAL_MS = 250;  // 4 Hz refresh
static constexpr uint32_t NAV_TIMEOUT_MS      = 5000; // show "NO LINK" after 5 s
static constexpr uint32_t COUNTER_INTERVAL_MS = 1000; // 1 Hz idle counter
static constexpr uint32_t REINIT_INTERVAL_MS  = 30000000; // periodic display reinit
static uint32_t lastDisplayMs  = 0;
static uint32_t lastCounterMs  = 0;
static uint32_t lastReinitMs   = 0;
static uint32_t idleCounter    = 0;
static bool     wasLinkDead    = false;

// ---- Boot phase state machine -----------------------------------------------
// WAITING  → loop shows "Waiting..." counter until first NavPacket
// STATUS   → shows nav-device boot results for BOOT_STATUS_HOLD_MS, then DONE
// DONE     → normal nav/cal/menu rendering
enum class BootPhase : uint8_t { WAITING, STATUS, DONE };
static BootPhase gBootPhase     = BootPhase::WAITING;
static uint32_t  gBootShowMs    = 0;
static constexpr uint32_t BOOT_STATUS_HOLD_MS = 4000;

// Track whether we need to redraw nav after menu closes
static bool menuWasOpen = false;

// ---- Fourier heading calibration UI state -----------------------------------
// Display drives 12 guided prompts at 30° intervals; BTN2 at each step sends
// CAPTURE_HDG_POINT to the nav device (which records gCurrentHeadingRawDeg).
// After all 12 points, FINALIZE_HDG_CAL is sent; the nav device saves
// /hdg_samples.csv for offline Fourier fitting.
static constexpr int kHdgCalNPoints = 12;
static constexpr float kHdgCalTargets[kHdgCalNPoints] = {
    0, 30, 60, 90, 120, 150, 180, 210, 240, 270, 300, 330
};

enum class HdgFourierCalPhase : uint8_t {
    NONE,        // not in hdg cal
    COLLECTING,  // showing prompt for current step
    DONE,        // showing done screen; BTN2 to exit
};

static HdgFourierCalPhase gHdgFourierCalPhase = HdgFourierCalPhase::NONE;
static int                gHdgFourierCalStep  = 0;

// ---- Speed calibration UI state --------------------------------------------
// Phases live entirely on the display device; the nav device drives
// cal_mode (2/3/4) in NavPackets once the run starts.
enum class SpeedCalPhase : uint8_t {
    NONE,         // not in speed cal
    DIST_SELECT,  // user choosing target distance
    WAITING,      // waiting for DPV flow to start
    COUNTDOWN,    // manual countdown to start (user long-pressed BTN2)
    RUNNING,      // run in progress (nav drives timer via cal_mode=3)
    RESULT,       // accept/reject result (nav drives cal_mode=4)
};

static SpeedCalPhase gSpeedCalPhase      = SpeedCalPhase::NONE;
static uint16_t      gSpeedCalDist_ft    = 300;  // selected distance
static uint8_t       gSpeedCalChoice     = 0;    // 0=RESET+ACCEPT, 1=ACCEPT, 2=REJECT
static uint32_t      gCountdownStartMs   = 0;    // millis() when countdown begins
static constexpr uint32_t COUNTDOWN_TOTAL_MS = 5000;  // 5 second countdown

// ---- Waypoint list cache (populated from WaypointListPacket at 1 Hz) --------
struct CachedWaypoint {
    char  name[13];  // 12 display chars + null
    float lat;
    float lon;
};
static CachedWaypoint gWpCache[WP_PACKET_MAX];
static uint8_t        gWpCount      = 0;
static uint8_t        gWpTotalCount = 0;

// ---- Waypoint selection / arrival UI state ----------------------------------
enum class WaypointUiPhase : uint8_t {
    NONE,    // not in waypoint UI
    SELECT,  // user choosing which waypoint to navigate TO
    ARRIVE,  // user choosing which waypoint they have ARRIVED AT
};
static WaypointUiPhase gWaypointUiPhase = WaypointUiPhase::NONE;
static uint8_t         gWaypointUiIdx  = 0;
static char            gWpPrevTitle[24] = "";
static char            gWpPrevName[20]  = "";

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

// ---- Power-off / deep sleep ------------------------------------------------
// Both buttons (active-LOW) must be held simultaneously to wake.
// ext1 wakes on ALL_LOW: the ESP32 wakes when all configured pins are LOW.
// After waking we re-check that both pins stay LOW for WAKE_HOLD_MS; if
// either releases we go back to sleep immediately.
static constexpr uint64_t SLEEP_WAKE_PIN_MASK =
    (1ULL << BUTTON1_PIN) | (1ULL << BUTTON2_PIN);
static constexpr uint32_t WAKE_HOLD_MS = 600;   // both-buttons hold to confirm wake

// Enter ESP32 deep sleep.  Never returns.
static void enterDeepSleep() {
    Serial.println("[SLEEP] entering deep sleep");
    Serial.flush();
    esp_sleep_enable_ext1_wakeup(SLEEP_WAKE_PIN_MASK, ESP_EXT1_WAKEUP_ALL_LOW);
    esp_deep_sleep_start();
}

// ---- Forward declarations --------------------------------------------------
static void processNavLine();
static void processUsbCmd();
static void sendCmd(DisplayCmd cmd);
static void sendSpeedCalStart(uint16_t dist_ft);
static void sendWaypointSelectCmd(uint8_t idx);
static void sendWaypointArriveCmd(uint8_t idx);
static void renderWaypointUi();
static void updateButton(ButtonState& b);
static void handleButtons();

// ===========================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n=== DPV-NAV (display device) ===");

    // ---- Wake-from-deep-sleep check ----------------------------------------
    // Buttons are configured as INPUT in the lines below; read them now using
    // direct INPUT mode since pinMode hasn't been called yet — but the GPIO
    // pad default is INPUT, so digitalRead is safe here.
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
        Serial.println("[WAKE] woke from deep sleep via EXT1");
        // Require both buttons held for WAKE_HOLD_MS to confirm intentional wake.
        // If either releases before that, go straight back to sleep.
        pinMode(BUTTON1_PIN, INPUT);
        pinMode(BUTTON2_PIN, INPUT);
        uint32_t wakeStart = millis();
        bool confirmed = true;
        while (millis() - wakeStart < WAKE_HOLD_MS) {
            if (digitalRead(BUTTON1_PIN) == HIGH || digitalRead(BUTTON2_PIN) == HIGH) {
                confirmed = false;
                break;
            }
            delay(10);
        }
        if (!confirmed) {
            Serial.println("[WAKE] only one button — going back to sleep");
            Serial.flush();
            enterDeepSleep();
        }
        Serial.println("[WAKE] both buttons confirmed — booting");
        // Send a wake byte to the nav device over Serial1 so it exits deep sleep.
        // (Nav device is configured to wake on its LINK_RX_PIN going LOW, which
        // happens on the UART start bit of this transmission.)
        Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
        Serial1.write('\n');   // single byte — just needs the start bit low edge
        Serial1.flush();
        // Serial1 will be re-initialized below in the normal setup path.
        Serial1.end();
    }

    // Serial link from nav device
    Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);

    // Buttons (direct GPIO, external pull-up)
    pinMode(BUTTON1_PIN, INPUT);
    pinMode(BUTTON2_PIN, INPUT);

    // TFT display (ST7789)
    bool displayOk = display::init();
    Serial.print("Display init: ");
    Serial.println(displayOk ? "OK" : "FAIL");

    // Self-test: R/G/B color fills + "DPV-Nav" splash
    display::selfTest();
    display::clear();

    // Initialize menu system
    menu::init(sendCmd);

    display::reinit();
    Serial.println("Display device ready — waiting for nav data");
}

// Returns a copy of pkt with heading/bearing adjusted for the current heading
// mode (true vs. magnetic). Nav device always sends true heading; when the user
// selects magnetic, subtract declination here and clear FLAG_TRUE_HEADING.
static NavPacket applyHeadingMode(NavPacket pkt) {
    if (!menu::settings().trueHeading) {
        auto wrap360 = [](float d) {
            while (d < 0.0f)    d += 360.0f;
            while (d >= 360.0f) d -= 360.0f;
            return d;
        };
        pkt.heading_deg      = wrap360(pkt.heading_deg      - DEFAULT_DECLINATION_DEG);
        pkt.bearing_home_deg = wrap360(pkt.bearing_home_deg - DEFAULT_DECLINATION_DEG);
        pkt.flags &= ~FLAG_TRUE_HEADING;
    }
    return pkt;
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

    // --- If menu just closed, check for pending actions or force redraw ------
    if (menuWasOpen && !menu::isOpen()) {
        menuWasOpen = false;

        if (menu::isPendingPowerOff()) {
            menu::clearPowerOffPending();
            // Tell the nav device to save state and sleep.
            sendCmd(DisplayCmd::POWER_OFF);
            // "Powering off" animation: 5 dots, one per second.
            display::clear();
            constexpr uint16_t CLR_WHITE = 0xFFFF;
            for (int dots = 1; dots <= 5; dots++) {
                display::clear();
                char buf[20];
                snprintf(buf, sizeof(buf), "Powering off");
                display::drawText(4, 108, buf, CLR_WHITE, 2);
                // Draw dots accumulated so far
                char dotBuf[8];
                for (int i = 0; i < dots; i++) dotBuf[i] = '.';
                dotBuf[dots] = '\0';
                display::drawText(4, 140, dotBuf, CLR_WHITE, 2);
                display::flush();
                delay(1000);
            }
            display::clear();
            display::flush();
            enterDeepSleep();
            // never returns
        }

        if (menu::isPendingSpeedCal()) {
            menu::clearSpeedCalPending();
            gSpeedCalPhase   = SpeedCalPhase::DIST_SELECT;
            gSpeedCalDist_ft = 300;  // default distance
            gSpeedCalChoice  = 0;
            Serial.println("[SPEED_CAL] entering distance selection");
        }

        if (menu::isPendingHdgCal()) {
            menu::clearHdgCalPending();
            sendCmd(DisplayCmd::START_HDG_FOURIER_CAL);
            gHdgFourierCalPhase = HdgFourierCalPhase::COLLECTING;
            gHdgFourierCalStep  = 0;
            Serial.println("[HDG_CAL] entering Fourier heading cal");
        }

        if (menu::isPendingWaypointSelect()) {
            menu::clearWaypointSelectPending();
            gWaypointUiPhase = WaypointUiPhase::SELECT;
            gWaypointUiIdx   = 0;
            gWpPrevTitle[0]  = '\0';
            gWpPrevName[0]   = '\0';
            Serial.println("[WP] entering Select Waypoint UI");
        }

        if (menu::isPendingWaypointArrive()) {
            menu::clearWaypointArrivePending();
            gWaypointUiPhase = WaypointUiPhase::ARRIVE;
            gWaypointUiIdx   = 0;
            gWpPrevTitle[0]  = '\0';
            gWpPrevName[0]   = '\0';
            Serial.println("[WP] entering Arrive Waypoint UI");
        }

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
        // reinit re-initializes SSD controller and SPI speed
    }

    // --- Update display -------------------------------------------------------
    // Skip normal rendering while any self-test is running
    if (display::isRandomRectTestActive() || display::isRandomTextTestActive()) return;

    if (gBootPhase == BootPhase::WAITING) {
        // No nav contact yet — show animated "Waiting..." counter at 1 Hz
        if (now - lastCounterMs >= COUNTER_INTERVAL_MS) {
            lastCounterMs = now;
            idleCounter++;
            char buf[22];
            snprintf(buf, sizeof(buf), "Waiting... %lus", (unsigned long)idleCounter);
            display::drawText(4, 44, buf, 0x07E0, 1);
            display::flush();
        }
        return;
    }

    if (gBootPhase == BootPhase::STATUS) {
        // Show boot status screen until hold time expires; re-render at 4 Hz
        // to keep it visible (display::showBootStatus was already called on first packet).
        if (now - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
            lastDisplayMs = now;
            display::showBootStatus(lastNav.boot_flags);
        }
        if (now - gBootShowMs >= BOOT_STATUS_HOLD_MS) {
            gBootPhase = BootPhase::DONE;
            wasLinkDead = false;
            display::clear();
        }
        return;
    }

    // --- BootPhase::DONE — normal operation ----------------------------------
    bool linkAlive = navValid && (now - lastNavMs < NAV_TIMEOUT_MS);

    if (linkAlive) {
        if (wasLinkDead) {
            wasLinkDead = false;
            display::clear();
        }
        // Live data — render at 4 Hz
        if (now - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
            lastDisplayMs = now;

            // Bin cal complete: hold DONE screen for CAL_COMPLETE_HOLD_MS then return
            if (calCompleteHolding) {
                display::showCalGrid(lastCalProgress,
                                     lastCalProgress.cal_type == (uint8_t)CalType::MOUNTED
                                         ? "MOUNTED CAL" : "BASELINE CAL");
                if (now - calCompleteShownMs >= CAL_COMPLETE_HOLD_MS) {
                    calCompleteHolding = false;
                    calProgressValid   = false;
                    display::clear();
                }
                return;
            }

            // Active bin cal progress grid — takes priority over all other rendering
            if (calProgressValid) {
                display::showCalGrid(lastCalProgress,
                                     lastCalProgress.cal_type == (uint8_t)CalType::MOUNTED
                                         ? "MOUNTED CAL" : "BASELINE CAL");
                return;
            }

            // Waypoint selection / arrival UI — takes priority while active
            if (gWaypointUiPhase != WaypointUiPhase::NONE) {
                renderWaypointUi();
                return;
            }

            // Fourier heading cal — takes priority while active
            if (gHdgFourierCalPhase == HdgFourierCalPhase::COLLECTING) {
                display::showHdgFourierCalPrompt(gHdgFourierCalStep, kHdgCalNPoints,
                                                 kHdgCalTargets[gHdgFourierCalStep],
                                                 navValid ? lastNav.heading_raw_deg : 0.0f);
                return;
            }
            if (gHdgFourierCalPhase == HdgFourierCalPhase::DONE) {
                display::showHdgFourierCalDone(kHdgCalNPoints);
                return;
            }

            // Speed cal distance selection overrides everything (pre-nav-device)
            if (gSpeedCalPhase == SpeedCalPhase::DIST_SELECT) {
                display::showSpeedCalDistSelect(gSpeedCalDist_ft);
            } else if (gSpeedCalPhase == SpeedCalPhase::COUNTDOWN) {
                // Countdown in progress — show countdown timer and auto-send START when done
                uint32_t elapsedMs = now - gCountdownStartMs;
                if (elapsedMs >= COUNTDOWN_TOTAL_MS) {
                    // Countdown complete — send START_SPEED_CAL and return to WAITING
                    sendSpeedCalStart(gSpeedCalDist_ft);
                    gSpeedCalPhase = SpeedCalPhase::WAITING;
                    Serial.println("[SPEED_CAL] Countdown complete, sent START_SPEED_CAL");
                } else {
                    // Still counting down — show remaining seconds (5, 4, 3, 2, 1, GO!)
                    uint32_t remainingMs = COUNTDOWN_TOTAL_MS - elapsedMs;
                    int secondsRemaining = (int)((remainingMs + 999) / 1000);  // round up
                    display::showSpeedCalCountdown(secondsRemaining);
                }
            } else {

            SystemState navState = static_cast<SystemState>(lastNav.system_state);
            if (navState == SystemState::CALIBRATION) {
                // Dispatch by cal_mode: 0/1 = mag cal (legacy), 2/3/4 = speed cal
                if (lastNav.cal_mode <= 1) {
                    display::showCal(lastNav.cal_remaining_s,
                                     lastNav.cal_coverage_pct,
                                     lastNav.cal_mode == 1);
                } else if (lastNav.cal_mode == 2) {
                    display::showSpeedCalWaiting();
                } else if (lastNav.cal_mode == 3) {
                    display::showSpeedCalRunning(lastNav.speed_cal_elapsed_s,
                                                 lastNav.speed_cal_dist_ft);
                } else if (lastNav.cal_mode == 4) {
                    display::showSpeedCalResult(lastNav.speed_cal_dist_ft,
                                                lastNav.speed_cal_elapsed_s,
                                                lastNav.speed_cal_k_existing,
                                                lastNav.speed_cal_k_proposed,
                                                gSpeedCalChoice);
                }
                // cal_mode 5/6 (bin cal): rendered via CalProgressPacket above
            } else if (menu::isOpen()) {
                // Menu is open — draw top row, then menu overlay, then flush (no-op)
                display::showNavTop(applyHeadingMode(lastNav));
                menu::render();
                display::flush();
            } else {
                // Normal full-screen nav or debug
                if (menu::settings().debugMode && debugValid) {
                    display::showDebug(lastDebug);
                } else {
                    display::showNav(applyHeadingMode(lastNav));
                }
            }
            } // end else (not DIST_SELECT)
        }
    } else {
        // Link dropped
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
            menu::updateNavState(lastNav.flags);
            if (gBootPhase == BootPhase::WAITING) {
                gBootPhase  = BootPhase::STATUS;
                gBootShowMs = millis();
                display::showBootStatus(lastNav.boot_flags);
            }
            // Advance speed cal phase based on cal_mode from nav device.
            // Only advance forward — never reset backward on a single packet.
            SystemState ns = static_cast<SystemState>(lastNav.system_state);
            if (ns == SystemState::CALIBRATION) {
                if (lastNav.cal_mode == 3 &&
                    (gSpeedCalPhase == SpeedCalPhase::WAITING ||
                     gSpeedCalPhase == SpeedCalPhase::RUNNING)) {
                    gSpeedCalPhase = SpeedCalPhase::RUNNING;
                } else if (lastNav.cal_mode == 4 &&
                           gSpeedCalPhase != SpeedCalPhase::RESULT) {
                    gSpeedCalPhase   = SpeedCalPhase::RESULT;
                    gSpeedCalChoice  = 0;
                    Serial.println("[SPEED_CAL] result ready — showing accept/reject");
                }
            }
            // If nav device returns to READY after bin cal, clear our cal state
            if (ns != SystemState::CALIBRATION && calProgressValid) {
                if (!calCompleteHolding) {
                    calProgressValid = false;
                }
            }
        }
    } else if (ptype == PacketType::DEBUG) {
        if (bytesToDebugPacket(rxBuf, rxPos, lastDebug)) {
            debugValid = true;
            lastNavMs = millis();
        }
    } else if (ptype == PacketType::CAL_PROGRESS) {
        if (bytesToCalProgressPacket(rxBuf, rxPos, lastCalProgress)) {
            calProgressValid   = true;
            lastCalProgressMs  = millis();
            lastNavMs          = millis();  // keep link-alive timer refreshed
            if (lastCalProgress.complete && !calCompleteHolding) {
                calCompleteHolding  = true;
                calCompleteShownMs  = millis();
                Serial.println("[CAL_GRID] Complete — holding DONE screen");
            }
        }
    } else if (ptype == PacketType::WAYPOINT_LIST) {
        WaypointListPacket wpPkt{};
        if (bytesToWaypointListPacket(rxBuf, rxPos, wpPkt)) {
            gWpCount      = wpPkt.count;
            gWpTotalCount = wpPkt.total_count;
            int n = (int)wpPkt.count;
            if (n > WP_PACKET_MAX) n = WP_PACKET_MAX;
            for (int i = 0; i < n; i++) {
                strncpy(gWpCache[i].name, wpPkt.waypoints[i].name, 12);
                gWpCache[i].name[12] = '\0';
                gWpCache[i].lat = wpPkt.waypoints[i].lat;
                gWpCache[i].lon = wpPkt.waypoints[i].lon;
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

static char wpTxBuf[64];

static void sendCmd(DisplayCmd cmd) {
    size_t n = displayCmdToBytes(cmd, txBuf, sizeof(txBuf));
    if (n > 0) {
        Serial1.write(txBuf, n);
    }
}

static void sendSpeedCalStart(uint16_t dist_ft) {
    size_t n = displaySpeedCalStartToBytes(dist_ft, txBuf, sizeof(txBuf));
    if (n > 0) {
        Serial1.write(txBuf, n);
        Serial.printf("[SPEED_CAL] START_SPEED_CAL dist=%uft\n", (unsigned)dist_ft);
    }
}

static void sendWaypointSelectCmd(uint8_t idx) {
    size_t n = displaySelectWaypointToBytes(idx, wpTxBuf, sizeof(wpTxBuf));
    if (n > 0) Serial1.write(wpTxBuf, n);
}

static void sendWaypointArriveCmd(uint8_t idx) {
    size_t n = displayArriveWaypointToBytes(idx, wpTxBuf, sizeof(wpTxBuf));
    if (n > 0) Serial1.write(wpTxBuf, n);
}

// ---------------------------------------------------------------------------
// Render waypoint selection / arrival UI (full-screen takeover).
// Uses incremental update — only redraws when content changes.
// Layout (320×240):
//   y=10  title   "NAV TO:" or "ARRIVED AT:" (size 2, cyan)
//   y=60  wp name (size 3, yellow, padded to 10 chars)
//   y=120 counter "n/N" (size 2, white)
//   y=200 hint    (size 1, gray)
// ---------------------------------------------------------------------------
static void renderWaypointUi() {
    constexpr uint16_t CLR_CYAN   = 0x07FF;
    constexpr uint16_t CLR_YELLOW = 0xFFE0;
    constexpr uint16_t CLR_WHITE  = 0xFFFF;
    constexpr uint16_t CLR_GRAY   = 0x7BEF;

    const char* title = (gWaypointUiPhase == WaypointUiPhase::SELECT)
                        ? "NAV TO:     "
                        : "ARRIVED AT: ";

    if (strcmp(title, gWpPrevTitle) != 0) {
        strncpy(gWpPrevTitle, title, sizeof(gWpPrevTitle) - 1);
        display::drawText(4, 10, title, CLR_CYAN, 2);
    }

    const char* wpName = (gWpCount > 0) ? gWpCache[gWaypointUiIdx].name : "(no waypoints)";
    char nameBuf[18];
    snprintf(nameBuf, sizeof(nameBuf), "%-16s", wpName);
    if (strcmp(nameBuf, gWpPrevName) != 0) {
        strncpy(gWpPrevName, nameBuf, sizeof(gWpPrevName) - 1);
        display::drawText(4, 60, nameBuf, CLR_YELLOW, 3);

        char countBuf[16];
        snprintf(countBuf, sizeof(countBuf), "%u/%u        ",
                 gWpCount > 0 ? (uint8_t)(gWaypointUiIdx + 1) : 0, gWpTotalCount);
        display::drawText(4, 120, countBuf, CLR_WHITE, 2);
    }

    // Static hint line — only write once per UI entry (prevTitle gate covers it)
    if (gWpCount > 0) {
        display::drawText(4, 200, "BTN1:next  BTN2:select", CLR_GRAY, 1);
    } else {
        display::drawText(4, 200, "No waypoints available ", CLR_GRAY, 1);
    }

    display::flush();
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

    // --- Fourier heading calibration -----------------------------------------
    if (gHdgFourierCalPhase == HdgFourierCalPhase::COLLECTING) {
        // BTN2 short press: send CAPTURE_HDG_POINT with current target; nav device
        // records its live gCurrentHeadingRawDeg alongside the target.
        if (!btn2.pressed && !btn2.fired && btn2.pressStartMs > 0) {
            btn2.fired = true;
            float target = kHdgCalTargets[gHdgFourierCalStep];
            size_t n = displayCaptureHdgPointToBytes(target, txBuf, sizeof(txBuf));
            if (n > 0) Serial1.write(txBuf, n);
            Serial.printf("[HDG_CAL] Step %d/%d target=%.0f\n",
                          gHdgFourierCalStep + 1, kHdgCalNPoints, target);
            gHdgFourierCalStep++;
            if (gHdgFourierCalStep >= kHdgCalNPoints) {
                sendCmd(DisplayCmd::FINALIZE_HDG_CAL);
                gHdgFourierCalPhase = HdgFourierCalPhase::DONE;
                display::clear();
            }
        }
        return;
    }

    if (gHdgFourierCalPhase == HdgFourierCalPhase::DONE) {
        // BTN2 short press: dismiss done screen and return to normal nav
        if (!btn2.pressed && !btn2.fired && btn2.pressStartMs > 0) {
            btn2.fired = true;
            gHdgFourierCalPhase = HdgFourierCalPhase::NONE;
            display::clear();
            Serial.println("[HDG_CAL] Done screen dismissed");
        }
        return;
    }

    // --- Speed cal: distance selection mode ----------------------------------
    if (gSpeedCalPhase == SpeedCalPhase::DIST_SELECT) {
        // BTN1: cycle distance (150→200→…→500→150)
        if (!btn1.pressed && !btn1.fired && btn1.pressStartMs > 0) {
            btn1.fired = true;
            gSpeedCalDist_ft += 50;
            if (gSpeedCalDist_ft > 500) gSpeedCalDist_ft = 150;
            Serial.printf("[SPEED_CAL] dist select: %uft\n", (unsigned)gSpeedCalDist_ft);
        }
        // BTN2: confirm distance, kick off run on nav device
        if (!btn2.pressed && !btn2.fired && btn2.pressStartMs > 0) {
            btn2.fired = true;
            sendSpeedCalStart(gSpeedCalDist_ft);
            gSpeedCalPhase = SpeedCalPhase::WAITING;
        }
        return;
    }

    // --- Speed cal: waiting for flow (or manual countdown) --------------------
    if (gSpeedCalPhase == SpeedCalPhase::WAITING || gSpeedCalPhase == SpeedCalPhase::COUNTDOWN) {
        // BTN2: long press starts manual countdown
        if (btn2.pressed && !btn2.fired) {
            uint32_t held = now - btn2.pressStartMs;
            if (held >= LONG_PRESS_MS && gSpeedCalPhase == SpeedCalPhase::WAITING) {
                // Transition to COUNTDOWN
                gCountdownStartMs = now;
                gSpeedCalPhase = SpeedCalPhase::COUNTDOWN;
                btn2.fired = true;
                Serial.println("[SPEED_CAL] Long-press BTN2: starting manual countdown");
            }
        }
        return;
    }

    // --- Speed cal: accept/reject result mode --------------------------------
    if (gSpeedCalPhase == SpeedCalPhase::RESULT) {
        // BTN1: cycle through choices
        if (!btn1.pressed && !btn1.fired && btn1.pressStartMs > 0) {
            btn1.fired = true;
            gSpeedCalChoice = (gSpeedCalChoice + 1) % 3;
        }
        // BTN2: confirm choice
        if (!btn2.pressed && !btn2.fired && btn2.pressStartMs > 0) {
            btn2.fired = true;
            if (gSpeedCalChoice == 0) {
                sendCmd(DisplayCmd::SPEED_CAL_ACCEPT_RESET);
                Serial.println("[SPEED_CAL] choice: RESET+ACCEPT");
            } else if (gSpeedCalChoice == 1) {
                sendCmd(DisplayCmd::SPEED_CAL_ACCEPT);
                Serial.println("[SPEED_CAL] choice: ACCEPT");
            } else {
                sendCmd(DisplayCmd::SPEED_CAL_REJECT);
                Serial.println("[SPEED_CAL] choice: REJECT");
            }
            gSpeedCalPhase = SpeedCalPhase::NONE;
            display::clear();
        }
        return;
    }

    // --- Waypoint UI ---------------------------------------------------------
    if (gWaypointUiPhase != WaypointUiPhase::NONE) {
        // BTN1: cycle to next waypoint
        if (!btn1.pressed && !btn1.fired && btn1.pressStartMs > 0) {
            btn1.fired = true;
            if (gWpCount > 0) {
                gWaypointUiIdx = (gWaypointUiIdx + 1) % gWpCount;
                gWpPrevName[0] = '\0';  // force redraw on change
            }
        }
        // BTN2: confirm selection
        if (!btn2.pressed && !btn2.fired && btn2.pressStartMs > 0) {
            btn2.fired = true;
            if (gWpCount > 0) {
                if (gWaypointUiPhase == WaypointUiPhase::SELECT) {
                    sendWaypointSelectCmd(gWaypointUiIdx);
                    Serial.printf("[WP] SELECT waypoint idx=%u name=%s\n",
                                  gWaypointUiIdx, gWpCache[gWaypointUiIdx].name);
                } else {
                    sendWaypointArriveCmd(gWaypointUiIdx);
                    Serial.printf("[WP] ARRIVE waypoint idx=%u name=%s\n",
                                  gWaypointUiIdx, gWpCache[gWaypointUiIdx].name);
                }
            }
            gWaypointUiPhase = WaypointUiPhase::NONE;
            display::clear();
        }
        return;
    }

    // --- Normal menu handling ------------------------------------------------
    // BTN1: short press on release
    if (!btn1.pressed && !btn1.fired && btn1.pressStartMs > 0) {
        btn1.fired = true;
        if (menu::isOpen()) {
            menu::next();
        } else {
            menu::open();
            display::clear();  // force full redraw with menu
        }
    }

    // BTN2: short press on release
    if (!btn2.pressed && !btn2.fired && btn2.pressStartMs > 0) {
        btn2.fired = true;
        if (menu::isOpen()) {
            menu::select();
        }
        // When menu is closed, BTN2 has no action
    }
}
