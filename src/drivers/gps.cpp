#include "gps.h"
#include "../board_pins.h"
#include "../config.h"
#include <Adafruit_GPS.h>
#include <Arduino.h>

static Adafruit_GPS adaGps(&Serial2);
static bool initialized = false;
static bool enabled = true;
static char lastPgtopSentence[80] = {};
static bool rawNmeaDebug = false;

// --- Fix freshness ---------------------------------------------------------
// Adafruit_GPS retains its last parsed values indefinitely, and setEnabled(false)
// cuts power to the module without clearing them. Any read after a power cycle
// therefore sees the pre-dive position looking perfectly valid. These track when
// a position was *actually* parsed so a retained one cannot pass as a live fix.
static uint32_t lastFixMs   = 0;  // millis() when a position sentence last parsed with a fix
static uint16_t newFixCount = 0;  // such sentences seen since the last enable (saturates)

// GPGSV sentence cache — one group of sentences per 1 Hz update cycle.
// A group starts when sentence 1-of-N arrives; earlier cached lines are discarded.
static constexpr int MAX_GSV_LINES = 4;  // covers up to 16 sats (4 per sentence)
static char gsvLines[MAX_GSV_LINES][90] = {};
static int  gsvLineCount = 0;

namespace gps {

bool init() {
  // Drive enable pin HIGH before opening serial so the module powers up cleanly.
  pinMode(GPS_ENABLE_PIN, OUTPUT);
  digitalWrite(GPS_ENABLE_PIN, HIGH);

  Serial2.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  adaGps.begin(9600);

  // Request RMC, GGA, GSA, and GSV.
  // GSA populates fixquality_3d; GSV provides per-satellite SNR for diagnostics.
  // Checksum derived from ALLDATA (1,1,1,1,1,1→*28) with GLL+VTG cleared (two *0x01 = cancel).
  adaGps.sendCommand("$PMTK314,0,1,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0*28");

  // 1 Hz update rate
  adaGps.sendCommand(PMTK_SET_NMEA_UPDATE_1HZ);

  // Enable EASY (Embedded Assist System) — uses stored almanac to reduce TTFF.
  // Requires VBACKUP pin connected to a coin cell for almanac to persist across power cycles.
  // Only supported at 1 Hz (already set above). MTK3339-specific command.
  adaGps.sendCommand("$PMTK869,1,1*35");

  // Enable antenna status reporting via PGTOP sentences.
  // After this the module emits $PGTOP,11,x where x: 1=error, 2=internal, 3=active external.
  adaGps.sendCommand(PGCMD_ANTENNA);

  // Connectivity check: a live module emits checksummed NMEA sentences at
  // 1 Hz even with no satellite fix (GPGSA/GPGSV keep arriving fix-less).
  // Wait for at least one to actually parse before declaring success --
  // previously this returned true unconditionally, so a disconnected or
  // dead GPS module looked identical to a working one until someone
  // noticed a stale/missing fix mid-dive-planning.
  constexpr uint32_t CONNECT_TIMEOUT_MS = 2500;
  uint32_t start = millis();
  bool sawValidSentence = false;
  while (millis() - start < CONNECT_TIMEOUT_MS) {
    while (Serial2.available()) {
      adaGps.read();
      if (adaGps.newNMEAreceived() && adaGps.parse(adaGps.lastNMEA())) {
        sawValidSentence = true;
      }
    }
    if (sawValidSentence) break;
    delay(5);
  }

  initialized = sawValidSentence;
  if (sawValidSentence) {
    Serial.println("GPS: initialized on Serial2 (9600 baud)");
  } else {
    Serial.println("GPS: no valid NMEA sentence received in 2.5s -- module not responding");
  }
  return sawValidSentence;
}

void setEnabled(bool enable) {
  // On the disabled->enabled edge, discard everything the parser is still
  // holding from before the module lost power. Without this the first getFix()
  // after surfacing returns the position from the *previous* surfacing -- which
  // is what GPS_FIX_STALE_MS is meant to catch and could not, because
  // fix_age_ms used to be stamped at read time rather than at parse time.
  if (enable && !enabled) {
    lastFixMs            = 0;
    newFixCount          = 0;
    adaGps.fix           = false;
    adaGps.fixquality    = 0;
    adaGps.fixquality_3d = 1;  // 1 = no fix
    adaGps.satellites    = 0;
    adaGps.HDOP          = 0.0f;
  }
  enabled = enable;
  digitalWrite(GPS_ENABLE_PIN, enable ? HIGH : LOW);
  Serial.print("[GPS] "); Serial.println(enabled ? "Enabled" : "Disabled");
}

bool update() {
  if (!initialized || !enabled) return false;

  // Process one byte at a time so every complete sentence is handled before the
  // next one overwrites lastNMEA(). The old drain-then-check approach lost any
  // sentence that arrived earlier in the burst (e.g. PGTOP before GPRMC).
  bool parsed = false;
  while (Serial2.available()) {
    adaGps.read();
    if (adaGps.newNMEAreceived()) {
      char* sentence = adaGps.lastNMEA();
      if (rawNmeaDebug) Serial.printf("[NMEA] %s\n", sentence);
      if (strncmp(sentence, "$PGTOP", 6) == 0) {
        strncpy(lastPgtopSentence, sentence, sizeof(lastPgtopSentence) - 1);
        lastPgtopSentence[sizeof(lastPgtopSentence) - 1] = '\0';
      }
      // Cache GPGSV sentences. The group header ($GPGSV,N,1,...) resets the
      // cache so stale lines from the previous second are discarded on new data.
      if (strncmp(sentence, "$GPGSV", 6) == 0 || strncmp(sentence, "$GLGSV", 6) == 0
          || strncmp(sentence, "$GNGSV", 6) == 0) {
        // Field 2 is the sentence index within the group (1-based).
        // Find the second comma to reach field 2.
        const char* p = sentence;
        int commas = 0;
        while (*p && commas < 2) { if (*p++ == ',') commas++; }
        if (*p == '1') gsvLineCount = 0;  // first sentence of group — reset
        if (gsvLineCount < MAX_GSV_LINES) {
          strncpy(gsvLines[gsvLineCount], sentence, sizeof(gsvLines[0]) - 1);
          gsvLines[gsvLineCount][sizeof(gsvLines[0]) - 1] = '\0';
          gsvLineCount++;
        }
      }
      // Only RMC/GGA carry a position. GSA/GSV parse fine but leave adaGps.fix
      // untouched, so counting them would let the reacquire guard be satisfied
      // without a single new position actually having arrived.
      const bool posSentence = (strstr(sentence, "RMC") != nullptr) ||
                               (strstr(sentence, "GGA") != nullptr);
      if (adaGps.parse(sentence)) {
        parsed = true;
        if (posSentence && adaGps.fix) {
          lastFixMs = millis();
          if (newFixCount < GPS_REACQUIRE_MIN_FIXES) newFixCount++;
        }
      }
    }
  }
  return parsed;
}

GpsFix getFix() {
  GpsFix fix{};

  if (!initialized || !enabled) return fix;

  // Position is withheld until the module has produced enough genuinely new
  // position sentences since the last enable (see setEnabled). Satellite count
  // and fix quality are passed through unguarded -- they were cleared on enable,
  // so they already reflect only post-power-up data and are useful for showing
  // reacquisition progress on the display.
  fix.has_fix = adaGps.fix && (newFixCount >= GPS_REACQUIRE_MIN_FIXES);
  fix.fix_quality  = adaGps.fixquality;
  fix.fix_type_3d  = adaGps.fixquality_3d;  // 1=no fix, 2=2D, 3=3D (from GSA)
  fix.satellites   = adaGps.satellites;

  if (fix.has_fix) {
    // Adafruit_GPS stores latitude/longitude in degrees + minutes format
    // latitudeDegrees/longitudeDegrees are already converted to decimal degrees
    fix.lat = adaGps.latitudeDegrees;
    fix.lon = adaGps.longitudeDegrees;
    fix.altitude_m = adaGps.altitude;
    fix.speed_knots = adaGps.speed;
    fix.course_deg = adaGps.angle;
    fix.hdop = adaGps.HDOP;
    fix.fix_age_ms = lastFixMs;  // when it was parsed, not when it was read
  }

  // UTC time is parsed from RMC sentences regardless of position fix status.
  // year is 2-digit (e.g. 25 for 2025); has_time is set once a non-zero year arrives.
  fix.utc_year   = adaGps.year;
  fix.utc_month  = adaGps.month;
  fix.utc_day    = adaGps.day;
  fix.utc_hour   = adaGps.hour;
  fix.utc_minute = adaGps.minute;
  fix.utc_second = adaGps.seconds;
  fix.has_time   = (adaGps.year > 0);

  fix.antenna_status = adaGps.antenna;

  return fix;
}

bool hasFix() {
  return initialized && enabled && adaGps.fix &&
         (newFixCount >= GPS_REACQUIRE_MIN_FIXES);
}

uint8_t computeSignalBars(const GpsFix& fix) {
  // Hard cap: no fix at all → 0 bars immediately.
  if (!fix.has_fix || fix.fix_type_3d <= 1) return 0;

  // --- Component scores [0.0, 1.0] ---

  // Fix type (2D vs 3D)
  float fixScore = (fix.fix_type_3d == 3) ? GPS_SCORE_FIX_3D : GPS_SCORE_FIX_2D;

  // HDOP (lower is better)
  float hdopScore;
  float h = fix.hdop;
  if      (h <= GPS_HDOP_THRESH_1) hdopScore = GPS_HDOP_SCORE_1;
  else if (h <= GPS_HDOP_THRESH_2) hdopScore = GPS_HDOP_SCORE_2;
  else if (h <= GPS_HDOP_THRESH_3) hdopScore = GPS_HDOP_SCORE_3;
  else if (h <= GPS_HDOP_THRESH_4) hdopScore = GPS_HDOP_SCORE_4;
  else if (h <= GPS_HDOP_THRESH_5) hdopScore = GPS_HDOP_SCORE_5;
  else                              hdopScore = GPS_HDOP_SCORE_BAD;

  // Satellites used
  float satScore;
  uint8_t s = fix.satellites;
  if      (s <= 3) satScore = GPS_SAT_SCORE_LE3;
  else if (s == 4) satScore = GPS_SAT_SCORE_4;
  else if (s == 5) satScore = GPS_SAT_SCORE_5;
  else if (s == 6) satScore = GPS_SAT_SCORE_6;
  else if (s == 7) satScore = GPS_SAT_SCORE_7;
  else             satScore = GPS_SAT_SCORE_GE8;

  // Weighted composite (SNR excluded — not available from Adafruit GPS library)
  float score = fixScore  * GPS_WEIGHT_FIX
              + hdopScore * GPS_WEIGHT_HDOP
              + satScore  * GPS_WEIGHT_SATS;

  // --- Sanity caps ---
  int maxBars = 4;
  if (fix.fix_type_3d == 2)        maxBars = GPS_CAP_2D_MAX_BARS;   // 2D fix → max 2
  if (fix.hdop > GPS_CAP_HDOP_THRESH) {                              // HDOP > 2.5 → max 1
    if (GPS_CAP_HDOP_MAX_BARS < maxBars) maxBars = GPS_CAP_HDOP_MAX_BARS;
  }

  // Map score [0.0, 1.0] → bars [0, 4]
  int bars = (int)(score * 4.0f + 0.5f);
  if (bars < 0)       bars = 0;
  if (bars > 4)       bars = 4;
  if (bars > maxBars) bars = maxBars;

  return (uint8_t)bars;
}

const char* getLastPgtopSentence() { return lastPgtopSentence; }

int         getGsvLineCount()        { return gsvLineCount; }
const char* getGsvLine(int idx)      { return (idx >= 0 && idx < gsvLineCount) ? gsvLines[idx] : ""; }

void setRawNmeaDebug(bool enable) { rawNmeaDebug = enable; }

}
