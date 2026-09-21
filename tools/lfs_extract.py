#!/usr/bin/env python3
"""Unpack a LittleFS partition image dumped off the nav device.

Why not mklittlefs: PlatformIO's bundled tool-mklittlefs is 0.2.3 (2021) and
its vendored littlefs predates on-disk version 2.1, which is what the current
ESP32 Arduino core writes. It fails to mount a real device image with
"Corrupted dir pair at 1 0" and then aborts on an assert. It can still
round-trip images it created itself, which makes the mismatch easy to miss.

Usage: lfs_extract.py <image.bin> <dest_dir>
"""
import os
import sys

from littlefs import LittleFS

BLOCK_SIZE = 4096          # Arduino ESP32 LittleFS geometry
IO_SIZE = 256
DISK_VERSION = 0x00020001  # littlefs 2.1, per the image superblock


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    img, dest = sys.argv[1], sys.argv[2]

    data = open(img, "rb").read()
    fs = LittleFS(block_size=BLOCK_SIZE, block_count=len(data) // BLOCK_SIZE,
                  read_size=IO_SIZE, prog_size=IO_SIZE,
                  mount=False, disk_version=DISK_VERSION)
    fs.context.buffer = bytearray(data)
    fs.mount()

    count = 0
    for root, _dirs, files in fs.walk("/"):
        for name in files:
            src = os.path.join(root, name).replace("//", "/")
            out = os.path.join(dest, src.lstrip("/"))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with fs.open(src, "rb") as fh:
                open(out, "wb").write(fh.read())
            print(f"  {os.path.getsize(out):8d}  {src}")
            count += 1

    print(f"\n{count} files extracted to {dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
