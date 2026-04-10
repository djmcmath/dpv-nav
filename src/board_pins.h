#pragma once

// --- I2C bus for IMU --- Feather v1 pins
constexpr int SDA_PIN = 23;  // if shift to feather v2, this is GPIO22
constexpr int SCL_PIN = 22;  // if shift to feather v2, this is GPIO20

// --- GPS UART (Adafruit Ultimate GPS v3 on Serial2, nav device only) ---
constexpr int GPS_RX_PIN = 16;  // ESP32 RX (GPIO 16) <- GPS TX
constexpr int GPS_TX_PIN = 17;  // ESP32 TX (GPIO 17) -> GPS RX

// --- Inter-device serial link (Serial1) ---
// Nav device uses GPIO25/26 (GPS occupies 16/17).
// Display device uses GPIO16/17 (no GPS on display board).
#ifdef BUILD_NAV
constexpr int LINK_TX_PIN = 25; //connect to DISPLAY's RX pin 16, pin 25 is aka A1 and DAC1.
constexpr int LINK_RX_PIN = 26; //connect to DISPLAY's TX pin 17, pin 26 is aka A0 and DAC2
#else  // BUILD_DISPLAY
constexpr int LINK_TX_PIN = 17;
constexpr int LINK_RX_PIN = 16;
#endif
constexpr uint32_t LINK_BAUD = 115200;

// --- Flow sensor (hall-effect pulse counting) ---
constexpr int FLOW_SENSOR_PIN = 34;  // Started with GPIO 25 (ADC2) but conflicts with WiFi

// --- Buttons (external pull-up to 3.3V, active LOW when pressed) ---
constexpr int BUTTON1_PIN = 27;
constexpr int BUTTON2_PIN = 13;

// --- TFT display (ST7789 320x240 on SPI, direct-wired to ESP32) ---
constexpr int TFT_CS    = 15;
constexpr int TFT_DC    = 33;
constexpr int TFT_RST   = 22; // Shared with SCL_PIN — verify wiring if I2C conflicts occur

// SPI bus pins (ESP32 default VSPI)
constexpr int DISP_SCK  = 5;
constexpr int DISP_MOSI = 18;
constexpr int DISP_MISO = 19;  // NC for display

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