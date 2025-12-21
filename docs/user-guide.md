

Power on at the truck / dock.
- Device runs self-test:
    IMU present -- just a check of inputs
    Flow sensor pulses detectable -- may need to blow on it or purge a regulator through it
    Battery OK -- sufficient voltage to operate
    Calibration present -- just a check for the file, at first, maybe later a check of validity / sanity
    UI: “OK” or “CAL NEEDED” with simple icon.

- Quick heading sanity check. // calibrate against iphone or magnetic compass
- If cal necessary - Prompt: “Rotate 360° slowly” (optional).

Device:
    Computes min/max mag readings; compares to stored calibration.
    If major deviation → “CAL WARNING” icon, but allow proceed.
    You can define threshold like “if magnitude deviates >X% from baseline, suggest recalibration.”
    Set ‘HOME’ waypoint.
    At descent line / planned return point.
    UI action: long-press button → “HOME SET”.
    Zero DR position at that spot: (x, y) = (0, 0).
    (Optional) Validate heading to a known feature.
    Face a known landmark (e.g., a cardinal marker).
    Compare indicated heading vs expected. Just a check; no adjustment in v1.

C. Dive navigation workflow
    Descend with DPV / unit active.
    Device running DR integration.
    UI shows:
        Current heading
        Bearing to home
        Range to home
        Normal operation.
    As you move:
        Flow sensor updates speed.
        AHRS updates orientation.
        Nav module integrates X/Y.
        UI updates range/bearing at 2–5 Hz.
    Return to home.
    At any time, follow arrow or numeric bearing back.
    At surfacing, you can log “final range error” vs actual.