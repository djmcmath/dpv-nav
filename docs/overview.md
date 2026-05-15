Big picture:
- Simple underwater navigation, super-limited scope.
- From a software side, the first deliverable just tracks range and bearing from "home."
- A second iteration can track range and bearing to some target (e.g. Harpoon or Bomber)
- Datalogging, of course -> allows later reconstruction of the dive
- From the hardware side: it's just a flow sensor and an IMU with an ESP32.  Display is out to a TFT mounted in a GoPro housing.  There's a 3D printed stand-off that gives the IMU module some space between the DPV tube and the magnetic sensor.
- Display has two modes (switchable at runtime via menu, or at compile time via `DISPLAY_MODE` in config.h):
    - **Navigation Mode** (default): Status bar (system state, GPS, home) + 2×2 grid showing heading, bearing, range, speed
    - **Debug Mode**: Calibrated sensor data (mag/accel/gyro XYZ), fused heading, raw magnetic heading, pitch/roll
- Hierarchical on-screen menu (BTN1 opens/cycles, BTN2 selects, 15s auto-close) for navigation targets, calibration, input toggles, and display settings

Architecture:
- Two-device system connected via Serial1 (JSON packets at 10 Hz):
  - **Nav device**: ESP32 running sensors (LSM6DS33 + LIS3MDL IMU, Adafruit GPS, hall-effect flow sensor), Mahony AHRS filter, dead-reckoning position model
  - **Display device**: ESP32 running ST7789 TFT (320×240), buttons
- Build with PlatformIO: `pio run -e nav` and `pio run -e display`

v1 features (implemented):
- Battery powered ESP32 + IMU + flow sensor + GPS
- Tilt-compensated heading (Mahony AHRS filter, quaternion-based)
- Speed from flowmeter (hall-effect pulse) or GPS (filtered: SOG deadband + COG coherence check rejects position-jitter noise at rest)
- DR position in local frame (flat-earth XY meters from baseline)
- GPS position truth override (optional, configurable — can disable for surface testing)
- "Home" waypoint set via button press; display shows distance + bearing back to home
- Default baseline position (47°N, 122°W) until GPS provides a fix
- Calibration persistence (mag, gyro, accel, heading) to LittleFS with auto-load on boot
- Two display modes: Navigation (status bar + 2×2 grid) and Debug (raw sensor data), switchable at runtime via menu
- Separate DebugPacket protocol for sensor data inspection (opt-in via `ENABLE_DEBUG_PACKET`)
- Configurable units: metric or imperial, switchable at runtime via menu
- Operational mode toggle: surface (GPS + WiFi active) vs dive (GPS + WiFi disabled), switchable at runtime via NAV > Op Mode
- Menu system with JSON-configurable structure: NAV (outbound/home/mark/op mode), CAL (baseline/mounted/hdg cal/speed), INPUT (GPS/WiFi/logging toggles), DISPLAY (mode/units/heading)
- Menu loaded from `/menu.json` on LittleFS; hardcoded fallback if file missing
- Bin-aware magnetometer calibration: two-stage baseline (off-scooter) + mounted (on-scooter) cal, each collecting samples across a heading×elevation grid until coverage is sufficient
- Fourier heading calibration: guided 12-point on-device collection at 30° intervals, offline 4-harmonic Fourier fit (`fourier_fit.py`), JSON upload; reduces residual error to ~2° max
- Interactive speed calibration: swim a known distance (150–500 ft), automatic run detection (flow threshold + heading-change stop), k-factor computed from total pulse count, rolling 6-run average stored to `/speed_cal.json`
