#!/usr/bin/env python3
"""
Decode a DPV-Nav binary log (MID or HIGH, /logs/YYYYMMDD-NNN.bin) to CSV.

    python tools/decode_binlog.py 20261011-003.bin            # -> 20261011-003.csv
    python tools/decode_binlog.py 20261011-003.bin -o out.csv

The unit writes MID and HIGH as fixed 77-byte records to save flash and does no
decoding itself; this is the other half. Output columns are the ones the old
HIGH CSV had, so existing analysis scripts read it unchanged.

The layout and scale factors mirror src/util/log_record.h. tools/logrecord_test/
packs records with the real firmware header and checks this script against them
-- run it after touching either side.
"""
import argparse
import os
import struct
import sys
import time

MAGIC = b"TLOG"
SUPPORTED_VERSIONS = (1,)
LEVEL_NAMES = {1: "LOW", 2: "MID", 3: "HIGH"}

HEADER = struct.Struct("<4sBBHH6x")
# timestamp_ms, local_s, heading, speed, pos_x, pos_y, lat, lon, flags, sats,
# hdop, depth, water_temp, 6 x vec3, pitch, roll, mag_temp
RECORD = struct.Struct("<IIHHiiiiBBBhh" + "h" * 18 + "hhh")
assert HEADER.size == 16 and RECORD.size == 77

NAN_I16, NAN_I32, NAN_U16 = -32768, -2147483648, 65535
POS_SRC = "GWEMC"     # index 7 = a source this firmware had no code for

VEC_FIELDS = [("mag_%s_raw", 0.01, 3), ("accel_%s_raw", 0.0005, 3), ("gyro_%s_raw", 0.001, 3),
              ("mag_%s_cal", 0.01, 3), ("accel_%s_cal", 0.0005, 3), ("gyro_%s_cal", 0.001, 3)]

COLUMNS = (["timestamp_ms", "local_time", "heading_deg", "speed_ms", "speed_src",
            "pos_x_m", "pos_y_m", "lat", "lon", "pos_src",
            "gps_satellites", "gps_hdop", "depth_m", "water_temp_c"]
           + [name % ax for name, _, _ in VEC_FIELDS for ax in "xyz"]
           + ["pitch_deg", "roll_deg", "mag_temp_c"])


def num(raw, scale, places, nan):
    return "nan" if raw == nan else f"{raw * scale:.{places}f}"


def decode_record(buf):
    f = RECORD.unpack(buf)
    (ts, local_s, heading, speed, pos_x, pos_y, lat, lon, flags, sats, hdop, depth, wtemp) = f[:13]
    vecs, (pitch, roll, mag_temp) = f[13:31], f[31:34]
    src_idx = (flags >> 1) & 0x7
    pos_src = POS_SRC[src_idx] if src_idx < len(POS_SRC) else "?"
    row = [str(ts),
           time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(local_s)) if local_s else "",
           num(heading, 0.01, 2, NAN_U16), num(speed, 0.001, 3, NAN_U16),
           "G" if flags & 0x01 else "F",
           num(pos_x, 0.01, 2, NAN_I32), num(pos_y, 0.01, 2, NAN_I32),
           num(lat, 1e-7, 8, NAN_I32), num(lon, 1e-7, 8, NAN_I32),
           pos_src]
    # Satellites and HDOP are only meaningful on a GPS row; the CSV left them empty otherwise.
    row += [str(sats), f"{hdop * 0.1:.1f}"] if pos_src == "G" else ["", ""]
    row += [num(depth, 0.01, 2, NAN_I16), num(wtemp, 0.01, 2, NAN_I16)]
    i = 0
    for _, scale, places in VEC_FIELDS:
        for _ in range(3):
            row.append(num(vecs[i], scale, places, NAN_I16))
            i += 1
    row += [num(pitch, 0.01, 2, NAN_I16), num(roll, 0.01, 2, NAN_I16), num(mag_temp, 0.01, 2, NAN_I16)]
    return row


def decode(data):
    """Return (header_info, rows, trailing_bytes). Raises ValueError on a file that isn't one."""
    if len(data) < HEADER.size:
        raise ValueError("file is shorter than a log header")
    magic, version, level, header_size, record_size = HEADER.unpack_from(data)
    if magic != MAGIC:
        raise ValueError("not a DPV-Nav binary log (bad magic)")
    if version not in SUPPORTED_VERSIONS:
        raise ValueError(f"log format version {version} is newer than this script")
    if header_size < HEADER.size or record_size < RECORD.size:
        raise ValueError(f"implausible sizes in header (header {header_size}, record {record_size})")
    body = len(data) - header_size
    n = max(body, 0) // record_size
    # A later firmware may append fields to a record; decode the part we know.
    rows = [decode_record(data[header_size + i * record_size:][:RECORD.size]) for i in range(n)]
    info = {"version": version, "level": level, "record_size": record_size}
    return info, rows, max(body, 0) - n * record_size


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("log", help="binary log file (.bin)")
    ap.add_argument("-o", "--output", help="CSV to write (default: the input name with .csv)")
    args = ap.parse_args()

    with open(args.log, "rb") as fh:
        data = fh.read()
    try:
        info, rows, trailing = decode(data)
    except ValueError as e:
        sys.exit(f"{args.log}: {e}")

    out = args.output or os.path.splitext(args.log)[0] + ".csv"
    if os.path.abspath(out) == os.path.abspath(args.log):
        sys.exit("refusing to overwrite the input file; pass -o")
    with open(out, "w", newline="") as fh:
        fh.write(",".join(COLUMNS) + "\n")
        for row in rows:
            fh.write(",".join(row) + "\n")

    level = LEVEL_NAMES.get(info["level"], str(info["level"]))
    span = (int(rows[-1][0]) - int(rows[0][0])) / 60000.0 if rows else 0.0
    print(f"{args.log}: {level} log, {len(rows)} rows over {span:.1f} min -> {out}", file=sys.stderr)
    if trailing:
        print(f"  note: {trailing} trailing bytes ignored (a row cut short, e.g. by power loss)",
              file=sys.stderr)


if __name__ == "__main__":
    main()
