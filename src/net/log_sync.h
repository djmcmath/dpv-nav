#pragma once

#include <Arduino.h>

// Automatic dive-log upload.
//
// Every closed log in /logs goes to divemap on its own as soon as the unit is
// on a known network, so the diver's whole job post-dive is "turn logging off,
// turn WiFi on, walk away". The manual "Upload to cloud" buttons on tern.local
// stay as the force/retry path (and mark files here when they succeed), but
// nothing has to be clicked for a normal dive.
//
// Two things this module deliberately does not do:
//
//   - It never uploads logging::currentPath(). A partial upload would land one
//     row now and a second, byte-different one when the dive ends -- the
//     server's content-hash dedupe can't collapse those, and the assembler
//     would see two overlapping runs of one dive.
//   - It never runs during a calibration. cloud_client's calls are blocking,
//     and a cal upload owns the display while it runs; nav_main gates the
//     update() call below on that.
//
// Uploads are blocking (see net/cloud_client.h) and each one can stall the nav
// loop for up to CLOUD_HTTP_TIMEOUT_MS -- past the display's 5 s NAV_TIMEOUT_MS.
// So a pass announces itself first: isUploading() goes true one loop tick
// before the first byte moves, nav_main folds it into NavPacket's
// FLAG2_UPLOADING, and the display holds an "uploading" screen (and suspends
// its link-timeout) until the flag clears. Only one file is uploaded per
// update() call so a NavPacket goes out between files and the count advances.
namespace log_sync {

// Call once from setup(), after LittleFS is mounted and logging::init() has
// run. Loads the uploaded-file record and prunes it (see markUploaded).
void init();

// Call once per main-loop tick, only when the unit is in normal navigation
// (not calibrating). Cheap no-op unless there is something to send.
void update();

// True from just before a pass's first upload until its last one finishes.
bool isUploading();

// Progress within the current pass, for the display. Both 0 when idle.
uint8_t doneCount();
uint8_t totalCount();

// Record `path` as uploaded. Called internally, and by web_server.cpp so a
// file pushed by hand isn't sent again by the next automatic pass.
//
// The record is keyed on filename *and* size. Filenames alone were the whole
// hazard the dated log names in util/logging.cpp were introduced to fix, and
// size closes the residual case (two long dives on one date, or a file
// replaced via the web UI's own upload form). Entries whose file no longer
// exists, or whose size no longer matches, are dropped whenever the record is
// rewritten -- that is what clears the flag on delete, and it covers all four
// ways a log can disappear (the file manager's Delete, Delete Selected,
// logging.cpp's cleanupOldLogs() prune, and a `pio run -e nav -t uploadfs`
// wipe) without any of them having to know this module exists.
void markUploaded(const char* path);

// True if the log at `path` (whose current size the caller already knows --
// tern.local's file list is walking a directory when it asks, and re-opening
// each entry mid-walk to stat it is neither free nor obviously safe) is
// already recorded as uploaded.
bool isUploaded(const char* path, uint32_t size);

// {"pending":N,"uploading":bool,"done":N,"total":N,"error":"..."} for
// tern.local. `error` is the last failure's message, "" if the last pass was
// clean.
String statusJson();

}  // namespace log_sync
