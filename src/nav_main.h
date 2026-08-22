#pragma once

// Reloads calibration JSON files from LittleFS into the in-memory state used
// by nav processing, without the boot-time blocking-sweep fallbacks
// loadCalibration() has. Exposed so net/cal_sync.cpp can apply a freshly
// staged cloud result (or a restored backup) the same way the existing
// "Reload Cal Files" web action does, without duplicating the
// two-stage-chain / legacy / per-type loading logic here.
void reloadCalibrationFiles();
