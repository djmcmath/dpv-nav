#!/bin/bash
# Host-compile and run the log idle-gate tests (src/util/log_idle.h).
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O1 -Wall -o logidle logidle.cpp
./logidle
