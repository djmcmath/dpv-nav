#pragma once

#include <stdint.h>

// NVS (Non-Volatile Storage) state persistence using ESP32 Preferences.
// Nav device uses namespace "nav_state"; display device uses "disp_state".
// Load on boot to restore previous session. Save only on change (or
// periodically for estimated position).

namespace nvs_nav {

struct State {
    bool    gps;        // GPS enabled (position + speed)
    bool    wifi;       // WiFi enabled
    bool    dive_mode;  // Dive mode active (true=dive, false=surface)
    uint8_t log_level;  // Logging level (0=OFF, 1=LOW, 2=HIGH)
    float   pos_x;      // Last estimated X position (meters east of baseline)
    float   pos_y;      // Last estimated Y position (meters north of baseline)
    bool    salt_water; // Water density for depth calc (true=salt, false=fresh)
};

// Load from NVS. Returns factory defaults if namespace is uninitialized.
State load();

// Save all fields to NVS.
void save(const State& s);

// Save only position fields (called on a configurable interval, not every loop tick).
void savePosition(float x_m, float y_m);

}  // namespace nvs_nav

namespace nvs_disp {

struct State {
    bool debug_mode;    // true=debug view, false=nav view
    bool show_eta;      // true=show ETA, false=show speed
    bool imperial;      // true=ft/mi, false=m/km
    bool true_heading;  // true=true heading, false=magnetic
};

// Load from NVS. Returns factory defaults if namespace is uninitialized.
State load();

// Save all fields to NVS.
void save(const State& s);

}  // namespace nvs_disp
