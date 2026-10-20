#include "status_led.h"
#include "../board_pins.h"
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <driver/rtc_io.h>

namespace status_led {

static Adafruit_NeoPixel pixel(1, NAV_NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
static Color shown{0, 0, 0};
static bool  ready = false;

void init() {
    // GPIO2 powers the pixel (and the STEMMA QT port). sleepForPowerOff() left
    // it latched low; nothing else releases that latch, so do it before driving.
    rtc_gpio_hold_dis(static_cast<gpio_num_t>(NAV_NEOPIXEL_POWER_PIN));
    rtc_gpio_deinit(static_cast<gpio_num_t>(NAV_NEOPIXEL_POWER_PIN));
    pinMode(NAV_NEOPIXEL_POWER_PIN, OUTPUT);
    digitalWrite(NAV_NEOPIXEL_POWER_PIN, HIGH);

    pixel.begin();
    pixel.setBrightness(255);  // levels are absolute, see status_led.h
    pixel.setPixelColor(0, 0);
    pixel.show();
    shown = OFF;
    ready = true;
}

void show(Color c) {
    if (!ready || c == shown) return;
    pixel.setPixelColor(0, pixel.Color(c.r, c.g, c.b));
    pixel.show();
    shown = c;
}

void sleepForPowerOff() {
    if (ready) {
        pixel.setPixelColor(0, 0);
        pixel.show();
    }
    ready = false;

    // Data line to high-Z, never held low: GPIO0 is the boot strapping pin, and
    // a wake with it held low would come up in the ROM download mode instead
    // of the firmware. The board's own pull-up keeps it high.
    pinMode(NAV_NEOPIXEL_PIN, INPUT);

    // Power rail off and latched off through deep sleep -- an ordinary pad goes
    // high-Z there. Single-pin RTC latch, deliberately not
    // gpio_deep_sleep_hold_en(), same reasoning as display::sleepForPowerOff().
    rtc_gpio_init(static_cast<gpio_num_t>(NAV_NEOPIXEL_POWER_PIN));
    rtc_gpio_set_direction(static_cast<gpio_num_t>(NAV_NEOPIXEL_POWER_PIN),
                           RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(static_cast<gpio_num_t>(NAV_NEOPIXEL_POWER_PIN), 0);
    rtc_gpio_hold_en(static_cast<gpio_num_t>(NAV_NEOPIXEL_POWER_PIN));
}

}  // namespace status_led
