#pragma once

// I2C bus bring-up with stuck-bus recovery.
//
// If the ESP32 resets (or is reflashed) while an I2C slave is mid-transaction,
// the slave can be left holding SDA low waiting for more clock pulses that
// never come. The ESP32 resetting doesn't clear this — the slave never saw
// it reset — so Wire.begin() alone comes back up against a permanently wedged
// bus: every transaction times out (ESP_ERR_TIMEOUT) on every address, which
// looks exactly like dead/disconnected sensors.
//
// begin() checks for this before handing the pins to Wire: if SDA is stuck
// low, it bit-bangs up to 9 clock pulses on SCL (enough to flush any partial
// byte a slave thinks it's mid-send) followed by a manual STOP condition,
// then proceeds with the normal Wire.begin().
namespace i2c_bus {

void begin(int sda_pin, int scl_pin);

}  // namespace i2c_bus
