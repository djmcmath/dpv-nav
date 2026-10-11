#!/bin/bash
# Host-compile and run the mounted dwell-capture tests against the real
# src/util/mounted_dwell.cpp (and the orientation port it takes pitch/roll from).
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O1 -Wall -I../../lib/dpvlink -o dwell dwell.cpp \
    ../../src/util/mounted_dwell.cpp ../../src/util/mag_cal_orient.cpp
./dwell
