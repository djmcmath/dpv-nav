#!/bin/bash
# Host-compile and run the 12-point heading cal level-gate tests against the
# real src/util/level_gate.cpp and the stillness window it shares with mounted
# dwell capture.
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O1 -Wall -I../../lib/dpvlink -o gate gate.cpp \
    ../../src/util/level_gate.cpp ../../src/util/stillness.cpp ../../src/util/mag_cal_orient.cpp
./gate
