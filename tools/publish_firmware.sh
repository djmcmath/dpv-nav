#!/usr/bin/env bash
# Publish a DPV-Nav firmware release (nav.bin + display.bin) for OTA.
#
#   tools/publish_firmware.sh 0.7.1 release-notes.md
#   tools/publish_firmware.sh --dry-run 0.7.1 release-notes.md   # build + stage only
#
# Publishes to the host the firmware itself talks to (CLOUD_API_HOST in
# src/config.h), into the dive-map checkout's data/firmware/dpv_nav -- the
# directory the backend serves as {DATA_DIR}/firmware/dpv_nav. Override with
# FIRMWARE_DEST=host:path (ssh/scp; a relative path is under the ssh user's
# home) or a local path. PUBLISH_SSH_USER overrides the default ssh user.
#
# release-notes.md: every line starting with "- " or "* " becomes one bullet in
# the "Changes" list on tern.local. Other lines are ignored.
#
# Order matters for devices checking mid-publish: the images go up first, the
# manifest last and atomically (tmp + mv), so a manifest never names an image
# that isn't there yet.
set -euo pipefail

DRY_RUN=0
if [[ "${1:-}" == "--dry-run" ]]; then
    DRY_RUN=1
    shift
fi

if [[ $# -ne 2 ]]; then
    sed -n '2,/^set -euo/p' "$0" | grep '^#' | sed 's/^# \{0,1\}//'
    exit 2
fi

VERSION="$1"
NOTES="$2"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
NAV_SLOT_BYTES=$((0x190000))      # partitions_nav.csv app0/app1
DISPLAY_SLOT_BYTES=$((0x140000))  # Arduino default.csv app0/app1

die() { echo "error: $*" >&2; exit 1; }

# One source of truth for the server name: the host baked into the firmware.
API_HOST="$(sed -n 's/^constexpr const char\* CLOUD_API_HOST *= *"\(.*\)";.*/\1/p' "$REPO/src/config.h")"
[[ -n "$API_HOST" ]] || die "couldn't read CLOUD_API_HOST from src/config.h"
: "${FIRMWARE_DEST:=${PUBLISH_SSH_USER:-djmcmath}@$API_HOST:divemap/data/firmware/dpv_nav}"

[[ "$VERSION" =~ ^[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}$ ]] || die "version must be MAJOR.MINOR.PATCH, got '$VERSION'"
[[ -f "$NOTES" ]] || die "notes file not found: $NOTES"

SRC_VERSION="$(sed -n 's/^#define FW_VERSION "\(.*\)"/\1/p' "$REPO/src/version.h")"
[[ "$SRC_VERSION" == "$VERSION" ]] || die "src/version.h says '$SRC_VERSION', not '$VERSION' -- bump it and rebuild"

if [[ -n "$(git -C "$REPO" status --porcelain -- src lib 2>/dev/null)" ]]; then
    echo "warning: src/ or lib/ has uncommitted changes -- this release won't match any commit" >&2
fi

echo "==> Building nav + display ($VERSION)"
(cd "$REPO" && pio run -e nav -e display)

NAV_BIN="$REPO/.pio/build/nav/firmware.bin"
DISPLAY_BIN="$REPO/.pio/build/display/firmware.bin"
filesize() { stat -f%z "$1" 2>/dev/null || stat -c%s "$1"; }
NAV_SIZE=$(filesize "$NAV_BIN")
DISPLAY_SIZE=$(filesize "$DISPLAY_BIN")
(( NAV_SIZE < NAV_SLOT_BYTES )) || die "nav.bin ($NAV_SIZE bytes) doesn't fit its OTA slot ($NAV_SLOT_BYTES)"
(( DISPLAY_SIZE < DISPLAY_SLOT_BYTES )) || die "display.bin ($DISPLAY_SIZE bytes) doesn't fit its OTA slot ($DISPLAY_SLOT_BYTES)"

STAGE="$(mktemp -d)"
[[ $DRY_RUN -eq 1 ]] || trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/$VERSION"
cp "$NAV_BIN" "$STAGE/$VERSION/nav.bin"
cp "$DISPLAY_BIN" "$STAGE/$VERSION/display.bin"

# Existing manifest, read from the destination itself (not the public URL) so
# a CDN/cache or a dev/prod mix-up can't make us drop earlier releases.
is_remote() { [[ "$FIRMWARE_DEST" =~ ^[^/]+: ]]; }
EXISTING="$STAGE/existing.json"
if is_remote; then
    HOST="${FIRMWARE_DEST%%:*}"
    DIR="${FIRMWARE_DEST#*:}"
    # Quoted remote paths don't get tilde expansion; ssh/scp already start in $HOME.
    DIR="${DIR#\~/}"
    ssh "$HOST" "cat '$DIR/manifest.json' 2>/dev/null || echo '{\"product\":\"dpv_nav\",\"releases\":[]}'" > "$EXISTING"
else
    cat "$FIRMWARE_DEST/manifest.json" 2>/dev/null > "$EXISTING" \
        || echo '{"product":"dpv_nav","releases":[]}' > "$EXISTING"
fi

python3 - "$EXISTING" "$STAGE/manifest.json" "$VERSION" "$NOTES" \
    "$STAGE/$VERSION/nav.bin" "$STAGE/$VERSION/display.bin" <<'PY'
import datetime, hashlib, json, os, re, sys

existing, out, version, notes, nav, display = sys.argv[1:]
manifest = json.load(open(existing))
releases = manifest.get("releases", [])

if any(r.get("version") == version for r in releases):
    sys.exit(f"error: {version} is already published -- bump the version instead of replacing it")

changes = [m.group(1).strip() for line in open(notes)
           if (m := re.match(r"^\s*[-*]\s+(.*\S)", line))]
if not changes:
    sys.exit("error: notes file has no '- ' bullet lines")

def image(path):
    data = open(path, "rb").read()
    return {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}

release = {
    "version": version,
    "date": datetime.date.today().isoformat(),
    "changes": changes,
    "images": {"nav": image(nav), "display": image(display)},
}

def key(v):
    return tuple(int(p) for p in v.split("."))

# Newest first -- tern.local lists changes in this order.
releases = sorted(releases + [release], key=lambda r: key(r["version"]), reverse=True)
manifest = {"product": "dpv_nav", "releases": releases}
json.dump(manifest, open(out, "w"), indent=2)
print(json.dumps(release, indent=2))
PY

if [[ $DRY_RUN -eq 1 ]]; then
    echo "==> Dry run: staged in $STAGE (nothing uploaded)"
    exit 0
fi

echo "==> Uploading to $FIRMWARE_DEST"
if is_remote; then
    ssh "$HOST" "mkdir -p '$DIR/$VERSION'"
    scp "$STAGE/$VERSION/nav.bin" "$STAGE/$VERSION/display.bin" "$HOST:$DIR/$VERSION/"
    scp "$STAGE/manifest.json" "$HOST:$DIR/manifest.json.tmp"
    ssh "$HOST" "mv '$DIR/manifest.json.tmp' '$DIR/manifest.json'"
else
    mkdir -p "$FIRMWARE_DEST/$VERSION"
    cp "$STAGE/$VERSION/nav.bin" "$STAGE/$VERSION/display.bin" "$FIRMWARE_DEST/$VERSION/"
    cp "$STAGE/manifest.json" "$FIRMWARE_DEST/manifest.json.tmp"
    mv "$FIRMWARE_DEST/manifest.json.tmp" "$FIRMWARE_DEST/manifest.json"
fi

echo "==> Published $VERSION. Verify with:"
echo "    curl -s https://$API_HOST/api/firmware/dpv_nav/manifest.json | head -20"
