#!/bin/bash
# Host-compile the binary log record packer (src/util/log_record.h), write a
# real .bin with it, and check tools/decode_binlog.py reads back what went in.
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O1 -Wall -o logrecord logrecord.cpp
./logrecord sample.bin expected.csv
python3 ../decode_binlog.py sample.bin -o decoded.csv
if diff -u expected.csv decoded.csv; then
    echo "decode_binlog.py output matches the packer."
else
    echo "FAIL: decoder output differs from what was packed"; exit 1
fi
