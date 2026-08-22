#include "nvs_state.h"
#include <Preferences.h>

// NVS namespace names — max 15 chars (ESP32 NVS constraint)
static constexpr char NAV_NS[]  = "nav_state";
static constexpr char DISP_NS[] = "disp_state";

// ---------------------------------------------------------------------------
// Nav device state
// ---------------------------------------------------------------------------
namespace nvs_nav {

State load() {
    State s;
    Preferences prefs;
    // readOnly=true: returns false if the namespace has never been written
    if (!prefs.begin(NAV_NS, /*readOnly=*/true)) {
        // First boot — namespace doesn't exist yet; return factory defaults
        s.gps       = true;
        s.wifi      = true;
        s.dive_mode = false;
        s.log_level = 0;
        s.pos_x     = 0.0f;
        s.pos_y     = 0.0f;
        return s;
    }
    s.gps       = prefs.getBool ("gps",        true);
    s.wifi      = prefs.getBool ("wifi",       true);
    s.dive_mode = prefs.getBool ("dive_mode",  false);
    s.log_level = prefs.getUChar("log_level",  0);
    s.pos_x     = prefs.getFloat("pos_x",      0.0f);
    s.pos_y     = prefs.getFloat("pos_y",      0.0f);
    prefs.end();
    return s;
}

void save(const State& s) {
    Preferences prefs;
    if (!prefs.begin(NAV_NS, /*readOnly=*/false)) return;
    prefs.putBool ("gps",       s.gps);
    prefs.putBool ("wifi",      s.wifi);
    prefs.putBool ("dive_mode", s.dive_mode);
    prefs.putUChar("log_level", s.log_level);
    prefs.putFloat("pos_x",     s.pos_x);
    prefs.putFloat("pos_y",     s.pos_y);
    prefs.end();
}

void savePosition(float x_m, float y_m) {
    Preferences prefs;
    if (!prefs.begin(NAV_NS, /*readOnly=*/false)) return;
    prefs.putFloat("pos_x", x_m);
    prefs.putFloat("pos_y", y_m);
    prefs.end();
}

}  // namespace nvs_nav

// ---------------------------------------------------------------------------
// Display device state
// ---------------------------------------------------------------------------
namespace nvs_disp {

State load() {
    State s;
    Preferences prefs;
    if (!prefs.begin(DISP_NS, /*readOnly=*/true)) {
        s.debug_mode   = false;
        s.show_eta     = false;
        s.imperial     = false;
        s.true_heading = true;
        return s;
    }
    s.debug_mode   = prefs.getBool("debug_mode",   false);
    s.show_eta     = prefs.getBool("show_eta",      false);
    s.imperial     = prefs.getBool("imperial",      false);
    s.true_heading = prefs.getBool("true_heading",  true);
    prefs.end();
    return s;
}

void save(const State& s) {
    Preferences prefs;
    if (!prefs.begin(DISP_NS, /*readOnly=*/false)) return;
    prefs.putBool("debug_mode",   s.debug_mode);
    prefs.putBool("show_eta",     s.show_eta);
    prefs.putBool("imperial",     s.imperial);
    prefs.putBool("true_heading", s.true_heading);
    prefs.end();
}

}  // namespace nvs_disp
