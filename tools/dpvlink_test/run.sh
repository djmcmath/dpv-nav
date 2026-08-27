#!/bin/bash
# Host-compile and run the dpvlink GAP_FILL target round-trip test.
# Uses the ArduinoJson copy PlatformIO already fetched into .pio/libdeps.
set -e
cd "$(dirname "$0")"
JSON_INC="../../.pio/libdeps/nav/ArduinoJson/src"
[ -d "$JSON_INC" ] || JSON_INC="../../.pio/libdeps/display/ArduinoJson/src"
g++ -std=c++17 -O1 -I"$JSON_INC" -o roundtrip roundtrip.cpp ../../lib/dpvlink/dpvlink.cpp
./roundtrip
