#include "i2c_bus.h"
#include <Arduino.h>
#include <Wire.h>

namespace i2c_bus {

namespace {
constexpr int CLOCK_HALF_PERIOD_US = 5;  // ~100 kHz bit-bang timing
constexpr int MAX_RECOVERY_CLOCKS  = 9;  // worst case: one stuck byte + ACK bit
}  // namespace

void begin(int sda_pin, int scl_pin) {
  pinMode(sda_pin, INPUT_PULLUP);
  pinMode(scl_pin, INPUT_PULLUP);
  delayMicroseconds(CLOCK_HALF_PERIOD_US);

  if (digitalRead(sda_pin) == LOW) {
    Serial.println("[I2C] SDA held low at boot -- running bus recovery");

    pinMode(scl_pin, OUTPUT);
    for (int i = 0; i < MAX_RECOVERY_CLOCKS && digitalRead(sda_pin) == LOW; i++) {
      digitalWrite(scl_pin, LOW);
      delayMicroseconds(CLOCK_HALF_PERIOD_US);
      digitalWrite(scl_pin, HIGH);
      delayMicroseconds(CLOCK_HALF_PERIOD_US);
    }

    // Manual STOP condition: SDA low-to-high while SCL is high.
    pinMode(sda_pin, OUTPUT);
    digitalWrite(sda_pin, LOW);
    delayMicroseconds(CLOCK_HALF_PERIOD_US);
    digitalWrite(scl_pin, HIGH);
    delayMicroseconds(CLOCK_HALF_PERIOD_US);
    digitalWrite(sda_pin, HIGH);
    delayMicroseconds(CLOCK_HALF_PERIOD_US);

    pinMode(sda_pin, INPUT_PULLUP);
    pinMode(scl_pin, INPUT_PULLUP);

    if (digitalRead(sda_pin) == LOW) {
      Serial.println("[I2C] Bus recovery FAILED -- SDA still stuck low (check wiring/power)");
    } else {
      Serial.println("[I2C] Bus recovery OK");
    }
  }

  Wire.begin(sda_pin, scl_pin);
}

}  // namespace i2c_bus
