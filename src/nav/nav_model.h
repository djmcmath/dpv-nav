#pragma once

namespace nav {

struct Position {
    float x_m;   // meters east of baseline
    float y_m;   // meters north of baseline
    float lat;   // current latitude (degrees)
    float lon;   // current longitude (degrees)
};

// Initialize with a baseline lat/lon (used for local XY ↔ lat/lon conversion).
void init(float baselineLat, float baselineLon);

// Snapshot current position as home.
void setHome();

// Set home to a specific lat/lon (e.g. outbound waypoint from config).
void setTargetLatLon(float lat, float lon);

// Clear home waypoint.
void clearHome();

// Dead-reckoning update: integrate speed along heading.
void updateDR(float heading_deg, float speed_ms, float dt);

// GPS truth update: overwrite current position with GPS lat/lon
// (only effective if GPS position usage is enabled).
void updateGPS(float lat, float lon);

// Enable/disable GPS position as truth source.
void setUseGps(bool enable);

// Restore a saved position (e.g. from NVS on boot).
void setPosition(float x_m, float y_m);

// Snap current position to a lat/lon without being gated by the GPS-enable flag.
// Used for "arrived at waypoint" to correct accumulated DR error. A set datum
// is shifted by the same offset: a snap corrects the frame, and the datum was
// recorded in that same (wrong) frame.
void snapToLatLon(float lat, float lon);

// --- Search datum ------------------------------------------------------------
// A remembered point -- typically the bottom of the line -- kept apart from the
// navigation target, so selecting a waypoint never loses the way back.

// Record the current position as the datum.
void setDatum();

// Restore a saved datum (e.g. from NVS on boot).
void setDatumXY(float x_m, float y_m);

void clearDatum();
bool hasDatum();

// Datum in local XY and lat/lon. Meaningless unless hasDatum().
Position getDatum();

// "The datum was really at this lat/lon." Translates the current position by
// the same offset that moves the datum there, so it is valid from anywhere,
// not just while standing on the datum. Repeatable: the last call wins.
// Returns false (and changes nothing) if no datum is set.
bool retroArrive(float lat, float lon);

// Make the datum the navigation target. Returns false if no datum is set.
bool targetDatum();

// Current position in local XY and lat/lon.
Position getPosition();

// True if home has been set.
bool hasHome();

// True if GPS position is being used as truth source.
bool isUsingGps();

// Distance from current position to home (meters). Returns 0 if no home.
float distanceToHome_m();

// Bearing from current position to home (0-360 degrees). Returns 0 if no home.
float bearingToHome_deg();

}  // namespace nav
