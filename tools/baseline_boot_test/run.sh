#!/bin/bash
# Host-compile and run the baseline live grid's bootstrap tests against the
# real src/util/baseline_boot.cpp and the orientation port it bins through.
#
# fixtures/*.expected.csv come from dive-map's replay model. Regenerate them
# after changing the gate, the ceiling, or the binning, on either side:
#   python3 calibration-processor/tools/baseline_bootstrap_replay.py --quiet \
#       --emit <this dir>/fixtures \
#       --run fixtures/<id>.csv fixtures/<id>.json <id>   (once per fixture)
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O1 -Wall -I../../lib/dpvlink -o boot boot.cpp \
    ../../src/util/baseline_boot.cpp ../../src/util/mag_cal_orient.cpp
./boot
