#line 1 "D:\\Documents\\dpv-nav\\firmware\\src\\nav\\state.h"
enum class SystemState {
    BOOT,
    SELF_TEST,
    CALIBRATION,
    READY,
    NAVIGATING,
    ERROR
};


/*************
Rough flow:
BOOT → power-up, basic hardware init.
SELF_TEST →
    Check sensors, load calibration.
    If OK → READY.
    If missing / invalid calibration → CALIBRATION (or READY with “CAL WARNING”).
CALIBRATION (optional interactive mode later) →
    Full 3D spin, store new offsets.
    Then → READY.
READY →
    Waiting at surface; home not set.
    UI: “Set HOME” prompt; one long-press sets home and transitions to NAVIGATING.
NAVIGATING →
    Normal DR updates.
    UI: heading, bearing to home, range.
ERROR →
    On fatal conditions (no IMU, flow sensor dead, etc.).
*/