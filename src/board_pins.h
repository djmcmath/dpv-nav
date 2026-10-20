#pragma once

// Nav and display are separate physical boards and can be on different
// Feather revisions independently -- set each one's here. V2 moved SDA/SCL,
// the RX/TX header pins, and MOSI/MISO off their V1 GPIOs (confirmed against
// Adafruit's pinout pages for both boards); V2 also doesn't expose GPIO16/17
// at all (likely claimed internally by the V2 module's PSRAM), so configuring
// a UART on those pins on a V2 board isn't just mis-wired, it risks fighting
// the PSRAM bus.
#define NAV_FEATHER_V1     0
#define DISPLAY_FEATHER_V1 1
#ifdef BUILD_NAV
#define FEATHER_V1 NAV_FEATHER_V1
#else  // BUILD_DISPLAY
#define FEATHER_V1 DISPLAY_FEATHER_V1
#endif

// --- I2C bus for IMU (nav device only) ---
#if FEATHER_V1
constexpr int SDA_PIN = 23;
constexpr int SCL_PIN = 22;
#else // Feather v2 pins
constexpr int SDA_PIN = 22;
constexpr int SCL_PIN = 20;
#endif

// --- GPS UART (Adafruit Ultimate GPS v3 on Serial2, nav device only) ---
#if FEATHER_V1
constexpr int GPS_RX_PIN     = 16;  // ESP32 RX (GPIO 16) <- GPS TX
constexpr int GPS_TX_PIN     = 17;  // ESP32 TX (GPIO 17) -> GPS RX
#else // Feather v2: header RX/TX pins are GPIO7/8, not 16/17
constexpr int GPS_RX_PIN     = 7;
constexpr int GPS_TX_PIN     = 8;
#endif
constexpr int GPS_ENABLE_PIN = 32;  // HIGH = GPS on, LOW = GPS off (fix retained by backup battery); unchanged V1/V2

// --- Onboard NeoPixel (nav device only; every nav board is a Feather V2) ---
// Data on GPIO0, power on GPIO2 (Adafruit's NEOPIXEL_I2C_POWER), which also
// feeds the STEMMA QT port -- so the rail is only ever cut at power-off. Both
// are strapping pins; driving them after boot is the board's intended use.
// See util/status_led.h.
constexpr int NAV_NEOPIXEL_PIN       = 0;
constexpr int NAV_NEOPIXEL_POWER_PIN = 2;

// --- Inter-device serial link (Serial1) ---
// Nav device uses GPIO25/26 (A1/A0 -- unchanged between V1 and V2; GPS occupies the RX/TX header pins instead).
// Display device uses the RX/TX header pins (no GPS on display board).
#ifdef BUILD_NAV
constexpr int LINK_TX_PIN = 25; //connect to DISPLAY's RX header pin, pin 25 (labeled A1)
constexpr int LINK_RX_PIN = 26; //connect to DISPLAY's TX header pin, pin 26 (labeled A0)
#else  // BUILD_DISPLAY
#if FEATHER_V1
constexpr int LINK_TX_PIN = 17;
constexpr int LINK_RX_PIN = 16;
#else // Feather v2
constexpr int LINK_TX_PIN = 8;
constexpr int LINK_RX_PIN = 7;
#endif
#endif
constexpr uint32_t LINK_BAUD = 115200;

// --- Flow sensor (hall-effect pulse counting) ---
constexpr int FLOW_SENSOR_PIN = 34;  // Started with GPIO 25 (ADC2) but conflicts with WiFi

// --- Battery voltage ADC (Feather ESP32 v1: GPIO35 = A13, VBAT via 2:1 divider) ---
constexpr int BATT_ADC_PIN = 35;

// --- Buttons (external pull-up to 3.3V, active LOW when pressed) ---
constexpr int BUTTON1_PIN = 27;
constexpr int BUTTON2_PIN = 13;

// --- TFT display (ST7789 320x240 on SPI, direct-wired to ESP32) ---
constexpr int TFT_CS    = 15;
constexpr int TFT_DC    = 33;
// EyeSPI pad 8. This was 22 for a long time, which is a pin the display board
// does not route anywhere -- the panel's reset line floated and init() got by on
// the ST7789's software reset alone. The board has always had RST on IO23
// (display_board.kicad_pcb, net 4) on Feather V1; only this constant was wrong.
// Same GPIO the nav board uses for I2C SDA, which is not a conflict: they are
// different boards, and the display build compiles no I2C at all.
// On Feather V2, RST is wired to IO22 -- confirmed against the physical board,
// not a guess. (V2's GPIO22 is also the nav board's I2C SDA; again not a
// conflict, different boards.)
#if FEATHER_V1
constexpr int TFT_RST   = 23;
#else // Feather v2
constexpr int TFT_RST   = 22;
#endif
// Backlight enable -- EyeSPI pad 2, "Lite". The breakout pulls it high, so the
// backlight is lit whenever the panel has power and nothing drives this pin.
// Jumpered to A5/GPIO4, chosen because it is RTC-capable (so it can hold a level
// through deep sleep -- see display::sleepForPowerOff), LEDC-capable should we
// ever want PWM dimming, and not a strapping pin.
constexpr int TFT_BL    = 4;

// SPI bus pins (ESP32 default VSPI)
constexpr int DISP_SCK  = 5;  // unchanged V1/V2
#if FEATHER_V1
constexpr int DISP_MOSI = 18;
constexpr int DISP_MISO = 19;  // NC for display
#else // Feather v2
constexpr int DISP_MOSI = 19;
constexpr int DISP_MISO = 21;  // NC for display
#endif

// --- MCP23017 GPIO Expander — removed; display board now uses EyeSPI direct ---
// constexpr uint8_t MCP23017_ADDR = 0x20;
// constexpr uint8_t MCP_BACKLIGHT_PIN = 2;


/*

ChatGPT Recommendation for board pinout:
Direct to ESP32
I²C:
SDA → SDA / GPIO23
SCL → SCL / GPIO22
SPI (bus):
SCK → SCK / GPIO5
MOSI → MOSI / GPIO18
MISO → MISO / GPIO19 //not used for display (maybe for card, later?)

Display:
TFT_CS → GPIO15
TFT_DC → GPIO33
TFT_RST → GPIO32
TFT_BACKLIGHT → GPIO21 (or MCP23017 pin if you’d like)

SD card:
SD_CS → GPIO14

GPS:
GPS_TX → RX / GPIO16
GPS_RX → TX / GPIO17

Flowmeter:
FLOW_IN → A2 / GPIO34

Optional MCP23017 interrupt line:
MCP_INT → GPIO25 (or 26)

On MCP23017
Button 1 → MCP GPIO (e.g. GPA0)
Button 2 → MCP GPIO (e.g. GPA1)

Any extra LEDs / control lines you want to offload.
*/