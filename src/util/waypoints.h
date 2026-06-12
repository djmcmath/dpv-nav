#pragma once

#include <Arduino.h>

namespace waypoints {

constexpr int WP_NAME_LEN  = 19;   // max chars, not including null
constexpr int WP_MAX_COUNT = 200;  // hard cap on in-memory list

struct Waypoint {
    char  name[WP_NAME_LEN + 1];
    float lat;
    float lon;
};

// Load from /config/waypoints.json.
// HOME (index 0) is always created at the default baseline lat/lon if absent.
void load();

// Save current list to /config/waypoints.json.
bool save();

// Total number of waypoints (HOME + user-defined).
int count();

// Get waypoint by index. Returns nullptr if out of range.
const Waypoint* get(int idx);

// Get waypoint index by name (case-sensitive). Returns -1 if not found.
int indexOf(const char* name);

// Update HOME (index 0) lat/lon in memory. Does not save to flash.
// Call save() periodically to persist.
void updateHome(float lat, float lon);

// Add new waypoint or update existing one by name.
// "HOME" is reserved — use updateHome() for that.
// Returns false if name is empty, "HOME", or list is full.
bool addOrUpdate(const char* name, float lat, float lon);

// Remove waypoint by name. Cannot remove "HOME".
// Returns false if not found or name == "HOME".
bool remove(const char* name);

// Serialize full list to JSON array string (for web API).
String toJson();

}  // namespace waypoints
