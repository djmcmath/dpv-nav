// GPS fix-quality indicator — QT Py ESP32-S2, gps_carrier board (see
// hardware/schematic/gps_board/README.md for the wiring this depends on).
//
// This device only LISTENS to the GPS. GPS RX is wired to the main nav
// board's TX (see schematic); QT Py TX is unconnected, so this firmware
// must never try to configure the module (no PMTK sendCommand calls) --
// whatever sentences the nav board enabled are all this ever sees.
//
// Drives two outputs from one state: the onboard NeoPixel (color + blink),
// and the single-color external LED on A0/Q1, which can't show color, so
// it just mirrors the NeoPixel's on/off phase.

#include <Adafruit_GPS.h>
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

static HardwareSerial GpsSerial(1);
static Adafruit_GPS adaGps(&GpsSerial);
static Adafruit_NeoPixel pixel(NEOPIXEL_NUM, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

// --- Fault detection ---------------------------------------------------
// "No connection" and "module died mid-run" are the same symptom (no valid
// NMEA for too long), just with a different grace period: a cold module
// needs a few seconds to say anything at all, but once it's been talking,
// going quiet for even a couple of seconds means the link is gone.
static constexpr uint32_t NMEA_BOOT_TIMEOUT_MS  = 8000;
static constexpr uint32_t NMEA_STALE_TIMEOUT_MS = 5000;

// --- HDOP tiers ----------------------------------------------------------
// Matched to the nav board's real GPS-accept logic (config.h on the nav
// board; this file has no access to it, so the numbers are restated here --
// keep them in sync by hand if those ever move):
//   - GOOD_MAX = 2.5 is nav's GPS_CAP_HDOP_THRESH: above it, nav caps the fix
//     to 1 bar, which can never clear the 2-bar floor needed to update
//     position. At or below it, this LED is reporting exactly the fixes nav
//     could accept (modulo the fix-type/satellite terms of its bar score).
//   - POOR_MAX = 4.0 matches GPS_HDOP_SCORE_BAD's threshold: nav's HDOP score
//     bottoms out to 0 past this point, so above it there's no path to an
//     accepted fix regardless of fix type or satellite count.
static constexpr float HDOP_GOOD_MAX = 2.5f;
static constexpr float HDOP_POOR_MAX = 4.0f;
static constexpr uint32_t GOOD_HOLD_MS = 10000;

// --- Blink timing (half-period; full cycle is 2x) -----------------------
static constexpr uint32_t ALARM_HALF_MS = 100;   // fault: red/yellow swap
static constexpr uint32_t SLOW_HALF_MS  = 500;   // searching / ok
static constexpr uint32_t FAST_HALF_MS  = 125;   // poor
static constexpr uint32_t GOOD_HALF_MS  = 250;   // good, still holding

static uint32_t lastSentenceMs = 0;
static bool sawAnySentence = false;
static uint32_t goodSinceMs = 0;

enum class IndicatorState { Fault, Searching, Poor, Ok, GoodHolding, GoodSolid };

static IndicatorState computeState(uint32_t now) {
  bool neverSawSentence = !sawAnySentence;
  bool staleSinceLastSentence = sawAnySentence && (now - lastSentenceMs >= NMEA_STALE_TIMEOUT_MS);
  if ((neverSawSentence && now >= NMEA_BOOT_TIMEOUT_MS) || staleSinceLastSentence) {
    goodSinceMs = 0;
    return IndicatorState::Fault;
  }

  if (!adaGps.fix) {
    goodSinceMs = 0;
    return IndicatorState::Searching;
  }

  float hdop = adaGps.HDOP;
  if (hdop <= 0.0f) {
    // Sentinel/not-yet-populated -- a fix this young can't be trusted as good.
    goodSinceMs = 0;
    return IndicatorState::Searching;
  }
  if (hdop > HDOP_POOR_MAX) {
    goodSinceMs = 0;
    return IndicatorState::Poor;
  }
  if (hdop > HDOP_GOOD_MAX) {
    goodSinceMs = 0;
    return IndicatorState::Ok;
  }

  if (goodSinceMs == 0) goodSinceMs = now;
  return (now - goodSinceMs >= GOOD_HOLD_MS) ? IndicatorState::GoodSolid
                                              : IndicatorState::GoodHolding;
}

static bool blinkOn(uint32_t now, uint32_t halfPeriodMs) {
  return (now % (2 * halfPeriodMs)) < halfPeriodMs;
}

static void render(IndicatorState state, uint32_t now) {
  uint32_t color = 0;
  bool on = true;

  switch (state) {
    case IndicatorState::Fault:
      color = blinkOn(now, ALARM_HALF_MS) ? pixel.Color(255, 0, 0) : pixel.Color(255, 180, 0);
      on = true;
      break;
    case IndicatorState::Searching:
      color = pixel.Color(255, 0, 0);
      on = blinkOn(now, SLOW_HALF_MS);
      break;
    case IndicatorState::Poor:
      color = pixel.Color(255, 0, 0);
      on = blinkOn(now, FAST_HALF_MS);
      break;
    case IndicatorState::Ok:
      color = pixel.Color(255, 180, 0);
      on = blinkOn(now, SLOW_HALF_MS);
      break;
    case IndicatorState::GoodHolding:
      color = pixel.Color(0, 255, 0);
      on = blinkOn(now, GOOD_HALF_MS);
      break;
    case IndicatorState::GoodSolid:
      color = pixel.Color(0, 255, 0);
      on = true;
      break;
  }

  pixel.setPixelColor(0, on ? color : 0);
  pixel.show();
  digitalWrite(A0, on ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200);

  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, NEOPIXEL_POWER_ON);
  pixel.begin();
  pixel.setBrightness(60);

  pinMode(A0, OUTPUT);
  digitalWrite(A0, LOW);

  // RX only -- see file header. -1 leaves TX unassigned.
  GpsSerial.begin(9600, SERIAL_8N1, RX, -1);
  adaGps.begin(9600);
}

void loop() {
  while (GpsSerial.available()) {
    adaGps.read();
    if (adaGps.newNMEAreceived() && adaGps.parse(adaGps.lastNMEA())) {
      lastSentenceMs = millis();
      sawAnySentence = true;
    }
  }

  uint32_t now = millis();
  IndicatorState state = computeState(now);
  render(state, now);

  static uint32_t lastDebugMs = 0;
  if (now - lastDebugMs >= 1000) {
    lastDebugMs = now;
    Serial.printf("[gps_indicator] state=%d fix=%d hdop=%.2f sats=%d\n",
                  (int)state, adaGps.fix, adaGps.HDOP, adaGps.satellites);
  }
}
