#!/bin/bash
# Host-compile and run the status LED tests (src/util/status_led.h).
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O1 -Wall -o statusled statusled.cpp
./statusled
