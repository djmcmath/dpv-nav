#!/bin/bash
# Host-compile and run the dive-log filename tests (src/util/log_names.h).
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O1 -Wall -o lognames lognames.cpp
./lognames
