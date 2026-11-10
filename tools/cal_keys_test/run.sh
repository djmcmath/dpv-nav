#!/bin/bash
# Host-compile and run the cal screens' softkey tests against the real
# src/util/cal_keys.h (hold BTN1 to discard, BTN2 to advance, chords inert).
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O1 -Wall -I../../lib/dpvlink -o keys keys.cpp
./keys
