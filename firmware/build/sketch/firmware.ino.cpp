#include <Arduino.h>
#line 1 "D:\\Documents\\dpv-nav\\firmware\\firmware.ino"
#include "src/main.h"

#line 3 "D:\\Documents\\dpv-nav\\firmware\\firmware.ino"
void setup();
#line 7 "D:\\Documents\\dpv-nav\\firmware\\firmware.ino"
void loop();
#line 3 "D:\\Documents\\dpv-nav\\firmware\\firmware.ino"
void setup() {
  dpvnav::setup();
}

void loop() {
  dpvnav::loop();
}
