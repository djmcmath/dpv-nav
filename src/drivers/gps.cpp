#include "gps.h"
#include "../board_pins.h"
#include <Adafruit_GPS.h>
#include <Arduino.h>

static Adafruit_GPS adaGps(&Serial2);
static bool initialized = false;
static bool enabled = true;

namespace gps {

bool init() {
  Serial2.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  adaGps.begin(9600);

  // Request RMC (position + speed) and GGA (fix + altitude + satellites)
  adaGps.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);

  // 1 Hz update rate
  adaGps.sendCommand(PMTK_SET_NMEA_UPDATE_1HZ);

  initialized = true;
  Serial.println("GPS: initialized on Serial2 (9600 baud)");
  return true;
}

void setEnabled(bool enable) {
  enabled = enable;
  Serial.print("[GPS] "); Serial.println(enabled ? "Enabled" : "Disabled");
}

bool update() {
  if (!initialized || !enabled) return false;

  // Read all available bytes from GPS serial (non-blocking)
  while (Serial2.available()) {
    adaGps.read();
  }

  // Check if a complete NMEA sentence was received and parse it
  if (adaGps.newNMEAreceived()) {
    return adaGps.parse(adaGps.lastNMEA());
  }

  return false;
}

GpsFix getFix() {
  GpsFix fix{};

  if (!initialized || !enabled) return fix;

  fix.has_fix = adaGps.fix;
  fix.fix_quality = adaGps.fixquality;
  fix.satellites = adaGps.satellites;

  if (adaGps.fix) {
    // Adafruit_GPS stores latitude/longitude in degrees + minutes format
    // latitudeDegrees/longitudeDegrees are already converted to decimal degrees
    fix.lat = adaGps.latitudeDegrees;
    fix.lon = adaGps.longitudeDegrees;
    fix.altitude_m = adaGps.altitude;
    fix.speed_knots = adaGps.speed;
    fix.course_deg = adaGps.angle;
    fix.hdop = adaGps.HDOP;
    fix.fix_age_ms = millis();
  }

  return fix;
}

bool hasFix() {
  return initialized && enabled && adaGps.fix;
}

}
