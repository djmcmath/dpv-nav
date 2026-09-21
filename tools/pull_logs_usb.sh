#!/usr/bin/env bash
# Pull the nav device's LittleFS contents (dive logs, currents.csv, cal JSONs)
# over USB, without WiFi and without firmware support.
#
# The firmware exposes no serial command for file transfer -- the only on-device
# paths out are the tern.local file list and log_sync's cloud upload. This reads
# the raw spiffs/littlefs partition off the flash instead and unpacks it on the
# host, so it works on a unit that has never associated with a network.
#
# READ-ONLY: esptool read_flash never writes to the device. Logs are left in
# place, and log_sync will still upload them normally once WiFi is available.
#
# Usage: tools/pull_logs_usb.sh [/dev/cu.usbserial-XXXX]
#
# The offsets below come from partitions_nav.csv. If that table is ever
# re-cut, update them here too -- a stale offset yields a garbage image, not
# an error.

set -euo pipefail

FS_OFFSET=0x330000       # spiffs (LittleFS) partition, partitions_nav.csv
FS_SIZE=0xC0000          # 768 KiB

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ESPTOOL="$HOME/.platformio/penv/bin/python $HOME/.platformio/packages/tool-esptoolpy/esptool.py"
VENV="$REPO_ROOT/tools/.lfsvenv"   # private venv for littlefs-python, created on first run

# Port: argument wins, else the nav upload_port from platformio.ini, else the
# only usbserial device present.
PORT="${1:-}"
if [[ -z "$PORT" ]]; then
  PORT=$(awk '/^\[env:nav\]/{n=1} n && /^upload_port/{print $3; exit}' "$REPO_ROOT/platformio.ini" 2>/dev/null || true)
fi
if [[ -z "$PORT" || ! -e "$PORT" ]]; then
  found=()
  for dev in /dev/cu.usbserial-*; do [[ -e "$dev" ]] && found+=("$dev"); done
  if [[ ${#found[@]} -eq 1 ]]; then
    PORT="${found[0]}"
  elif [[ ${#found[@]} -eq 0 ]]; then
    echo "No /dev/cu.usbserial-* device found. Plug in the NAV board (not the display)." >&2
    exit 1
  else
    echo "Several serial devices present; pass the nav one explicitly:" >&2
    printf '  %s\n' "${found[@]}" >&2
    exit 1
  fi
fi

STAMP=$(date +%Y%m%d-%H%M%S)
OUT="$REPO_ROOT/tools/logs/usb-$STAMP"
IMG="$OUT/littlefs.bin"
mkdir -p "$OUT"

echo "Port:   $PORT"
echo "Output: $OUT"
echo

# If this is really the display board, the read still succeeds (same 4MB flash)
# but the image is not a filesystem and mklittlefs below fails to unpack it.
#
# Baud. Both 921600 and 460800 fail on this board's USB-serial bridge with
# "Invalid head of packet" the moment esptool switches rate -- the bridge
# acknowledges the change and then corrupts the stub's framing. 115200 is the
# rate that works; it reads 768 KiB in ~70 s, which is fine for a manual pull.
# Set BAUD="460800 115200" to try a faster rate first on a different board.
read_ok=0
for baud in ${BAUD:-115200}; do
  echo "Reading filesystem partition at ${baud} baud..."
  rm -f "$IMG"
  if $ESPTOOL --port "$PORT" --baud "$baud" read_flash "$FS_OFFSET" "$FS_SIZE" "$IMG"; then
    read_ok=1
    break
  fi
  echo "  ...failed at ${baud}, dropping to the next rate." >&2
  echo >&2
done
if [[ $read_ok -ne 1 ]]; then
  echo "Flash read failed at every baud rate. Try a different USB cable or port." >&2
  exit 1
fi

echo
# Unpack. See tools/lfs_extract.py for why PlatformIO's mklittlefs cannot do
# this -- it is too old to mount a littlefs 2.1 image and aborts on an assert.
if [[ ! -x "$VENV/bin/python" ]]; then
  echo "Creating $VENV for littlefs-python (first run only)..."
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install -q littlefs-python
fi

echo "Files recovered:"
"$VENV/bin/python" "$REPO_ROOT/tools/lfs_extract.py" "$IMG" "$OUT/fs"
