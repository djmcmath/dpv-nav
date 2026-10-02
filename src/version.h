#pragma once

// Firmware release version, shared by BOTH boards. A release always ships
// nav.bin + display.bin built from the same tree, so there is one number, not
// two. Bump this before running tools/publish_firmware.sh -- the script refuses
// to publish if it doesn't match the version you pass it.
//
// Lives here rather than in platformio.ini because platformio.ini is untracked.
// Format is MAJOR.MINOR.PATCH, or MAJOR.MINOR.PATCH-pre.N for an internal/test
// build (SemVer pre-release, e.g. "0.8.0-dev.1"): 15 chars max, suffix is
// dot-separated [0-9A-Za-z] runs. Units only install test builds when opted in
// on tern.local, or when already running one. Number a test build after the
// NEXT release so the public one outranks it.
#define FW_VERSION "0.7.19"
