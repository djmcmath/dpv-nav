Big picture:
- Simple underwater navigation, super-limited scope.
- From a software side, the first deliverable just tracks range and bearing from "home."
- A second iteration can track range and bearing to some target (e.g. Harpoon or Bomber)
- Datalogging, of course -> allows later reconstruction of the dive
- From the hardware side: it's just a flow sensor and an IMU with an ESP32.  Display is out to a tiny OLED mounted in a GoPro housing.  There's a 3D printed stand-off that gives the IMU module some space between the DPV tube and the magnetic sensor.
- Display shows:
    - Current heading / speed
    - Range / Bearing to home


So v1 features:
- Battery powered ESP32 + IMU + flow sensor
- Tilt-compensated heading
- DR position in local frame (meters from home, not lat/lon)
- "Home" is just wherever you were when you powered it on
