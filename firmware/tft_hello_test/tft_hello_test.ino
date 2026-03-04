// SSD1351 OLED display test for Adafruit 1.27" 128x96 color OLED
// Wired via EyeSPI breakout to Feather ESP32 HUZZAH32
//
// Connections:
//   ESP32 SCK (GPIO 5)  -> EyeSPI SCK
//   ESP32 MO  (GPIO 18) -> EyeSPI MOSI
//   ESP32 MI  (GPIO 19) -> EyeSPI MISO -- NOT CONNECTED
//   GPIO 21             -> EyeSPI TCS  (chip select)
//   GPIO 32             -> EyeSPI DC   (data/command)
//   GPIO 4              -> EyeSPI RST  (reset)
//
// Required libraries (install via Arduino Library Manager):
//   - Adafruit GFX Library
//   - Adafruit SSD1351

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1351.h>
#include <SPI.h>

// Display dimensions
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 96

// Pin definitions
#define OLED_CS   4 //instead of 21
#define OLED_DC   32
#define OLED_RST  14 //instead of 4
#define OLED_SDCS 33 //NC

// SPI bus pins (Feather ESP32 HUZZAH32 silkscreen labels)
#define SPI_SCK   5
#define SPI_MOSI  18
#define SPI_MISO  19 //NC

// Color definitions (16-bit 565 format)
#define BLACK   0x0000
#define WHITE   0xFFFF
#define RED     0xF800
#define GREEN   0x07E0
#define BLUE    0x001F
#define CYAN    0x07FF

Adafruit_SSD1351 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, OLED_CS, OLED_DC, OLED_RST);

void pinToggleTest() {
  // Toggle each control pin and print state so you can probe with a voltmeter
  //Serial.println("\n=== PIN TOGGLE TEST ===");
  //Serial.println("Use a voltmeter to verify each pin toggles.");
  //Serial.println("Each pin goes LOW for 1 sec, then HIGH for 1 sec.\n");

  int pins[] = { OLED_CS, OLED_DC, OLED_RST, SPI_SCK, SPI_MOSI };
  const char* names[] = { "CS (GPIO 14)", "DC (GPIO 32)", "RST (GPIO 15)", "SCK (GPIO 5)", "MOSI (GPIO 18)" };

  /*for (int i = 0; i < 5; i++) {
    pinMode(pins[i], OUTPUT);
    Serial.print("  "); Serial.print(names[i]); Serial.print(" -> LOW ... ");
    digitalWrite(pins[i], LOW);
    delay(1000);
    Serial.print("HIGH ... ");
    digitalWrite(pins[i], HIGH);
    delay(1000);
    Serial.println("done");
  }*/
  //Serial.println("=== PIN TEST COMPLETE ===\n");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\nSSD1351 OLED Test - Diagnostic Version");

  // --- Step 1: Verify wiring with pin toggles ---
  //pinToggleTest();

  // --- Step 2: Deselect SD card on EyeSPI breakout ---
  pinMode(OLED_SDCS, OUTPUT);
  digitalWrite(OLED_SDCS, HIGH);
  Serial.println("SD CS deselected (GPIO 33 HIGH)");

  // --- Step 3: Manual reset pulse ---
  Serial.println("Manual reset pulse...");
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, HIGH);
  delay(100);
  digitalWrite(OLED_RST, LOW);
  delay(100);
  digitalWrite(OLED_RST, HIGH);
  delay(200);
  Serial.println("Reset complete");

  // --- Step 4: Init SPI with explicit pins ---
  // On ESP32, SPI.begin() without args may use VSPI defaults (SCK=18, MOSI=23)
  // which do NOT match the Feather silkscreen. Force correct pins:
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, OLED_CS);
  Serial.print("SPI initialized with SCK="); Serial.print(SPI_SCK);
  Serial.print(" MISO="); Serial.print(SPI_MISO);
  Serial.print(" MOSI="); Serial.print(SPI_MOSI);
  Serial.print(" SS="); Serial.println(OLED_CS);

  // --- Step 5: Init display ---
  Serial.println("Calling oled.begin()...");
  oled.begin(8000000);
  Serial.println("oled.begin() returned");

  // --- Step 6: Color fill test ---
  Serial.println("Filling RED...");
  oled.fillScreen(RED);
  delay(1000);

  Serial.println("Filling GREEN...");
  oled.fillScreen(GREEN);
  delay(1000);

  Serial.println("Filling BLUE...");
  oled.fillScreen(BLUE);
  delay(1000);

  Serial.println("Drawing text...");
  oled.fillScreen(BLACK);
  delay(1000);
  oled.setTextColor(WHITE);
  oled.setTextSize(2);
  oled.setCursor(10, 20);
  oled.println("Hello");
  oled.println("  World!");

  oled.setTextSize(1);
  oled.setTextColor(CYAN);
  oled.setCursor(10, 70);
  oled.println("DPV-Nav Display OK");

  Serial.println("Display test complete - screen should show text");

  delay(1000);
  oled.fillScreen(BLUE);
  //delay(1000);
  oled.setTextColor(WHITE);
  oled.setTextSize(2);

  for (int i = 0; i < 100; i++) {
    oled.setCursor(10, 20);
    oled.printf("%d", i);
    delay(250);
  }

}

void loop() {
  // Nothing to do
}
