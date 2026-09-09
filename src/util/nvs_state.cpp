#include "nvs_state.h"
#include "../config.h"
#include <Arduino.h>
#include <Preferences.h>
#include <math.h>

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
        s.gps        = true;
        s.wifi       = true;
        s.dive_mode  = false;
        s.log_level  = 0;
        s.pos_x      = 0.0f;
        s.pos_y      = 0.0f;
        s.salt_water = DEFAULT_SALT_WATER;
        return s;
    }
    s.gps        = prefs.getBool ("gps",        true);
    s.wifi       = prefs.getBool ("wifi",       true);
    s.dive_mode  = prefs.getBool ("dive_mode",  false);
    s.log_level  = prefs.getUChar("log_level",  0);
    s.pos_x      = prefs.getFloat("pos_x",      0.0f);
    s.pos_y      = prefs.getFloat("pos_y",      0.0f);
    // NVS is a second place a NaN can outlive a reboot. Dead reckoning does
    // x += speed * sin(heading) * dt, so a NaN heading makes position NaN, the
    // 30 s periodic save writes it to flash, and every subsequent boot restores
    // it via nav::setPosition() -- range and bearing to home stay NaN forever
    // even after the heading itself is fixed. Refuse to restore one.
    bool posPoisoned = !isfinite(s.pos_x) || !isfinite(s.pos_y);
    if (posPoisoned) {
        Serial.printf("[NVS] POISON: stored position (%f, %f) is non-finite -- resetting to origin\n",
                      (double)s.pos_x, (double)s.pos_y);
        s.pos_x = 0.0f;
        s.pos_y = 0.0f;
    }
    s.salt_water = prefs.getBool ("salt",       DEFAULT_SALT_WATER);
    prefs.end();

    // Scrub the NaN out of flash, not just out of the returned struct.
    // savePosition() now refuses to write non-finite values, so without this
    // the stale NaN would sit in NVS forever and warn on every single boot.
    if (posPoisoned) {
        Preferences fix;
        if (fix.begin(NAV_NS, /*readOnly=*/false)) {
            fix.putFloat("pos_x", 0.0f);
            fix.putFloat("pos_y", 0.0f);
            fix.end();
            Serial.println("[NVS] stored position scrubbed to (0,0)");
        }
    }
    return s;
}

void save(const State& s) {
    Preferences prefs;
    if (!prefs.begin(NAV_NS, /*readOnly=*/false)) return;
    prefs.putBool ("gps",       s.gps);
    prefs.putBool ("wifi",      s.wifi);
    prefs.putBool ("dive_mode", s.dive_mode);
    prefs.putUChar("log_level", s.log_level);
    // Same guard as savePosition(): keep the last good position rather than
    // overwriting it with NaN when a full save is triggered by some other toggle.
    if (isfinite(s.pos_x) && isfinite(s.pos_y)) {
        prefs.putFloat("pos_x", s.pos_x);
        prefs.putFloat("pos_y", s.pos_y);
    }
    prefs.putBool ("salt",      s.salt_water);
    prefs.end();
}

void savePosition(float x_m, float y_m) {
    // Don't let a NaN position reach flash in the first place; see load().
    if (!isfinite(x_m) || !isfinite(y_m)) return;
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
        s.heading_mode = HEADING_TRUE;
        return s;
    }
    s.debug_mode   = prefs.getBool("debug_mode",   false);
    s.show_eta     = prefs.getBool("show_eta",      false);
    s.imperial     = prefs.getBool("imperial",      false);

    // "hdg_mode" superseded the older "true_heading" bool when RAW was added.
    // Seed from the old key when the new one is absent so a unit upgrading in
    // place keeps whatever the diver had selected.
    uint8_t fallback = prefs.getBool("true_heading", true) ? HEADING_TRUE : HEADING_MAG;
    s.heading_mode   = prefs.getUChar("hdg_mode", fallback);

    // RAW is a bench mode: it shows an uncorrected heading, which is precisely
    // what the Fourier cal exists to stop anyone navigating on. A diver who
    // forgets the toggle must not find the unit still in RAW after a reboot.
    if (s.heading_mode == HEADING_RAW) s.heading_mode = HEADING_MAG;
    if (s.heading_mode > HEADING_RAW)  s.heading_mode = HEADING_TRUE;

    prefs.end();
    return s;
}

void save(const State& s) {
    Preferences prefs;
    if (!prefs.begin(DISP_NS, /*readOnly=*/false)) return;
    prefs.putBool("debug_mode",   s.debug_mode);
    prefs.putBool("show_eta",     s.show_eta);
    prefs.putBool("imperial",     s.imperial);
    // Persist RAW as MAG rather than refusing to save: the diver's *other*
    // settings in this same call still need to land, and RAW is deliberately
    // not sticky (see load()).
    prefs.putUChar("hdg_mode",
                   s.heading_mode == HEADING_RAW ? HEADING_MAG : s.heading_mode);
    prefs.end();
}

}  // namespace nvs_disp
