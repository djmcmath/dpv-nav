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

// Baseline BTN2 gives no visible change until nav's completion packet arrives
// (which can take a while for a big run -- nav does a synchronous CSV dump
// before it even sends that packet). Without this, the screen just redraws
// the same bars/prompt, indistinguishable from "not pressed yet", so the
// diver presses again. Set immediately on press; cleared once the real
// completion packet lands (entering calCompleteHolding) or after a timeout
// in case the command was dropped over serial.
static bool gBaselineFinishPending = false;
static uint32_t gBaselineFinishPendingMs = 0;
static constexpr uint32_t BASELINE_FINISH_PENDING_TIMEOUT_MS = 8000;

// ---- Link transmit buffer --------------------------------------------------
// 96, not 64: ACCEPT_CLOUD_CAL/REJECT_CLOUD_CAL carry a 36-char UUID ("cid")
// plus JSON overhead, which doesn't fit the old 64-byte size.
static char txBuf[96];

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
// LOGO     → boot splash (Tern logo centered); after LOGO_MIN_MS adds "Waiting..." counter
//            until first NavPacket arrives, then transitions to STATUS
// STATUS   → shows nav-device boot results for BOOT_STATUS_HOLD_MS, then DONE
// DONE     → normal nav/cal/menu rendering
enum class BootPhase : uint8_t { LOGO, STATUS, DONE };
static BootPhase gBootPhase     = BootPhase::LOGO;
static uint32_t  gBootShowMs    = 0;
static uint32_t  gLogoStartMs   = 0;
static constexpr uint32_t LOGO_MIN_MS         = 3000;
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
    // No DONE phase: the 12th point's capture hands off straight to the
    // shared gCloudCalPhase state machine (WAITING/OFFLINE/FAILED/RESULT),
    // same as baseline/mounted's bin-grid completion -- see
    // docs/architecture/heading-cal-cloud-plan.md.
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

// ---- Cloud calibration UI state (docs/cloud-calibration-plan.md) -----------
// Entered automatically when a bin-coverage cal's DONE-hold ends. The nav
// device then blocks on its own upload+fit call (see net/cloud_client.h), so
// no NavPacket/CalProgress updates arrive until a CalCloudResultPacket lands.
// gCloudCalPhase overrides the normal link-timeout ("NO LINK") check for that
// window rather than reporting a false dead link -- see linkAlive below.
enum class CloudCalUiPhase : uint8_t {
    NONE,     // not in cloud-cal UI
    WAITING,  // upload/fit in progress on the nav device (blocking there)
    OFFLINE,  // no WiFi -- upload was skipped
    FAILED,   // upload/fit failed, or nav device never responded
    RESULT,   // fit succeeded -- accept/reject
};

static CloudCalUiPhase   gCloudCalPhase          = CloudCalUiPhase::NONE;
static uint32_t          gCloudCalWaitStartMs    = 0;
static constexpr uint32_t CLOUD_CAL_WAIT_TIMEOUT_MS = 40000;  // > nav's ~30s worst case
static uint8_t           gCloudCalChoice         = 0;  // 0=ACCEPT, 1=REJECT
// False for a gap-fill result: that fit covers only the cells just patched
// and must never be offered as a standalone install (see nav_main.cpp's
// ACCEPT_CLOUD_CAL comment). The RESULT screen shows no ACCEPT/REJECT toggle
// when this is false -- just an acknowledgement.
static bool              gCloudCalInstallable    = true;
static uint8_t           gCloudCalQuality        = 0;
static uint8_t           gCloudCalType           = 0;  // CalType enum -- selects % vs deg label
static float             gCloudCalRmsPct         = 0.0f;
static int16_t           gCloudCalCoverageGaps   = -1;  // baseline only; -1 = n/a
static char              gCloudCalRecommendation[96] = "";
static char              gCloudCalError[64]          = "";
static char              gCloudCalId[40]             = "";

// ---- Cloud account-link UI state (device-auth bootstrap, RFC 8628) --------
// Entered when the diver selects "Link acct" from the Config menu. The nav
// device runs cloud::beginAuthorize() (fast, blocking) then polls for
// approval via the non-blocking cloud::updateAuthorizePoll() state machine
// (up to CLOUD_AUTH_POLL_TIMEOUT_MS, config.h) -- see net/cloud_client.h.
// BTN2 sends CANCEL_LINK to back out at any point during STARTING/WAITING.
// gCloudLinkPhase overrides the normal link-timeout check the same way
// gCloudCalPhase does, for the whole window.
enum class CloudLinkUiPhase : uint8_t {
    NONE,      // not in cloud-link UI
    STARTING,  // LINK_ACCOUNT sent, waiting for the device/user code
    WAITING,   // code received, polling for approval (cancelable via BTN2)
    OFFLINE,   // no WiFi -- flow was not attempted
    FAILED,    // begin/poll failed, or the nav device never responded
    DONE,      // approved -- token saved on the nav device
};

static CloudLinkUiPhase  gCloudLinkPhase          = CloudLinkUiPhase::NONE;
static uint32_t          gCloudLinkWaitStartMs    = 0;
// > nav's CLOUD_AUTH_POLL_TIMEOUT_MS (config.h) plus margin for the two HTTP
// round trips, so nav has a chance to deliver its own FAILED/DONE result
// packet before this independent display-side watchdog gives up first.
static constexpr uint32_t CLOUD_LINK_WAIT_TIMEOUT_MS = CLOUD_AUTH_POLL_TIMEOUT_MS + 30 * 1000;
static char              gCloudLinkUserCode[16]        = "";
static char              gCloudLinkError[64]           = "";

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
static void sendCloudCalRespond(DisplayCmd cmd, const char* calId);
static void sendSpeedCalStart(uint16_t dist_ft);
static void sendWaypointSelectCmd(uint8_t idx);
static void sendWaypointArriveCmd(uint8_t idx);
static void renderWaypointUi();

// One place decides which cal screen a CalProgressPacket gets. There are two
// call sites (live progress, and the post-completion DONE hold) and they must
// agree -- a completion screen that switches renderers mid-session is how you
// end up staring at a blank grid wondering whether the cal worked.
static void renderCalProgress(const CalProgressPacket& pkt) {
    if (pkt.phase == (uint8_t)CalPhase::ROUGH_SCAN) {
        display::showBaselineRoughScan(pkt);   // baseline: no grid, honestly
    } else if (pkt.phase == (uint8_t)CalPhase::GAP_FILL) {
        display::showCalGrid(pkt, "FILL GAPS");
    } else {
        display::showCalGrid(pkt, pkt.cal_type == (uint8_t)CalType::MOUNTED
                                      ? "MOUNTED CAL" : "BASELINE CAL");
    }
}

static void updateButton(ButtonState& b);
static bool handleModalButtons();
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

    // Initialize menu system (no display interaction)
    menu::init(sendCmd);

    // Re-stabilize the SPI controller, then draw the boot logo.
    // reinit() must come before showLogo() — it blanks the screen.
    display::reinit();
    lastReinitMs = millis();
    display::showLogo();
    gLogoStartMs = millis();
    Serial.println("Display device ready — waiting for nav data");
}

// Returns a copy of pkt with heading/bearing adjusted for the current heading
// mode. Nav device always sends true heading in heading_deg; when the user
// selects magnetic, subtract declination here and clear FLAG_TRUE_HEADING.
//
// RAW additionally swaps in heading_raw_deg, which the nav device already
// sends as the pre-Fourier, pre-motor-offset *magnetic* heading (nav_main.cpp
// passes headingMagDeg) -- the same value CAPTURE_HDG_POINT records as
// "indicated". That makes RAW the supported way to read indicated headings for
// the website's thin-sector gap-fill form, replacing the old routine of
// deleting /hdg_fourier.json and /motor_cal.json and reloading cal.
// Bearing-to-home stays on the magnetic treatment in RAW: there is no "raw"
// bearing, and leaving the rest of the nav screen coherent matters more than
// consistency with a field that has no raw counterpart.
static NavPacket applyHeadingMode(NavPacket pkt) {
    const uint8_t mode = menu::settings().headingMode;
    if (mode != nvs_disp::HEADING_TRUE) {
        auto wrap360 = [](float d) {
            while (d < 0.0f)    d += 360.0f;
            while (d >= 360.0f) d -= 360.0f;
            return d;
        };
        pkt.heading_deg      = (mode == nvs_disp::HEADING_RAW)
                                   ? wrap360(pkt.heading_raw_deg)
                                   : wrap360(pkt.heading_deg - DEFAULT_DECLINATION_DEG);
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

        if (menu::isPendingCloudLink()) {
            menu::clearCloudLinkPending();
            sendCmd(DisplayCmd::LINK_ACCOUNT);
            gCloudLinkPhase       = CloudLinkUiPhase::STARTING;
            gCloudLinkWaitStartMs = millis();
            gCloudLinkUserCode[0] = '\0';
            Serial.println("[CLOUD_LINK] requesting device-auth code");
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

    if (gBootPhase == BootPhase::LOGO) {
        uint32_t elapsed = now - gLogoStartMs;
        if (elapsed >= LOGO_MIN_MS) {
            if (navValid) {
                // Nav came up during the logo hold — transition now that 3 s have passed.
                gBootPhase  = BootPhase::STATUS;
                gBootShowMs = now;
                display::showBootStatus(lastNav.boot_flags);
            } else {
                // Still waiting — overlay counter at bottom of logo screen at 1 Hz.
                if (now - lastCounterMs >= COUNTER_INTERVAL_MS) {
                    lastCounterMs = now;
                    idleCounter++;
                    char buf[22];
                    snprintf(buf, sizeof(buf), "Waiting... %lus", (unsigned long)idleCounter);
                    display::drawText(4, 224, buf, 0x07E0, 1);
                }
            }
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
    // Cloud-cal UI overrides the link-timeout check: the nav device blocks on
    // its own upload+fit call while awaiting a result, so no packets arrive
    // for up to ~30s even though the link is fine. See CloudCalUiPhase above.
    // calCompleteHolding needs the same override: nav sends the "complete"
    // packet, then immediately does a (possibly multi-second, for a large
    // baseline run) CSV dump + blocking upload attempt before it sends
    // anything else — the CAL_COMPLETE_HOLD_MS window alone doesn't cover
    // that, since dump+upload can easily outlast it, and gCloudCalPhase isn't
    // set to WAITING until *after* the hold ends. Without this, a big enough
    // run flashes "NO LINK" for a few seconds even though nothing is wrong.
    bool awaitingCloudCal  = (gCloudCalPhase != CloudCalUiPhase::NONE);
    bool awaitingCloudLink = (gCloudLinkPhase != CloudLinkUiPhase::NONE);
    // gBaselineFinishPending covers the gap between BTN2 requesting
    // FINISH_BASELINE_COLLECTION and nav's completion packet actually
    // arriving -- nav may already be mid CSV-dump by the time it gets to
    // sending that packet. Same reasoning as calCompleteHolding below.
    bool linkAlive = calCompleteHolding || gBaselineFinishPending ||
                     awaitingCloudCal || awaitingCloudLink ||
                     (navValid && (now - lastNavMs < NAV_TIMEOUT_MS));

    if (linkAlive) {
        if (wasLinkDead) {
            wasLinkDead = false;
            display::clear();
        }
        // Live data — render at 4 Hz
        if (now - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
            lastDisplayMs = now;

            // Baseline: BTN2 was pressed, waiting for nav to confirm completion.
            // Times out (command likely dropped) back to the normal rough-scan
            // screen so the diver can just press BTN2 again.
            if (gBaselineFinishPending) {
                if (now - gBaselineFinishPendingMs >= BASELINE_FINISH_PENDING_TIMEOUT_MS) {
                    gBaselineFinishPending = false;
                    Serial.println("[BIN_CAL] Finish-baseline request timed out, retry available");
                } else {
                    display::showBaselineFinishing();
                    return;
                }
            }

            // Bin cal complete: hold DONE screen for CAL_COMPLETE_HOLD_MS then return.
            // Baseline never leaves ROUGH_SCAN (no grid to show); mounted uses the grid.
            if (calCompleteHolding) {
                renderCalProgress(lastCalProgress);
                if (now - calCompleteShownMs >= CAL_COMPLETE_HOLD_MS) {
                    calCompleteHolding = false;
                    calProgressValid   = false;
                    // The nav device uploads the CSV next (blocking there) --
                    // show the waiting screen until a CalCloudResultPacket lands.
                    gCloudCalPhase       = CloudCalUiPhase::WAITING;
                    gCloudCalWaitStartMs = now;
                    display::clear();
                }
                return;
            }

            // Cloud calibration result UI — takes priority over everything else,
            // including the normal link-timeout screen (see linkAlive above).
            if (gCloudCalPhase != CloudCalUiPhase::NONE) {
                switch (gCloudCalPhase) {
                    case CloudCalUiPhase::WAITING:
                        if (now - gCloudCalWaitStartMs >= CLOUD_CAL_WAIT_TIMEOUT_MS) {
                            gCloudCalPhase = CloudCalUiPhase::FAILED;
                            strncpy(gCloudCalError, "No response from nav device",
                                    sizeof(gCloudCalError) - 1);
                            gCloudCalError[sizeof(gCloudCalError) - 1] = '\0';
                            display::showCloudCalFailed(gCloudCalError);
                        } else {
                            display::showCloudCalWaiting((now - gCloudCalWaitStartMs) / 1000);
                        }
                        break;
                    case CloudCalUiPhase::OFFLINE:
                        display::showCloudCalOffline();
                        break;
                    case CloudCalUiPhase::FAILED:
                        display::showCloudCalFailed(gCloudCalError);
                        break;
                    case CloudCalUiPhase::RESULT:
                        if (gCloudCalInstallable) {
                            display::showCloudCalResult(gCloudCalQuality, gCloudCalRmsPct,
                                                        gCloudCalRecommendation, gCloudCalCoverageGaps,
                                                        gCloudCalChoice, gCloudCalType);
                        } else {
                            // Gap-fill: informational only, no accept/reject
                            // choice to make on-device -- this fit is a merge
                            // candidate, not a thing to install. Reject/discard
                            // isn't offered either: leaving the row at
                            // accepted=null is exactly its correct resting
                            // state until the diver resolves it on the website
                            // (by merging it in, or ignoring it).
                            display::showGapFillUploaded(gCloudCalRmsPct, gCloudCalRecommendation,
                                                         gCloudCalCoverageGaps);
                        }
                        break;
                    default:
                        break;
                }
                return;
            }

            // Cloud account-link UI — takes priority over everything else,
            // including the normal link-timeout screen (see linkAlive above).
            if (gCloudLinkPhase != CloudLinkUiPhase::NONE) {
                switch (gCloudLinkPhase) {
                    case CloudLinkUiPhase::STARTING:
                        if (now - gCloudLinkWaitStartMs >= CLOUD_LINK_WAIT_TIMEOUT_MS) {
                            gCloudLinkPhase = CloudLinkUiPhase::FAILED;
                            strncpy(gCloudLinkError, "No response from nav device",
                                    sizeof(gCloudLinkError) - 1);
                            gCloudLinkError[sizeof(gCloudLinkError) - 1] = '\0';
                            display::showCloudLinkFailed(gCloudLinkError);
                        } else {
                            display::showCloudLinkWaiting("", (now - gCloudLinkWaitStartMs) / 1000);
                        }
                        break;
                    case CloudLinkUiPhase::WAITING:
                        if (now - gCloudLinkWaitStartMs >= CLOUD_LINK_WAIT_TIMEOUT_MS) {
                            gCloudLinkPhase = CloudLinkUiPhase::FAILED;
                            strncpy(gCloudLinkError, "No response from nav device",
                                    sizeof(gCloudLinkError) - 1);
                            gCloudLinkError[sizeof(gCloudLinkError) - 1] = '\0';
                            display::showCloudLinkFailed(gCloudLinkError);
                        } else {
                            display::showCloudLinkWaiting(gCloudLinkUserCode,
                                                          (now - gCloudLinkWaitStartMs) / 1000);
                        }
                        break;
                    case CloudLinkUiPhase::OFFLINE:
                        display::showCloudLinkOffline();
                        break;
                    case CloudLinkUiPhase::FAILED:
                        display::showCloudLinkFailed(gCloudLinkError);
                        break;
                    case CloudLinkUiPhase::DONE:
                        display::showCloudLinkDone();
                        break;
                    default:
                        break;
                }
                return;
            }

            // Active bin cal progress — takes priority over all other rendering.
            // Baseline (always ROUGH_SCAN) gets its own screen, no grid;
            // mounted (COLLECT) and gap-fill (GAP_FILL) use the grid.
            if (calProgressValid) {
                renderCalProgress(lastCalProgress);
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

            display::setImperialUnits(menu::settings().imperial);
            display::setRawHeading(menu::settings().headingMode == nvs_disp::HEADING_RAW);
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
            menu::updateNavState(lastNav.flags, lastNav.flags2);
            if (gBootPhase == BootPhase::LOGO) {
                // Only switch to STATUS if the logo minimum hold has elapsed;
                // otherwise navValid is now true and loop() will handle it at the 3 s mark.
                if (millis() - gLogoStartMs >= LOGO_MIN_MS) {
                    gBootPhase  = BootPhase::STATUS;
                    gBootShowMs = millis();
                    display::showBootStatus(lastNav.boot_flags);
                }
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
                gBaselineFinishPending = false;
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
    } else if (ptype == PacketType::CAL_CLOUD_RESULT) {
        CalCloudResultPacket pkt{};
        if (bytesToCalCloudResultPacket(rxBuf, rxPos, pkt)) {
            lastNavMs = millis();  // keep link-alive timer refreshed
            if (pkt.stage == (uint8_t)CalCloudStage::OFFLINE) {
                gCloudCalPhase = CloudCalUiPhase::OFFLINE;
            } else if (pkt.stage == (uint8_t)CalCloudStage::FAILED) {
                gCloudCalPhase = CloudCalUiPhase::FAILED;
                strncpy(gCloudCalError, pkt.error, sizeof(gCloudCalError) - 1);
                gCloudCalError[sizeof(gCloudCalError) - 1] = '\0';
            } else {
                gCloudCalPhase   = CloudCalUiPhase::RESULT;
                gCloudCalQuality = pkt.quality;
                gCloudCalType    = pkt.cal_type;
                gCloudCalRmsPct  = pkt.rms_pct;
                gCloudCalCoverageGaps = pkt.coverage_gaps;
                gCloudCalInstallable  = pkt.installable;
                gCloudCalChoice  = 0;
                strncpy(gCloudCalRecommendation, pkt.recommendation, sizeof(gCloudCalRecommendation) - 1);
                gCloudCalRecommendation[sizeof(gCloudCalRecommendation) - 1] = '\0';
                strncpy(gCloudCalId, pkt.calibration_id, sizeof(gCloudCalId) - 1);
                gCloudCalId[sizeof(gCloudCalId) - 1] = '\0';
            }
            Serial.printf("[CLOUD_CAL] result stage=%u\n", pkt.stage);
        }
    } else if (ptype == PacketType::CLOUD_LINK_RESULT) {
        CloudLinkResultPacket pkt{};
        if (bytesToCloudLinkResultPacket(rxBuf, rxPos, pkt)) {
            lastNavMs = millis();  // keep link-alive timer refreshed
            if (pkt.stage == (uint8_t)CloudLinkStage::OFFLINE) {
                gCloudLinkPhase = CloudLinkUiPhase::OFFLINE;
            } else if (pkt.stage == (uint8_t)CloudLinkStage::FAILED) {
                gCloudLinkPhase = CloudLinkUiPhase::FAILED;
                strncpy(gCloudLinkError, pkt.error, sizeof(gCloudLinkError) - 1);
                gCloudLinkError[sizeof(gCloudLinkError) - 1] = '\0';
            } else if (pkt.stage == (uint8_t)CloudLinkStage::CODE_READY) {
                gCloudLinkPhase = CloudLinkUiPhase::WAITING;
                strncpy(gCloudLinkUserCode, pkt.user_code, sizeof(gCloudLinkUserCode) - 1);
                gCloudLinkUserCode[sizeof(gCloudLinkUserCode) - 1] = '\0';
            } else {
                gCloudLinkPhase = CloudLinkUiPhase::DONE;
            }
            Serial.printf("[CLOUD_LINK] result stage=%u\n", pkt.stage);
        }
    } else if (ptype == PacketType::BOOT_PING) {
        // Nav's boot self-test round-trip check (see setup() in nav_main.cpp)
        // -- echo back immediately so nav can confirm display->nav is alive.
        lastNavMs = millis();
        sendCmd(DisplayCmd::LINK_HELLO);
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

static void sendCloudCalRespond(DisplayCmd cmd, const char* calId) {
    size_t n = displayCloudCalRespondToBytes(cmd, calId, txBuf, sizeof(txBuf));
    if (n > 0) Serial1.write(txBuf, n);
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
// Swallow any short-press release that is still pending. Every modal screen
// below returns early, and most of them only ever consume the one button they
// care about -- the other button's release then sits with fired == false and
// fires into the menu the instant the modal exits, opening the menu or
// advancing an item the diver never pressed for. Anything that claims a pass
// must claim both buttons.
static void consumePendingReleases() {
    if (!btn1.pressed && btn1.pressStartMs > 0) btn1.fired = true;
    if (!btn2.pressed && btn2.pressStartMs > 0) btn2.fired = true;
}

// Full-screen modes that own the buttons. Returns true if one of them handled
// (or deliberately ignored) this pass, in which case the menu must not see it.
static bool handleModalButtons() {
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
            return true;
        }
    }

    // --- Baseline / gap-fill: BTN2 declares collection done, ends the session ---
    // Gap-fill needs this as much as baseline does: a targeted cell can simply
    // be unreachable in the water (a ceiling, a wall, a scooter you can't
    // invert), and without a manual finish the session would never end. It also
    // auto-completes when every target is satisfied -- this is the escape
    // hatch, not the only exit.
    if (calProgressValid &&
        (lastCalProgress.phase == (uint8_t)CalPhase::ROUGH_SCAN ||
         lastCalProgress.phase == (uint8_t)CalPhase::GAP_FILL)) {
        if (gBaselineFinishPending) {
            // Already requested -- ignore repeat presses until nav confirms
            // (complete=true) or the pending screen times out.
            return true;
        }
        if (!btn2.pressed && !btn2.fired && btn2.pressStartMs > 0) {
            btn2.fired = true;
            sendCmd(DisplayCmd::FINISH_BASELINE_COLLECTION);
            // nav may still ignore this (too few samples); the pending screen
            // times out below if no completion packet ever arrives.
            gBaselineFinishPending   = true;
            gBaselineFinishPendingMs = millis();
            display::clear();
            Serial.println("[BIN_CAL] BTN2: requested finish baseline collection");
        }
        return true;
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
                gHdgFourierCalPhase = HdgFourierCalPhase::NONE;
                // Nav is about to CSV-dump + (if online) blocking-upload the
                // heading samples -- hand off to the same wait/result UI
                // baseline/mounted's bin-grid completion already uses, rather
                // than a separate local "done" screen. See
                // docs/architecture/heading-cal-cloud-plan.md.
                gCloudCalPhase       = CloudCalUiPhase::WAITING;
                gCloudCalWaitStartMs = millis();
                display::clear();
            }
        }
        return true;
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
        return true;
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
        return true;
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
        return true;
    }

    // --- Cloud calibration result UI -----------------------------------------
    if (gCloudCalPhase == CloudCalUiPhase::OFFLINE || gCloudCalPhase == CloudCalUiPhase::FAILED) {
        // BTN2: dismiss
        if (!btn2.pressed && !btn2.fired && btn2.pressStartMs > 0) {
            btn2.fired = true;
            gCloudCalPhase = CloudCalUiPhase::NONE;
            display::clear();
        }
        return true;
    }
    if (gCloudCalPhase == CloudCalUiPhase::RESULT) {
        if (!gCloudCalInstallable) {
            // Gap-fill: single dismiss, no ACCEPT/REJECT command ever sent.
            // The uploaded fit stays parked at accepted=null on the server --
            // its correct resting state until the website resolves it.
            if (!btn2.pressed && !btn2.fired && btn2.pressStartMs > 0) {
                btn2.fired = true;
                gCloudCalPhase = CloudCalUiPhase::NONE;
                display::clear();
            }
            return true;
        }
        // BTN1: cycle ACCEPT/REJECT
        if (!btn1.pressed && !btn1.fired && btn1.pressStartMs > 0) {
            btn1.fired = true;
            gCloudCalChoice = (gCloudCalChoice + 1) % 2;
        }
        // BTN2: confirm choice
        if (!btn2.pressed && !btn2.fired && btn2.pressStartMs > 0) {
            btn2.fired = true;
            DisplayCmd cmd = (gCloudCalChoice == 0) ? DisplayCmd::ACCEPT_CLOUD_CAL
                                                     : DisplayCmd::REJECT_CLOUD_CAL;
            sendCloudCalRespond(cmd, gCloudCalId);
            Serial.printf("[CLOUD_CAL] choice: %s\n", gCloudCalChoice == 0 ? "ACCEPT" : "REJECT");
            gCloudCalPhase = CloudCalUiPhase::NONE;
            display::clear();
        }
        return true;
    }
    // WAITING: no button action -- the nav device is mid-upload and won't see
    // anything sent right now anyway (it's blocked on the network call).
    if (gCloudCalPhase == CloudCalUiPhase::WAITING) {
        return true;
    }

    // --- Cloud account-link UI ------------------------------------------------
    if (gCloudLinkPhase == CloudLinkUiPhase::OFFLINE || gCloudLinkPhase == CloudLinkUiPhase::FAILED ||
        gCloudLinkPhase == CloudLinkUiPhase::DONE) {
        // BTN2: dismiss
        if (!btn2.pressed && !btn2.fired && btn2.pressStartMs > 0) {
            btn2.fired = true;
            gCloudLinkPhase = CloudLinkUiPhase::NONE;
            display::clear();
        }
        return true;
    }
    // STARTING/WAITING: BTN2 cancels. The nav-side poll is a non-blocking
    // state machine (see cloud_client.h), so CANCEL_LINK reaches it and
    // takes effect immediately instead of queuing behind a blocked call.
    if (gCloudLinkPhase == CloudLinkUiPhase::STARTING || gCloudLinkPhase == CloudLinkUiPhase::WAITING) {
        if (!btn2.pressed && !btn2.fired && btn2.pressStartMs > 0) {
            btn2.fired = true;
            sendCmd(DisplayCmd::CANCEL_LINK);
            gCloudLinkPhase = CloudLinkUiPhase::NONE;
            display::clear();
        }
        return true;
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
        return true;
    }

    return false;  // no modal active — the menu gets this pass
}

static void handleButtons() {
    if (handleModalButtons()) {
        consumePendingReleases();
        return;
    }

    // Snapshot before BTN1 runs: BTN1 may open the menu in this very pass, and
    // BTN2 must not be allowed to select whatever item that lands on. A quick
    // two-button tap (the same gesture that wakes the unit) released both
    // buttons inside one debounce window, so BTN1's open and BTN2's select ran
    // back to back and fired root item 0 sight-unseen.
    const bool menuWasOpenOnEntry = menu::isOpen();

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
        if (menuWasOpenOnEntry && menu::isOpen()) {
            menu::select();
        }
        // When the menu is closed — or was only just opened by BTN1 above —
        // BTN2 has no action.
    }
}
