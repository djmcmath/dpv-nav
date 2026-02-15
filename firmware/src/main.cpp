#include <Wire.h>
#include <math.h>
#include <Arduino.h>
#include <SPIFFS.h>
#include "main.h"
#include "board_pins.h"
#include "./sensors/imu.h"
#include "./nav/ui_controller.h"
#include "./types/types.h"
#include "./math/orientation.h"
#include "./math/mahony.h"
#include "./sensors/calib.h"
#include "./util/logging.h"
#include "./util/storage.h"
#include "./util/mag_cal_collect.h"

MahonyState ahrs;
MahonyParams params{ .kp = 1.0f, .ki = 0.005f, .useMag = true };
//kp: responsiveness of corrections.  Higher => faster convergence, but more sensitivity to potential erroneous readings.  Lower => slower convergence, but more stable.
//ki: estimates residual gyro bias.  Nominally, should eliminate steady-state gyro drift over time.

// Default uncalibrated values - will be loaded from SPIFFS or re-calibrated on first boot
Calib3 gyroCal{{0,0,0},{1,1,1}};
Calib3 accelCal{{0,0,0},{1,1,1}};
MagCalib magCal{{0,0,0}, { {1,0,0},{0,1,0},{0,0,1} }};  // Identity - no calibration yet
imu::ImuConfig imuConfig{ .accel_g_fullscale = 16.0f,
                         .gyro_dps_fullscale = 2000.0f,
                         .mag_uT_fullscale = 4.0f,
                         .sample_hz = 100 };

// Separate axis maps: accel/gyro are on same chip (LSM6DS3), mag is separate (LIS3MDL)
imu::AxisMap accelGyroAxisMap{ .x_axis = +1, .y_axis = +2, .z_axis = +3 };  // No inversions needed!
imu::AxisMap magAxisMap{ .x_axis = +1, .y_axis = -2, .z_axis = +3 };  // Invert Y only (Z correct for NH field)

//Tech note: Periodically Ctrl-Shift-P -> Run Task -> Arduino Generate Compile Commands (or just Ctrl-Alt-B).

namespace dpvnav {

void setup() {
  Serial.begin(115200);
  Serial.print("Waiting for Serial");
  while (!Serial) {
    delay(10);
    Serial.print(".");
  }

  Serial.println("Initializing I2C and LIS3MDL magnetometer...");

  logging::init(); // Initialize logging system (SPIFFS, log file, etc.)

  //calls mag, gyro, and accel init functions
  if (!imu::init(imuConfig, accelGyroAxisMap, magAxisMap)) {
    Serial.println("Error initializing IMU");
    while (1);
  }
  delay(50);

  mahonyInit(ahrs);
  
  // Magnetometer calibration: rotate device through all orientations for 30 seconds
  Serial.println("\n=== MAGNETOMETER CALIBRATION ===");
  //TODO - set this to check for a user input as well, e.g. if the button is being pushed at boot, skip loading from flash and run the calibration anyways.
  if (!storage::loadMagCalibration("mag_cal.json", magCal)) {
    imu::calibrateMagnetometer(magCal, 90000);  // 30 sec calibration
    storage::saveMagCalibration("mag_cal.json", magCal);  // Save for next boot
  } 
  Serial.println("mag cal is:");
  Serial.print("bias x/y/z: "); Serial.print(magCal.bias.x, 6);
  Serial.print("/"); Serial.print(magCal.bias.y, 6);
  Serial.print("/"); Serial.print(magCal.bias.z, 6);
  Serial.print("soft iron matrix:\n");
  for (int i = 0; i < 3; i++) {
    Serial.print("  ");
    for (int j = 0; j < 3; j++) {
      Serial.print(magCal.softIron[i][j], 6);
      if (j < 2) Serial.print(", ");
    }
    Serial.println();
  }
  imu::setMagCalibration(magCal);             // Apply calibration to all future readMag() calls
  Serial.println("=== CALIBRATION COMPLETE ===\n");

  // Diagnostic: Read current magnetometer and show heading
  delay(100);
  imu::Vec3f mag_test;
  if (imu::readMag_uT(mag_test) == imu::ImuStatus::Ok) {
    float test_heading = atan2f(mag_test.y, mag_test.x) * 180.0f / M_PI;
    if (test_heading < 0) test_heading += 360.0f;
    Serial.println("=== INITIAL HEADING CHECK ===");
    Serial.print("Calibrated mag: X="); Serial.print(mag_test.x, 2);
    Serial.print(" Y="); Serial.print(mag_test.y, 2);
    Serial.print(" Z="); Serial.print(mag_test.z, 2); Serial.println(" µT");
    Serial.print("Current heading: "); Serial.print(test_heading, 1); Serial.println("°");
    Serial.println("Point device North and verify heading shows ~0-10° or ~350-360°");
    Serial.println("If offset, may need to check sensor orientation or declination");
    Serial.println("==============================\n");
  }

  // Gyroscope calibration: sample at rest for 10 seconds
  if (!storage::loadCalib3("gyro_cal.json", gyroCal)) {
    imu::calibrateGyroscope(gyroCal, 10000);  // 10 second calibration
    storage::saveCalib3("gyro_cal.json", gyroCal);
  }
  imu::setGyroCalibration(gyroCal);

  // Accelerometer calibration: 6-point orientation sequence
  if (!storage::loadCalib3("accel_cal.json", accelCal)) {
    imu::calibrateAccelerometer(accelCal, 2500);  // 2.5 sec per orientation
    storage::saveCalib3("accel_cal.json", accelCal);
  }
  imu::setAccelCalibration(accelCal);

  Serial.println("\n=== ALL CALIBRATIONS COMPLETE ===\n");

  // Print available serial commands
  Serial.println("=== DPV-Nav Ready ===");
  Serial.println("Serial commands:");
  Serial.println("  start_cal      - Start magnetometer calibration data collection (30 sec)");
  Serial.println("  dump_cal       - Dump collected calibration data to serial (copy to PC)");
  Serial.println("  clear_cal      - Clear mag calibration data from SPIFFS");
  Serial.println("  reset_all_cal        - Delete ALL calibrations - requires reboot");
  Serial.println("  sensor_orientation   - Show RAW sensor frames (no axis mapping)");
  Serial.println("  debug_axes           - End-to-end axis analysis (trace data)");
  Serial.println("  axis_test            - Test sensor axes to find correct orientation");
  Serial.println("  help                 - Show this message");
  Serial.println("=====================\n");

  // Print coordinate frame info
  Serial.println("=== COORDINATE FRAME INFO ===");
  Serial.println("Configured frame: NED (North-East-Down)");
  Serial.println("  +X = North (forward)");
  Serial.println("  +Y = East (right)");
  Serial.println("  +Z = Down");
  Serial.println("Accel/Gyro axis mapping: X=+sensor_X, Y=+sensor_Y, Z=+sensor_Z");
  Serial.println("  (LSM6DS3 chip - no inversions needed)");
  Serial.println("Mag axis mapping: X=+sensor_X, Y=-sensor_Y, Z=+sensor_Z");
  Serial.println("  (LIS3MDL chip - Y inverted, Z matches NH magnetic field)");
  Serial.println("Magnetometer: Heading = atan2(Y, X)");
  Serial.println("  0° = North, 90° = East, 180° = South, 270° = West");
  Serial.println("===============================\n");
}

void loop() {

  // Handle serial commands for magnetometer calibration
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toLowerCase();

    if (cmd == "start_cal") {
      Serial.println("[CMD] Starting magnetometer calibration data collection...");
      if (!mag_cal::startCollection(120000)) {  // 30 seconds
        Serial.println("[CMD] Failed to start collection!");
      }
    }
    else if (cmd == "dump_cal") {
      Serial.println("[CMD] Dumping calibration data...");
      mag_cal::dumpToSerial();
    }
    else if (cmd == "clear_cal") {
      Serial.println("[CMD] Clearing magnetometer calibration data...");
      mag_cal::clearData();
    }
    else if (cmd == "reset_all_cal") {
      Serial.println("[CMD] Clearing ALL calibration files...");
      if (SPIFFS.begin(true)) {
        // Correct file paths to match what storage.cpp uses
        SPIFFS.remove("/mag_cal.json");
        SPIFFS.remove("/gyro_cal.json");
        SPIFFS.remove("/accel_cal.json");
        SPIFFS.remove("/mag_cal_samples.csv");
        Serial.println("[CMD] All calibration files deleted. Reboot to re-calibrate.");
      } else {
        Serial.println("[CMD] Error: SPIFFS mount failed");
      }
    }
    else if (cmd == "reset_accel_cal") {
      Serial.println("[CMD] Clearing accelerometer calibration data...");
      if (SPIFFS.begin(true)) {
        SPIFFS.remove("/accel_cal.json");
        Serial.println("[CMD] Accelerometer calibration file deleted. Reboot to re-calibrate.");
      } else {
        Serial.println("[CMD] Error: SPIFFS mount failed");
      }
    }
    else if (cmd == "sensor_orientation") {
      Serial.println("\n========================================================================");
      Serial.println("SENSOR PHYSICAL ORIENTATION TEST");
      Serial.println("========================================================================");
      Serial.println("This reads sensors in their NATIVE frame (no axis mapping applied).");
      Serial.println("Place device LEVEL on desk, then press Enter...");
      while (!Serial.available()) { delay(10); }
      Serial.readStringUntil('\n');

      // Read sensors in their native sensor frame (no axis mapping)
      imu::Vec3i16 accel_native, gyro_native, mag_native;
      imu::readAccelRaw_SensorFrame(accel_native);
      imu::readGyroRaw_SensorFrame(gyro_native);
      imu::readMagRaw_SensorFrame(mag_native);

      int16_t ax = accel_native.x;
      int16_t ay = accel_native.y;
      int16_t az = accel_native.z;

      int16_t gx = gyro_native.x;
      int16_t gy = gyro_native.y;
      int16_t gz = gyro_native.z;

      int16_t mx = mag_native.x;
      int16_t my = mag_native.y;
      int16_t mz = mag_native.z;

      Serial.println("\n--- NATIVE SENSOR FRAME (no axis mapping) ---");
      Serial.print("  Accel (counts): ["); Serial.print(ax); Serial.print(", ");
      Serial.print(ay); Serial.print(", "); Serial.print(az); Serial.println("]");
      Serial.print("  Gyro (counts):  ["); Serial.print(gx); Serial.print(", ");
      Serial.print(gy); Serial.print(", "); Serial.print(gz); Serial.println("]");
      Serial.print("  Mag (counts):   ["); Serial.print(mx); Serial.print(", ");
      Serial.print(my); Serial.print(", "); Serial.print(mz); Serial.println("]");

      Serial.println("\n--- EXPECTED for LEVEL device in NED frame ---");
      Serial.println("  Accel: Z should be LARGE and NEGATIVE (~-8000)");
      Serial.println("         X and Y should be near 0");
      Serial.println("  Gyro:  All should be near 0 (stationary)");
      Serial.println("  Mag:   Depends on orientation:");
      Serial.println("         - If pointing North: X large positive");
      Serial.println("         - If pointing East:  Y large positive");

      Serial.println("\n--- ANALYSIS ---");
      Serial.println("For each sensor, determine which axis is 'down' (large negative ~-8000):");

      // Accel analysis
      Serial.print("  Accel 'down' axis: ");
      if (ax < -6000) Serial.println("X (sensor X = NED Z)");
      else if (ax > 6000) Serial.println("-X (sensor -X = NED Z)");
      else if (ay < -6000) Serial.println("Y (sensor Y = NED Z)");
      else if (ay > 6000) Serial.println("-Y (sensor -Y = NED Z)");
      else if (az < -6000) Serial.println("Z (sensor Z = NED Z) ✓ Typical");
      else if (az > 6000) Serial.println("-Z (sensor -Z = NED Z)");
      else Serial.println("UNKNOWN - values too small");

      Serial.println("\n--- RECOMMENDED AXIS MAPPING ---");
      Serial.println("Based on readings above, set AxisMap as follows:");
      Serial.println("  If accel Z is negative: z_axis = +3 (no inversion)");
      Serial.println("  If accel Z is positive: z_axis = -3 (invert)");
      Serial.println("  If accel Y is negative: y_axis = +2 or use accel Y for NED Z");
      Serial.println("  Same logic applies to gyro (same chip as accel)");
      Serial.println("  Mag may need DIFFERENT mapping (separate chip)");
      Serial.println("========================================================================\n");
    }
    else if (cmd == "debug_axes") {
      Serial.println("\n========================================================================");
      Serial.println("END-TO-END AXIS CONVENTION ANALYSIS");
      Serial.println("========================================================================");
      Serial.println("This traces sensor data through every transformation stage.\n");
      Serial.println("Press Enter to capture one snapshot...");
      while (!Serial.available()) { delay(10); }
      Serial.readStringUntil('\n');

      // Stage 1: Read RAW sensor values (AFTER axis mapping in current code)
      imu::Vec3i16 rawMagSensor, rawAccelSensor, rawGyroSensor;
      imu::readMagRaw(rawMagSensor);
      imu::readAccelRaw(rawAccelSensor);
      imu::readGyroRaw(rawGyroSensor);

      Serial.println("\n--- STAGE 1: After Axis Mapping (int16 counts) ---");
      Serial.print("  Mag:   ["); Serial.print(rawMagSensor.x); Serial.print(", ");
      Serial.print(rawMagSensor.y); Serial.print(", "); Serial.print(rawMagSensor.z); Serial.println("]");
      Serial.print("  Accel: ["); Serial.print(rawAccelSensor.x); Serial.print(", ");
      Serial.print(rawAccelSensor.y); Serial.print(", "); Serial.print(rawAccelSensor.z); Serial.println("]");
      Serial.print("  Gyro:  ["); Serial.print(rawGyroSensor.x); Serial.print(", ");
      Serial.print(rawGyroSensor.y); Serial.print(", "); Serial.print(rawGyroSensor.z); Serial.println("]");

      // Stage 2: After unit conversion (but before calibration)
      imu::Vec3f magRawFloat, accelRawFloat, gyroRawFloat;
      imu::Vec3f magCal, accelCal, gyroCal;
      imu::readMag_raw_cal(magRawFloat, magCal);
      imu::readAccel_g_raw_cal(accelRawFloat, accelCal);
      imu::readGyro_rad_s_raw_cal(gyroRawFloat, gyroCal);

      Serial.println("\n--- STAGE 2: After Unit Conversion (before calibration) ---");
      Serial.print("  Mag (µT):  ["); Serial.print(magRawFloat.x, 2); Serial.print(", ");
      Serial.print(magRawFloat.y, 2); Serial.print(", "); Serial.print(magRawFloat.z, 2); Serial.println("]");
      Serial.print("  Accel (g): ["); Serial.print(accelRawFloat.x, 4); Serial.print(", ");
      Serial.print(accelRawFloat.y, 4); Serial.print(", "); Serial.print(accelRawFloat.z, 4); Serial.println("]");
      Serial.print("  Gyro (°/s): ["); Serial.print(gyroRawFloat.x * 57.3f, 2); Serial.print(", ");
      Serial.print(gyroRawFloat.y * 57.3f, 2); Serial.print(", "); Serial.print(gyroRawFloat.z * 57.3f, 2); Serial.println("]");

      Serial.println("\n--- STAGE 3: After Calibration (what Mahony receives) ---");
      Serial.print("  Mag (µT):     ["); Serial.print(magCal.x, 2); Serial.print(", ");
      Serial.print(magCal.y, 2); Serial.print(", "); Serial.print(magCal.z, 2); Serial.println("]");
      Serial.print("  Accel (g):    ["); Serial.print(accelCal.x, 4); Serial.print(", ");
      Serial.print(accelCal.y, 4); Serial.print(", "); Serial.print(accelCal.z, 4); Serial.print("]  (");
      Serial.print(accelCal.x * 9.81f, 2); Serial.print(", ");
      Serial.print(accelCal.y * 9.81f, 2); Serial.print(", ");
      Serial.print(accelCal.z * 9.81f, 2); Serial.println(" m/s²)");
      Serial.print("  Gyro (°/s):   ["); Serial.print(gyroCal.x * 57.3f, 2); Serial.print(", ");
      Serial.print(gyroCal.y * 57.3f, 2); Serial.print(", "); Serial.print(gyroCal.z * 57.3f, 2); Serial.println("]");

      // Stage 4: Current quaternion and Euler angles
      Serial.println("\n--- STAGE 4: AHRS State ---");
      Serial.print("  Quaternion: ["); Serial.print(ahrs.q.w, 4); Serial.print(", ");
      Serial.print(ahrs.q.x, 4); Serial.print(", "); Serial.print(ahrs.q.y, 4); Serial.print(", ");
      Serial.print(ahrs.q.z, 4); Serial.println("]");

      Euler attitude = quatToEulerRad(ahrs.q);
      float roll_deg = attitude.roll * 180.0f / M_PI;
      float pitch_deg = attitude.pitch * 180.0f / M_PI;
      float heading_deg = headingDegFromYawRad(attitude.yaw, 0.0f);

      Serial.print("  Euler (AHRS): Roll="); Serial.print(roll_deg, 1);
      Serial.print("°, Pitch="); Serial.print(pitch_deg, 1);
      Serial.print("°, Heading="); Serial.print(heading_deg, 1); Serial.println("°");

      // Stage 5: Independent heading calculations for comparison
      Serial.println("\n--- STAGE 5: Independent Heading Calculations ---");

      // Simple mag heading (no tilt comp)
      float mag_heading_simple = atan2f(magCal.y, magCal.x) * 180.0f / M_PI;
      if (mag_heading_simple < 0) mag_heading_simple += 360.0f;
      Serial.print("  Mag (no tilt comp): "); Serial.print(mag_heading_simple, 1); Serial.println("°");

      // Tilt-compensated mag heading
      float ax = accelCal.x, ay = accelCal.y, az = accelCal.z;  // Already in g
      float a_norm = sqrtf(ax*ax + ay*ay + az*az);
      if (a_norm > 0.1f) { ax /= a_norm; ay /= a_norm; az /= a_norm; }
      float mag_dot_a = magCal.x*ax + magCal.y*ay + magCal.z*az;
      float mag_x_h = magCal.x - mag_dot_a * ax;
      float mag_y_h = magCal.y - mag_dot_a * ay;
      float mag_heading_tc = atan2f(mag_y_h, mag_x_h) * 180.0f / M_PI;
      if (mag_heading_tc < 0) mag_heading_tc += 360.0f;
      Serial.print("  Mag (tilt comp):    "); Serial.print(mag_heading_tc, 1); Serial.println("°");

      // Expected values for level device pointing in current direction
      Serial.println("\n--- EXPECTED VALUES (for level device in NED frame) ---");
      Serial.println("  Accel: X≈0, Y≈0, Z≈-1.0 g (upward reaction force)");
      Serial.println("  Mag:   Points toward magnetic north (X+ for North, Y+ for East)");
      Serial.println("  Gyro:  X≈0, Y≈0, Z≈0 °/s (stationary)");
      Serial.println("  AHRS Heading should match Mag (tilt comp) heading");

      float heading_error = heading_deg - mag_heading_tc;
      if (heading_error > 180) heading_error -= 360;
      if (heading_error < -180) heading_error += 360;

      Serial.println("\n--- DIAGNOSIS ---");
      Serial.print("  Heading error (AHRS - MagTC): "); Serial.print(heading_error, 1); Serial.println("°");
      if (fabsf(heading_error) < 10) {
        Serial.println("  ✓ GOOD - Headings match within 10°");
      } else if (fabsf(heading_error) < 45) {
        Serial.println("  ⚠ WARNING - Headings diverging (10-45°)");
      } else if (fabsf(heading_error - 180) < 20 || fabsf(heading_error + 180) < 20) {
        Serial.println("  ✗ ERROR - 180° flip detected! Check axis signs");
      } else if (fabsf(heading_error - 90) < 20 || fabsf(heading_error + 90) < 20 ||
                 fabsf(heading_error - 270) < 20 || fabsf(heading_error + 270) < 20) {
        Serial.println("  ✗ ERROR - 90° rotation detected! Check axis mapping");
      } else {
        Serial.println("  ✗ ERROR - Large heading mismatch! Frame mismatch suspected");
      }

      // Check if accel frame matches expected NED
      if (fabsf(az + 1.0f) < 0.1f && fabsf(ax) < 0.1f && fabsf(ay) < 0.1f) {
        Serial.println("  ✓ Accel Z sign correct for NED frame (Z ≈ -1.0 g)");
      } else if (fabsf(az - 1.0f) < 0.1f) {
        Serial.println("  ✗ Accel Z has WRONG sign (Z ≈ +1.0 g, should be negative)");
      }

      Serial.println("========================================================================\n");
    }
    else if (cmd == "axis_test") {
      Serial.println("\n=== AXIS ORIENTATION TEST ===");
      Serial.println("This test helps identify which sensor axes point in which physical directions.");
      Serial.println("Hold device LEVEL and point it in different directions.\n");

      for (int test = 0; test < 4; test++) {
        const char* direction = "";
        switch(test) {
          case 0: direction = "NORTH (forward)"; break;
          case 1: direction = "EAST (right)"; break;
          case 2: direction = "SOUTH (backward)"; break;
          case 3: direction = "WEST (left)"; break;
        }

        Serial.print("\nPoint device "); Serial.print(direction);
        Serial.println(" and press Enter...");
        while (!Serial.available()) { delay(10); }
        Serial.readStringUntil('\n');

        // Read mag and accel
        imu::Vec3f mag, accel;
        delay(200);  // Settle

        // Average 10 samples
        float mx = 0, my = 0, mz = 0;
        float ax = 0, ay = 0, az = 0;
        for (int i = 0; i < 10; i++) {
          if (imu::readMag_uT(mag) == imu::ImuStatus::Ok) {
            mx += mag.x; my += mag.y; mz += mag.z;
          }
          if (imu::readAccel_g(accel) == imu::ImuStatus::Ok) {
            ax += accel.x; ay += accel.y; az += accel.z;
          }
          delay(10);
        }
        mx /= 10; my /= 10; mz /= 10;
        ax /= 10; ay /= 10; az /= 10;

        Serial.print("  Mag: [");
        Serial.print(mx, 2); Serial.print(", ");
        Serial.print(my, 2); Serial.print(", ");
        Serial.print(mz, 2); Serial.println("] µT");

        Serial.print("  Accel: [");
        Serial.print(ax, 3); Serial.print(", ");
        Serial.print(ay, 3); Serial.print(", ");
        Serial.print(az, 3); Serial.println("] g");
      }

      Serial.println("\n=== ANALYSIS ===");
      Serial.println("Compare the magnetometer readings:");
      Serial.println("- The axis with LARGEST change between North/South is the FORWARD axis");
      Serial.println("- The axis with LARGEST change between East/West is the RIGHT axis");
      Serial.println("- If forward axis shows negative values for North, it's pointing backwards");
      Serial.println("\nExpected for NED frame:");
      Serial.println("  North: Mag X > 0 (large positive)");
      Serial.println("  East:  Mag Y > 0 (large positive)");
      Serial.println("  South: Mag X < 0 (large negative)");
      Serial.println("  West:  Mag Y < 0 (large negative)");
      Serial.println("  All:   Accel Z ≈ 1g (down), X/Y ≈ 0 (level)");
      Serial.println("=====================================\n");
    }
    else if (cmd == "help") {
      Serial.println("\nAvailable commands:");
      Serial.println("  start_cal            - Collect mag calibration data (30 sec, rotate device)");
      Serial.println("  dump_cal             - Export data via serial (copy-paste to save)");
      Serial.println("  clear_cal            - Delete mag calibration data from SPIFFS");
      Serial.println("  reset_all_cal        - Delete ALL calibrations (mag/gyro/accel) - requires reboot");
      Serial.println("  sensor_orientation   - Show RAW sensor frames (no axis mapping)");
      Serial.println("  debug_axes           - End-to-end axis analysis (trace data)");
      Serial.println("  axis_test            - Test sensor axes to find correct orientation");
      Serial.println("  help                 - Show this message\n");
    }
    else if (cmd.length() > 0) {
      Serial.print("[CMD] Unknown command: '");
      Serial.print(cmd);
      Serial.println("' (type 'help' for commands)");
    }
  }

  // If actively collecting calibration data, log samples at high rate and skip normal processing
  if (mag_cal::isCollecting()) {
    mag_cal::logSample();
    delay(10);  // ~100 Hz sampling rate
    return;     // Skip normal loop processing during calibration
  }

  static uint32_t lastMicros = micros();
  uint32_t now = micros();
  float dt = (now - lastMicros) / 1e6f;
  lastMicros = now;

  imu::Vec3f magRaw, magCal, accelRaw, accelCal, gyroRaw, gyroCal;
  
  if (imu::readMag_raw_cal(magRaw, magCal) != imu::ImuStatus::Ok) {
    Serial.println("Error reading magnetometer");
    delay(1000);
    return;
  }
  if (imu::readAccel_g_raw_cal(accelRaw, accelCal) != imu::ImuStatus::Ok) {
    Serial.println("Error reading accelerometer");
    delay(1000);
    return;
  }
  if (imu::readGyro_rad_s_raw_cal(gyroRaw, gyroCal) != imu::ImuStatus::Ok) {
    Serial.println("Error reading gyroscope");
    delay(1000);
    return;
  }
  
  // Magnetometer stability test (Test 1)
  static float magX_samples[100];
  static float magY_samples[100];
  static float magZ_samples[100];
  static int mag_sample_idx = 0;

  if (mag_sample_idx < 100) {
    magX_samples[mag_sample_idx] = magCal.x;
    magY_samples[mag_sample_idx] = magCal.y;
    magZ_samples[mag_sample_idx] = magCal.z;
    mag_sample_idx++;

    if (mag_sample_idx == 100) {
      // Calculate standard deviation
      float meanX = 0, meanY = 0, meanZ = 0;
      for (int i = 0; i < 100; i++) {
        meanX += magX_samples[i];
        meanY += magY_samples[i];
        meanZ += magZ_samples[i];
      }
      meanX /= 100; meanY /= 100; meanZ /= 100;

      float stdX = 0, stdY = 0, stdZ = 0;
      for (int i = 0; i < 100; i++) {
        stdX += (magX_samples[i] - meanX) * (magX_samples[i] - meanX);
        stdY += (magY_samples[i] - meanY) * (magY_samples[i] - meanY);
        stdZ += (magZ_samples[i] - meanZ) * (magZ_samples[i] - meanZ);
      }
      stdX = sqrtf(stdX / 100);
      stdY = sqrtf(stdY / 100);
      stdZ = sqrtf(stdZ / 100);

      float magMag = sqrtf(meanX*meanX + meanY*meanY + meanZ*meanZ);

      Serial.println("\n=========================================================");
      Serial.println("MAG STABILITY TEST (100 samples at rest)");
      Serial.println("=========================================================");
      Serial.print("Mean: X="); Serial.print(meanX, 2);
      Serial.print(" Y="); Serial.print(meanY, 2);
      Serial.print(" Z="); Serial.print(meanZ, 2);
      Serial.print(" ||Mag||="); Serial.println(magMag, 2);
      Serial.print("Std Dev: X="); Serial.print(stdX, 3); Serial.println(" µT");
      Serial.print("         Y="); Serial.print(stdY, 3); Serial.println(" µT");
      Serial.print("         Z="); Serial.print(stdZ, 3); Serial.println(" µT");
      Serial.println("\nTarget: < 0.3 µT on each axis for stable heading");
      if (stdX > 0.5 || stdY > 0.5 || stdZ > 0.5) {
        Serial.println("RESULT: HIGH NOISE - Magnetometer too noisy for stable heading");
      } else if (stdX > 0.3 || stdY > 0.3 || stdZ > 0.3) {
        Serial.println("RESULT: MODERATE NOISE - May see some drift");
      } else {
        Serial.println("RESULT: LOW NOISE - Magnetometer is stable");
      }
      Serial.println("=========================================================\n");
    }
  }

  // Accelerometer stability test (Test 2 - runs after mag test)
  static float accelX_samples[100];
  static float accelY_samples[100];
  static float accelZ_samples[100];
  static int accel_sample_idx = 0;

  if (mag_sample_idx >= 100 && accel_sample_idx < 100) {
    accelX_samples[accel_sample_idx] = accelCal.x;
    accelY_samples[accel_sample_idx] = accelCal.y;
    accelZ_samples[accel_sample_idx] = accelCal.z;
    accel_sample_idx++;

    if (accel_sample_idx == 100) {
      // Calculate mean
      float meanX = 0, meanY = 0, meanZ = 0;
      for (int i = 0; i < 100; i++) {
        meanX += accelX_samples[i];
        meanY += accelY_samples[i];
        meanZ += accelZ_samples[i];
      }
      meanX /= 100; meanY /= 100; meanZ /= 100;

      // Calculate standard deviation
      float stdX = 0, stdY = 0, stdZ = 0;
      for (int i = 0; i < 100; i++) {
        stdX += (accelX_samples[i] - meanX) * (accelX_samples[i] - meanX);
        stdY += (accelY_samples[i] - meanY) * (accelY_samples[i] - meanY);
        stdZ += (accelZ_samples[i] - meanZ) * (accelZ_samples[i] - meanZ);
      }
      stdX = sqrtf(stdX / 100);
      stdY = sqrtf(stdY / 100);
      stdZ = sqrtf(stdZ / 100);

      float accelMag = sqrtf(meanX*meanX + meanY*meanY + meanZ*meanZ);

      Serial.println("\n=========================================================");
      Serial.println("ACCEL STABILITY TEST (100 samples at rest)");
      Serial.println("=========================================================");
      Serial.print("Mean: X="); Serial.print(meanX, 4);
      Serial.print(" Y="); Serial.print(meanY, 4);
      Serial.print(" Z="); Serial.print(meanZ, 4);
      Serial.print(" g  ||Accel||="); Serial.print(accelMag, 4); Serial.println(" g");
      Serial.print("Std Dev: X="); Serial.print(stdX, 5); Serial.println(" g");
      Serial.print("         Y="); Serial.print(stdY, 5); Serial.println(" g");
      Serial.print("         Z="); Serial.print(stdZ, 5); Serial.println(" g");

      // Expected: Accelerometer measures UPWARD normal force (reaction to gravity)
      // In NED frame (+Z = down), upward force is NEGATIVE Z
      Serial.println("\nExpected for level device in NED frame (down = +Z):");
      Serial.println("  X ≈ 0 g, Y ≈ 0 g, Z ≈ -1.0 g (upward reaction force)");
      Serial.print("Current orientation: ");
      if (fabsf(meanZ + 1.0f) < 0.1f && fabsf(meanX) < 0.1f && fabsf(meanY) < 0.1f) {
        Serial.println("LEVEL");
      } else {
        float pitch_est = asinf(-meanX) * 180.0f / M_PI;
        float roll_est = atan2f(meanY, -meanZ) * 180.0f / M_PI;  // Negate Z for roll calc
        Serial.print("TILTED (est pitch="); Serial.print(pitch_est, 1);
        Serial.print("°, roll="); Serial.print(roll_est, 1); Serial.println("°)");
      }

      Serial.println("\nTarget: < 0.01 g std dev on each axis for stable attitude");
      if (stdX > 0.02 || stdY > 0.02 || stdZ > 0.02) {
        Serial.println("RESULT: HIGH NOISE - Accelerometer too noisy");
      } else if (stdX > 0.01 || stdY > 0.01 || stdZ > 0.01) {
        Serial.println("RESULT: MODERATE NOISE - May see some drift");
      } else {
        Serial.println("RESULT: LOW NOISE - Accelerometer is stable");
      }
      Serial.println("=========================================================\n");
    }
  }

  // Low-pass filter magnetometer to reduce noise
  static imu::Vec3f mag_filtered = {0, 0, 0};
  const float alpha = 0.2f;  // Filter coefficient: 0.2 = 20% new, 80% old (adjust 0.1-0.3 for more/less filtering)

  // First-time initialization
  if (mag_filtered.x == 0 && mag_filtered.y == 0 && mag_filtered.z == 0) {
    mag_filtered = magCal;
  }

  // Apply exponential moving average filter
  mag_filtered.x = alpha * magCal.x + (1.0f - alpha) * mag_filtered.x;
  mag_filtered.y = alpha * magCal.y + (1.0f - alpha) * mag_filtered.y;
  mag_filtered.z = alpha * magCal.z + (1.0f - alpha) * mag_filtered.z;

  // Update AHRS orientation with calibrated sensor data and filtered magnetometer
  const imu::Vec3f& gyroRad_s = gyroCal;
  // Convert accel from g to m/s² for Mahony filter
  const imu::Vec3f accel_mss = { accelCal.x * 9.81f, accelCal.y * 9.81f, accelCal.z * 9.81f };
  // Mag axis mapping inverts Y for heading display (atan2(y,x) convention),
  // but Mahony filter needs true NED body frame (+Y = starboard/East)
  const imu::Vec3f mag_ned = { mag_filtered.x, -mag_filtered.y, mag_filtered.z };
  math::updateOrientation(gyroRad_s, accel_mss, mag_ned, dt);

  // Extract Euler angles from quaternion
  Euler attitude = quatToEulerRad(ahrs.q);
  float roll_deg = attitude.roll * 180.0f / M_PI;
  float pitch_deg = attitude.pitch * 180.0f / M_PI;
  float heading_deg = headingDegFromYawRad(attitude.yaw, 14.88f);  // 14°53'E declination: magnetic north is east of true north

  // Calculate raw magnetometer heading (independent of AHRS) for comparison
  // This is tilt-compensated heading directly from mag/accel
  float mag_heading_deg = 0.0f;
  float mag_heading_simple_deg = 0.0f;  // Also calculate without tilt compensation for comparison
  {
    // Simple heading (no tilt compensation) - only valid when level
    mag_heading_simple_deg = atan2f(mag_filtered.y, mag_filtered.x) * 180.0f / M_PI;
    if (mag_heading_simple_deg < 0) mag_heading_simple_deg += 360.0f;

    // Tilt-compensated heading using accelerometer to determine "down"
    // Normalize accelerometer to get gravity direction
    float ax = accelCal.x;
    float ay = accelCal.y;
    float az = accelCal.z;
    float a_norm = sqrtf(ax*ax + ay*ay + az*az);
    if (a_norm > 0.1f) {
      ax /= a_norm; ay /= a_norm; az /= a_norm;
    }

    // Project magnetometer onto horizontal plane (perpendicular to gravity)
    // Remove component parallel to gravity: mag_h = mag - (mag·accel)*accel
    float mag_dot_a = mag_filtered.x*ax + mag_filtered.y*ay + mag_filtered.z*az;
    float mag_x_h = mag_filtered.x - mag_dot_a * ax;
    float mag_y_h = mag_filtered.y - mag_dot_a * ay;
    float mag_z_h = mag_filtered.z - mag_dot_a * az;

    // Heading from horizontal components
    // For NED frame: North=X+, East=Y+
    mag_heading_deg = atan2f(mag_y_h, mag_x_h) * 180.0f / M_PI;
    if (mag_heading_deg < 0) mag_heading_deg += 360.0f;
  }

  // Simplified output - show both AHRS heading and raw mag heading
  static uint32_t printCount = 0;
  if (printCount++ % 4 == 0) {  // Every 4th loop (once per second at 250ms delay)
    Serial.print("AHRS: "); Serial.print(heading_deg, 1); Serial.print("°");
    Serial.print("  MagTC: "); Serial.print(mag_heading_deg, 1); Serial.print("°");  // Tilt-compensated
    Serial.print("  MagRaw: "); Serial.print(mag_heading_simple_deg, 1); Serial.print("°");  // No tilt comp
    Serial.print("  P: "); Serial.print(pitch_deg, 1); Serial.print("°");
    Serial.print("  R: "); Serial.print(roll_deg, 1); Serial.print("°");

    // Show gyro to detect drift
    Serial.print("  Gyro: [");
    Serial.print(gyroCal.x * 180.0f / M_PI, 2); Serial.print(",");
    Serial.print(gyroCal.y * 180.0f / M_PI, 2); Serial.print(",");
    Serial.print(gyroCal.z * 180.0f / M_PI, 2); Serial.println("] °/s");
  }

  logging::LogEntry entry{
    .timestamp_ms = millis(),
    .mag_raw = magRaw, 
    .accel_raw = accelRaw, 
    .gyro_raw = gyroRaw, 
    .mag_cal = magCal,   
    .accel_cal = accelCal, 
    .gyro_cal = gyroCal,  
    .heading_deg = heading_deg,
    .roll_deg = roll_deg,     
    .pitch_deg = pitch_deg     
  };

  // Temporarily disabled to reduce output clutter during testing
  // ui::console_update(magRaw, magCal, accelRaw, accelCal, gyroRaw, gyroCal, heading_deg);

  if (!logging::logEntry(entry)) {
    // Silent logging - only report if it fails repeatedly
    static int log_fail_count = 0;
    if (++log_fail_count % 20 == 0) {
      Serial.println("Warning: Logging failures detected");
    }
  }
  //stateMachine::update(dt);     // handles BOOT/CAL/NAV/ERROR

  delay(250);  // 5 Hz printout
}

} // namespace dpvnav