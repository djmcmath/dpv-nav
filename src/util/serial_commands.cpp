#include "serial_commands.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <math.h>
#include "../sensors/imu.h"
#include "../math/orientation.h"
#include "../util/mag_cal_collect.h"

// Stability test state
static bool mag_stability_active = false;
static bool accel_stability_active = false;
static float stabX_samples[100];
static float stabY_samples[100];
static float stabZ_samples[100];
static int stab_sample_idx = 0;

namespace serial_cmd {

void printHelp() {
  Serial.println("=== DPV-Nav Ready ===");
  Serial.println("Serial commands:");
  Serial.println("  start_cal            - Start magnetometer calibration data collection (30 sec)");
  Serial.println("  dump_cal             - Dump collected calibration data to serial (copy to PC)");
  Serial.println("  clear_cal            - Clear mag calibration data from LittleFS");
  Serial.println("  reset_all_cal        - Delete ALL calibrations - requires reboot");
  Serial.println("  mag_stability        - Magnetometer stability test (100 samples at rest)");
  Serial.println("  accel_stability      - Accelerometer stability test (100 samples at rest)");
  Serial.println("  sensor_orientation   - Show RAW sensor frames (no axis mapping)");
  Serial.println("  debug_axes           - End-to-end axis analysis (trace data)");
  Serial.println("  axis_test            - Test sensor axes to find correct orientation");
  Serial.println("  accel_orient         - Which accel-cal orientation matches as you rotate?");
  Serial.println("  help                 - Show this message");
  Serial.println("=====================\n");
}

static void cmdStartCal() {
  Serial.println("[CMD] Starting magnetometer calibration data collection...");
  if (!mag_cal::startCollection(120000)) {
    Serial.println("[CMD] Failed to start collection!");
  }
}

static void cmdDumpCal() {
  Serial.println("[CMD] Dumping calibration data...");
  mag_cal::dumpToSerial();
}

static void cmdClearCal() {
  Serial.println("[CMD] Clearing magnetometer calibration data...");
  mag_cal::clearData();
}

static void cmdResetAllCal() {
  Serial.println("[CMD] Clearing ALL calibration files...");
  if (LittleFS.begin(true)) {
    LittleFS.remove("/mag_cal.json");
    LittleFS.remove("/gyro_cal.json");
    LittleFS.remove("/accel_cal.json");
    LittleFS.remove("/mag_cal_samples.csv");
    Serial.println("[CMD] All calibration files deleted. Reboot to re-calibrate.");
  } else {
    Serial.println("[CMD] Error: LittleFS mount failed");
  }
}

static void cmdResetAccelCal() {
  Serial.println("[CMD] Clearing accelerometer calibration data...");
  if (LittleFS.begin(true)) {
    LittleFS.remove("/accel_cal.json");
    Serial.println("[CMD] Accelerometer calibration file deleted. Reboot to re-calibrate.");
  } else {
    Serial.println("[CMD] Error: LittleFS mount failed");
  }
}

static void cmdSensorOrientation() {
  Serial.println("\n========================================================================");
  Serial.println("SENSOR PHYSICAL ORIENTATION TEST");
  Serial.println("========================================================================");
  Serial.println("This reads sensors in their NATIVE frame (no axis mapping applied).");
  Serial.println("Place device LEVEL on desk, then press Enter...");
  while (!Serial.available()) { delay(10); }
  Serial.readStringUntil('\n');

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

static void cmdDebugAxes(MahonyState& ahrs) {
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
  imu::Vec3f magCalV, accelCalV, gyroCalV;
  imu::readMag_raw_cal(magRawFloat, magCalV);
  imu::readAccel_g_raw_cal(accelRawFloat, accelCalV);
  imu::readGyro_rad_s_raw_cal(gyroRawFloat, gyroCalV);

  Serial.println("\n--- STAGE 2: After Unit Conversion (before calibration) ---");
  Serial.print("  Mag (µT):  ["); Serial.print(magRawFloat.x, 2); Serial.print(", ");
  Serial.print(magRawFloat.y, 2); Serial.print(", "); Serial.print(magRawFloat.z, 2); Serial.println("]");
  Serial.print("  Accel (g): ["); Serial.print(accelRawFloat.x, 4); Serial.print(", ");
  Serial.print(accelRawFloat.y, 4); Serial.print(", "); Serial.print(accelRawFloat.z, 4); Serial.println("]");
  Serial.print("  Gyro (°/s): ["); Serial.print(gyroRawFloat.x * 57.3f, 2); Serial.print(", ");
  Serial.print(gyroRawFloat.y * 57.3f, 2); Serial.print(", "); Serial.print(gyroRawFloat.z * 57.3f, 2); Serial.println("]");

  Serial.println("\n--- STAGE 3: After Calibration (what Mahony receives) ---");
  Serial.print("  Mag (µT):     ["); Serial.print(magCalV.x, 2); Serial.print(", ");
  Serial.print(magCalV.y, 2); Serial.print(", "); Serial.print(magCalV.z, 2); Serial.println("]");
  Serial.print("  Accel (g):    ["); Serial.print(accelCalV.x, 4); Serial.print(", ");
  Serial.print(accelCalV.y, 4); Serial.print(", "); Serial.print(accelCalV.z, 4); Serial.print("]  (");
  Serial.print(accelCalV.x * 9.81f, 2); Serial.print(", ");
  Serial.print(accelCalV.y * 9.81f, 2); Serial.print(", ");
  Serial.print(accelCalV.z * 9.81f, 2); Serial.println(" m/s²)");
  Serial.print("  Gyro (°/s):   ["); Serial.print(gyroCalV.x * 57.3f, 2); Serial.print(", ");
  Serial.print(gyroCalV.y * 57.3f, 2); Serial.print(", "); Serial.print(gyroCalV.z * 57.3f, 2); Serial.println("]");

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

  float mag_heading_simple = atan2f(magCalV.y, magCalV.x) * 180.0f / M_PI;
  if (mag_heading_simple < 0) mag_heading_simple += 360.0f;
  Serial.print("  Mag (no tilt comp): "); Serial.print(mag_heading_simple, 1); Serial.println("°");

  // Tilt-compensated mag heading
  float ax = accelCalV.x, ay = accelCalV.y, az = accelCalV.z;
  float a_norm = sqrtf(ax*ax + ay*ay + az*az);
  if (a_norm > 0.1f) { ax /= a_norm; ay /= a_norm; az /= a_norm; }
  float mag_dot_a = magCalV.x*ax + magCalV.y*ay + magCalV.z*az;
  float mag_x_h = magCalV.x - mag_dot_a * ax;
  float mag_y_h = magCalV.y - mag_dot_a * ay;
  float mag_heading_tc = atan2f(mag_y_h, mag_x_h) * 180.0f / M_PI;
  if (mag_heading_tc < 0) mag_heading_tc += 360.0f;
  Serial.print("  Mag (tilt comp):    "); Serial.print(mag_heading_tc, 1); Serial.println("°");

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

  if (fabsf(az + 1.0f) < 0.1f && fabsf(ax) < 0.1f && fabsf(ay) < 0.1f) {
    Serial.println("  ✓ Accel Z sign correct for NED frame (Z ≈ -1.0 g)");
  } else if (fabsf(az - 1.0f) < 0.1f) {
    Serial.println("  ✗ Accel Z has WRONG sign (Z ≈ +1.0 g, should be negative)");
  }

  Serial.println("========================================================================\n");
}

static void cmdAxisTest() {
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

static void cmdAccelOrientTest() {
  Serial.println("\n=== ACCEL ORIENTATION CHECK ===");
  Serial.println("Slowly rotate the device. A new line prints each time the detected");
  Serial.println("accelerometer calibration orientation changes. Press Enter to stop.\n");

  int lastIdx = -2;  // sentinel distinct from -1 (no match), forces first print
  while (!Serial.available()) {
    imu::Vec3i16 raw;
    if (imu::readAccelRaw(raw) == imu::ImuStatus::Ok) {
      int idx = imu::classifyAccelOrientation(raw);
      if (idx != lastIdx) {
        lastIdx = idx;
        Serial.print("Detected: ");
        if (idx >= 0) {
          Serial.println(imu::kAccelOrientationNames[idx]);
        } else {
          Serial.println("(tilted / not aligned to any orientation)");
        }
      }
    }
    delay(200);
  }
  Serial.readStringUntil('\n');
  Serial.println("\n=== Test stopped ===\n");
}

static void cmdMagStability() {
  Serial.println("[CMD] Starting magnetometer stability test (100 samples at rest)...");
  mag_stability_active = true;
  accel_stability_active = false;
  stab_sample_idx = 0;
}

static void cmdAccelStability() {
  Serial.println("[CMD] Starting accelerometer stability test (100 samples at rest)...");
  accel_stability_active = true;
  mag_stability_active = false;
  stab_sample_idx = 0;
}

void processInput(MahonyState& ahrs) {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toLowerCase();

  if (cmd == "start_cal")              cmdStartCal();
  else if (cmd == "dump_cal")          cmdDumpCal();
  else if (cmd == "clear_cal")         cmdClearCal();
  else if (cmd == "reset_all_cal")     cmdResetAllCal();
  else if (cmd == "reset_accel_cal")   cmdResetAccelCal();
  else if (cmd == "sensor_orientation") cmdSensorOrientation();
  else if (cmd == "debug_axes")        cmdDebugAxes(ahrs);
  else if (cmd == "axis_test")         cmdAxisTest();
  else if (cmd == "accel_orient")      cmdAccelOrientTest();
  else if (cmd == "mag_stability")     cmdMagStability();
  else if (cmd == "accel_stability")   cmdAccelStability();
  else if (cmd == "help")              printHelp();
  else if (cmd.length() > 0) {
    Serial.print("[CMD] Unknown command: '");
    Serial.print(cmd);
    Serial.println("' (type 'help' for commands)");
  }
}

void updateStabilityTest(const imu::Vec3f& magCal, const imu::Vec3f& accelCal) {
  if (mag_stability_active && stab_sample_idx < 100) {
    stabX_samples[stab_sample_idx] = magCal.x;
    stabY_samples[stab_sample_idx] = magCal.y;
    stabZ_samples[stab_sample_idx] = magCal.z;
    stab_sample_idx++;
    Serial.print("\r[Mag stability] Samples remaining: "); Serial.print(100 - stab_sample_idx); Serial.print("   ");

    if (stab_sample_idx == 100) {
      mag_stability_active = false;
      float meanX = 0, meanY = 0, meanZ = 0;
      for (int i = 0; i < 100; i++) {
        meanX += stabX_samples[i]; meanY += stabY_samples[i]; meanZ += stabZ_samples[i];
      }
      meanX /= 100; meanY /= 100; meanZ /= 100;

      float stdX = 0, stdY = 0, stdZ = 0;
      for (int i = 0; i < 100; i++) {
        stdX += (stabX_samples[i] - meanX) * (stabX_samples[i] - meanX);
        stdY += (stabY_samples[i] - meanY) * (stabY_samples[i] - meanY);
        stdZ += (stabZ_samples[i] - meanZ) * (stabZ_samples[i] - meanZ);
      }
      stdX = sqrtf(stdX / 100); stdY = sqrtf(stdY / 100); stdZ = sqrtf(stdZ / 100);
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

  if (accel_stability_active && stab_sample_idx < 100) {
    stabX_samples[stab_sample_idx] = accelCal.x;
    stabY_samples[stab_sample_idx] = accelCal.y;
    stabZ_samples[stab_sample_idx] = accelCal.z;
    stab_sample_idx++;
    Serial.print("\r[Accel stability] Samples remaining: "); Serial.print(100 - stab_sample_idx); Serial.print("   ");

    if (stab_sample_idx == 100) {
      accel_stability_active = false;
      float meanX = 0, meanY = 0, meanZ = 0;
      for (int i = 0; i < 100; i++) {
        meanX += stabX_samples[i]; meanY += stabY_samples[i]; meanZ += stabZ_samples[i];
      }
      meanX /= 100; meanY /= 100; meanZ /= 100;

      float stdX = 0, stdY = 0, stdZ = 0;
      for (int i = 0; i < 100; i++) {
        stdX += (stabX_samples[i] - meanX) * (stabX_samples[i] - meanX);
        stdY += (stabY_samples[i] - meanY) * (stabY_samples[i] - meanY);
        stdZ += (stabZ_samples[i] - meanZ) * (stabZ_samples[i] - meanZ);
      }
      stdX = sqrtf(stdX / 100); stdY = sqrtf(stdY / 100); stdZ = sqrtf(stdZ / 100);
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

      Serial.println("\nExpected for level device in NED frame (down = +Z):");
      Serial.println("  X ≈ 0 g, Y ≈ 0 g, Z ≈ -1.0 g (upward reaction force)");
      Serial.print("Current orientation: ");
      if (fabsf(meanZ + 1.0f) < 0.1f && fabsf(meanX) < 0.1f && fabsf(meanY) < 0.1f) {
        Serial.println("LEVEL");
      } else {
        float pitch_est = asinf(-meanX) * 180.0f / M_PI;
        float roll_est = atan2f(meanY, -meanZ) * 180.0f / M_PI;
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
}

}  // namespace serial_cmd
