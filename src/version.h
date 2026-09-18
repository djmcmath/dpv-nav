#pragma once

// Firmware release version, shared by BOTH boards. A release always ships
// nav.bin + display.bin built from the same tree, so there is one number, not
// two. Bump this before running tools/publish_firmware.sh -- the script refuses
// to publish if it doesn't match the version you pass it.
//
// Lives here rather than in platformio.ini because platformio.ini is untracked.
// Format is strictly MAJOR.MINOR.PATCH (the server rejects anything else).
#define FW_VERSION "0.7.18"
