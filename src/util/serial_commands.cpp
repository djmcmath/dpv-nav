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
  Serial.println("  axis_test            - Guided capture at known attitudes; verdict + repo fixture");
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

// ---------------------------------------------------------------------------
// axis_test -- guided sensor-frame capture at KNOWN physical attitudes
// ---------------------------------------------------------------------------
// Walks the operator through a fixed set of known attitudes, records averaged
// sensor readings at each, prints an immediate frame-consistency verdict, and
// emits a paste-ready CSV that gets checked into the repo as the ground-truth
// fixture for the orientation math.
//
// WHY IT LOOKS LIKE THIS NOW. The previous version sampled four LEVEL headings
// and printed the numbers with no verdict. Every other instrument we had was
// equally blind to tilt: the archived nav logs never exceed +-5 deg of
// pitch/roll, and tools/orient_equivalence.py plus tests/test_coverage.py are
// fully synthetic -- they build their samples from the same convention the
// code under test assumes, so they can only ever prove self-consistency. A
// real axis-convention error (mag delivered Y-mirrored, consumed alongside
// right-handed accel -- see CLAUDE.md's "Magnetometer Coordinate Frame")
// therefore survived every test we had, and only surfaced when a diver held
// the unit at 45 deg on a real bench and watched the highlighted cell jump to
// the opposite side of the compass. A fixture captured from real hardware at
// known attitudes is the only instrument that can catch that class of bug,
// because it is the only one whose ground truth does not come from the code.
//
// THE HEADLINE METRIC IS MAGNETIC DIP CONSISTENCY. The angle between the
// magnetic field vector and gravity-down is a property of your physical
// location: it must read the SAME at every attitude, and it is independent of
// heading, of declination, and of which hemisphere you are in. If mag and
// accel are expressed in frames of opposite handedness, that angle instead
// swings between attitudes -- most violently with the unit on its side, where
// gravity lies along the mirrored axis. One number, no ground-truth heading
// required, and it fails loudly for exactly the bug that got us.

namespace {

struct FramePose {
  const char* slug;      // stable identifier -- becomes the fixture's first column
  const char* prompt;    // what the operator should physically do
  int8_t accelOrient;    // expected classifyAccelOrientation() index, -1 = hand-held
  float  hdgDeg;         // MAGNETIC heading of the nose (NAN = undefined here)
  float  pitchDeg;       // nose elevation, + = up
  float  rollDeg;        // bank, + = right side down (NAN = undefined here)
};

// Two tiers, deliberately:
//
//  * Gravity-snapped poses (accelOrient >= 0) can be held EXACTLY -- rest the
//    unit on a flat surface or against a table edge -- and
//    classifyAccelOrientation() verifies the operator actually held the one
//    that was asked for instead of trusting them. These pin the sign of every
//    accel axis, and of every mag axis at level/inverted/on-side.
//  * Hand-held poses (accelOrient == -1) are approximate (+-10 deg is normal)
//    and exist to exercise the COMBINED tilt+roll path -- the one that
//    actually broke. Assertions against them have to stay at bin granularity
//    (30 deg sectors, 30 deg bands), so aim at the middle of a cell.
//
// Headings are MAGNETIC, not true: everything downstream works in the magnetic
// frame, and declination would only add another way to be wrong. Read them off
// a phone compass -- the tolerance is +-15 deg.
//
// The bearings are 015/105/195/285 rather than the obvious N/E/S/W because the
// heading sectors these samples get binned into are 30 deg wide starting at 0,
// so due north sits exactly ON the sector 11 / sector 0 boundary and ANY aiming
// error lands in a different cell than intended. Aiming at sector centres
// instead buys the full +-15 deg before a pose changes cell. Pitch (+-45, mid
// band) and roll (0/90/180/270, all sector centres) are already centred.
const FramePose kFramePoses[] = {
  // -- gravity-snapped, exactly holdable, pose verified -----------------------
  { "level_015",     "LEVEL and upright, nose on compass bearing 015",           5,   15.0f,   0.0f,   0.0f },
  { "level_105",     "LEVEL and upright, nose on compass bearing 105",           5,  105.0f,   0.0f,   0.0f },
  { "level_195",     "LEVEL and upright, nose on compass bearing 195",           5,  195.0f,   0.0f,   0.0f },
  { "level_285",     "LEVEL and upright, nose on compass bearing 285",           5,  285.0f,   0.0f,   0.0f },
  { "inverted_015",  "UPSIDE DOWN and level, nose on bearing 015",               4,   15.0f,   0.0f, 180.0f },
  { "right_side_015","Resting on its RIGHT side (LEFT side up), nose on 015",    3,   15.0f,   0.0f,  90.0f },
  { "left_side_015", "Resting on its LEFT side (RIGHT side up), nose on 015",    2,   15.0f,   0.0f, -90.0f },
  { "nose_up_90",    "Nose pointing STRAIGHT UP (bearing does not matter)",      0,    NAN,   90.0f,   NAN  },
  { "nose_down_90",  "Nose pointing STRAIGHT DOWN (bearing does not matter)",    1,    NAN,  -90.0f,   NAN  },
  // -- hand-held, combined tilt + roll: the path that actually failed ---------
  { "up45_015",      "Nose UP about 45 deg, upright, nose on bearing 015",      -1,   15.0f,  45.0f,   0.0f },
  { "up45_rollr_015","Nose UP about 45 deg, rolled RIGHT 90 deg, bearing 015",  -1,   15.0f,  45.0f,  90.0f },
  { "down45_105",    "Nose DOWN about 45 deg, upright, nose on bearing 105",    -1,  105.0f, -45.0f,   0.0f },
  { "up45_rolll_195","Nose UP about 45 deg, rolled LEFT 90 deg, bearing 195",   -1,  195.0f,  45.0f, -90.0f },
};
constexpr int kFramePoseCount = sizeof(kFramePoses) / sizeof(kFramePoses[0]);

struct PoseCapture {
  float mx, my, mz;   // raw logical-frame mag counts, averaged (what the CSV wants)
  float ax, ay, az;   // calibrated accel, g
  float gx, gy, gz;   // calibrated gyro, rad/s
  float cmx, cmy, cmz;  // mag with the installed MagCalib applied, for the dip check
  float dipDeg;       // angle between the field and gravity-down, at this pose
  bool  verified;     // pose confirmed by classifyAccelOrientation (or N/A)
  bool  still;        // accel magnitude held steady through the capture
};

PoseCapture g_poseCaps[kFramePoseCount];

// Averages a burst of reads at the current attitude. Returns false only if the
// sensors would not read at all.
bool captureFramePose(int idx, const MagCalib& magCal) {
  const FramePose& p = kFramePoses[idx];
  PoseCapture& c = g_poseCaps[idx];

  constexpr int kReads = 40;
  double smx = 0, smy = 0, smz = 0;
  double sax = 0, say = 0, saz = 0;
  double sgx = 0, sgy = 0, sgz = 0;
  float amagMin = 1e9f, amagMax = -1e9f;
  int votes[7] = {0};   // index 6 == "matched no orientation"
  int n = 0;

  for (int i = 0; i < kReads; i++) {
    imu::Vec3i16 magRaw, accelRaw;
    imu::Vec3f accelRawG, accelCal, gyroRawV, gyroCal;
    if (imu::readMagRaw(magRaw) == imu::ImuStatus::Ok &&
        imu::readAccelRaw(accelRaw) == imu::ImuStatus::Ok &&
        imu::readAccel_g_raw_cal(accelRawG, accelCal) == imu::ImuStatus::Ok &&
        imu::readGyro_rad_s_raw_cal(gyroRawV, gyroCal) == imu::ImuStatus::Ok) {
      smx += magRaw.x;   smy += magRaw.y;   smz += magRaw.z;
      sax += accelCal.x; say += accelCal.y; saz += accelCal.z;
      sgx += gyroCal.x;  sgy += gyroCal.y;  sgz += gyroCal.z;

      const float amag = sqrtf(accelCal.x * accelCal.x +
                               accelCal.y * accelCal.y +
                               accelCal.z * accelCal.z);
      if (amag < amagMin) amagMin = amag;
      if (amag > amagMax) amagMax = amag;

      const int oi = imu::classifyAccelOrientation(accelRaw);
      votes[(oi >= 0 && oi < 6) ? oi : 6]++;
      n++;
    }
    delay(15);
  }
  if (n == 0) return false;

  c.mx = (float)(smx / n); c.my = (float)(smy / n); c.mz = (float)(smz / n);
  c.ax = (float)(sax / n); c.ay = (float)(say / n); c.az = (float)(saz / n);
  c.gx = (float)(sgx / n); c.gy = (float)(sgy / n); c.gz = (float)(sgz / n);

  // Same calibration application the gap-fill path uses: softIron * (raw - bias).
  const float xc = c.mx - magCal.bias.x;
  const float yc = c.my - magCal.bias.y;
  const float zc = c.mz - magCal.bias.z;
  c.cmx = magCal.softIron[0][0]*xc + magCal.softIron[0][1]*yc + magCal.softIron[0][2]*zc;
  c.cmy = magCal.softIron[1][0]*xc + magCal.softIron[1][1]*yc + magCal.softIron[1][2]*zc;
  c.cmz = magCal.softIron[2][0]*xc + magCal.softIron[2][1]*yc + magCal.softIron[2][2]*zc;

  // Dip: angle of the field below the horizontal plane, i.e. its component
  // along gravity-down. Taking down as -accel (the specific-force convention
  // this board reads in) only fixes the SIGN of dip; it is the same at every
  // pose either way, so the spread -- the thing being tested -- is unaffected.
  const float an = sqrtf(c.ax*c.ax + c.ay*c.ay + c.az*c.az);
  const float mn = sqrtf(c.cmx*c.cmx + c.cmy*c.cmy + c.cmz*c.cmz);
  if (an > 1e-6f && mn > 1e-6f) {
    float d = (c.cmx * -c.ax + c.cmy * -c.ay + c.cmz * -c.az) / (an * mn);
    if (d > 1.0f) d = 1.0f;
    if (d < -1.0f) d = -1.0f;
    c.dipDeg = asinf(d) * 57.2957795f;
  } else {
    c.dipDeg = NAN;
  }

  c.still = (amagMax - amagMin) < 0.08f;
  if (p.accelOrient < 0) {
    c.verified = true;    // hand-held pose: nothing to verify against
  } else {
    // Majority of reads must agree with the pose that was asked for.
    c.verified = votes[p.accelOrient] > (n / 2);
  }
  return true;
}

}  // namespace

static void cmdAxisTest() {
  Serial.println("\n========================================================================");
  Serial.println("SENSOR FRAME CAPTURE -- ground truth for the orientation math");
  Serial.println("========================================================================");
  Serial.print("You will be walked through ");
  Serial.print(kFramePoseCount);
  Serial.println(" known attitudes. For each one:");
  Serial.println("  - put the unit in the attitude described");
  Serial.println("  - hold it STILL, then press Enter");
  Serial.println("  - a burst of readings is averaged (about 1 second)");
  Serial.println();
  Serial.println("Poses that rest on a surface can be held exactly -- do that where you");
  Serial.println("can. Hand-held tilted poses only need to be within about 10 degrees.");
  Serial.println("Bearings are MAGNETIC, read off a phone compass; +-15 deg is fine.");
  Serial.println("Do this away from steel furniture, speakers, and laptops.");

  MagCalib magCal{};
  imu::getMagCalibration(magCal);
  const bool calLooksIdentity =
      fabsf(magCal.bias.x) < 1e-6f && fabsf(magCal.bias.y) < 1e-6f &&
      fabsf(magCal.bias.z) < 1e-6f && fabsf(magCal.softIron[0][0] - 1.0f) < 1e-6f;
  if (calLooksIdentity) {
    Serial.println("\n  [!] No magnetometer calibration appears to be installed. The capture");
    Serial.println("      still works, but the dip-consistency verdict below will be");
    Serial.println("      meaningless -- hard iron alone can move it tens of degrees.");
    Serial.println("      Run a baseline cal first if you want the verdict to mean anything.");
  }

  Serial.println("\nPress Enter to begin...");
  while (!Serial.available()) { delay(10); }
  Serial.readStringUntil('\n');

  for (int i = 0; i < kFramePoseCount; i++) {
    const FramePose& p = kFramePoses[i];

    while (true) {
      Serial.println();
      Serial.print("--- Pose "); Serial.print(i + 1);
      Serial.print(" of "); Serial.print(kFramePoseCount);
      Serial.print("  ["); Serial.print(p.slug); Serial.println("]");
      Serial.print("    "); Serial.println(p.prompt);
      if (p.accelOrient >= 0) {
        Serial.print("    (rest it on a surface if you can -- expecting: ");
        Serial.print(imu::kAccelOrientationNames[p.accelOrient]);
        Serial.println(")");
      }
      Serial.print("    Hold still, then press Enter...");
      while (!Serial.available()) { delay(10); }
      Serial.readStringUntil('\n');
      Serial.println(" sampling");

      if (!captureFramePose(i, magCal)) {
        Serial.println("    [!] Sensors would not read. Check I2C, then press Enter to retry.");
        while (!Serial.available()) { delay(10); }
        Serial.readStringUntil('\n');
        continue;
      }

      const PoseCapture& c = g_poseCaps[i];
      Serial.printf("    mag(raw counts) [%9.1f, %9.1f, %9.1f]\n", c.mx, c.my, c.mz);
      Serial.printf("    accel(g)        [%9.3f, %9.3f, %9.3f]\n", c.ax, c.ay, c.az);
      Serial.printf("    dip at this pose: %+.1f deg\n", c.dipDeg);

      bool problem = false;
      if (!c.still) {
        Serial.println("    [!] The unit was moving during the capture.");
        problem = true;
      }
      if (!c.verified) {
        Serial.println("    [!] That is not the attitude that was asked for.");
        problem = true;
      }
      if (!problem) break;

      Serial.print("    Press Enter to retry, or type 'keep' + Enter to accept it anyway: ");
      String resp = Serial.readStringUntil('\n');
      resp.trim();
      if (resp.startsWith("keep")) {
        Serial.println("keeping");
        break;
      }
    }
  }

  // ------------------------------------------------------------------ verdicts
  Serial.println("\n\n========================================================================");
  Serial.println("VERDICT 1 -- dip consistency (are mag and accel in the same frame?)");
  Serial.println("========================================================================");
  float dipMin = 1e9f, dipMax = -1e9f;
  const char* dipMinPose = "";
  const char* dipMaxPose = "";
  for (int i = 0; i < kFramePoseCount; i++) {
    const float d = g_poseCaps[i].dipDeg;
    if (isnan(d)) continue;
    if (d < dipMin) { dipMin = d; dipMinPose = kFramePoses[i].slug; }
    if (d > dipMax) { dipMax = d; dipMaxPose = kFramePoses[i].slug; }
  }
  const float dipSpread = dipMax - dipMin;
  Serial.printf("  lowest  %+7.1f deg  at %s\n", dipMin, dipMinPose);
  Serial.printf("  highest %+7.1f deg  at %s\n", dipMax, dipMaxPose);
  Serial.printf("  SPREAD  %7.1f deg\n\n", dipSpread);
  if (dipSpread < 8.0f) {
    Serial.println("  PASS -- dip holds steady across every attitude, so mag and accel");
    Serial.println("  agree about handedness. Residual spread is cal quality plus how");
    Serial.println("  precisely the hand-held poses were held.");
  } else {
    Serial.println("  FAIL -- dip is supposed to be a property of your location, so it");
    Serial.println("  cannot change with how you hold the unit. A spread this large means");
    Serial.println("  the mag and accel vectors are NOT in the same frame. Look first at");
    Serial.println("  which poses are the extremes: if they are the on-side ones, the");
    Serial.println("  disagreement is on the Y axis (see magMap in nav_main.cpp and the");
    Serial.println("  magNED negation the Mahony path applies but the cal path does not).");
  }

  Serial.println("\n========================================================================");
  Serial.println("VERDICT 2 -- individual axis directions");
  Serial.println("========================================================================");
  const PoseCapture& lvl015 = g_poseCaps[0];
  const PoseCapture& lvl105 = g_poseCaps[1];
  const PoseCapture& lvl195 = g_poseCaps[2];
  const PoseCapture& lvl285 = g_poseCaps[3];
  const PoseCapture& inv  = g_poseCaps[4];
  const PoseCapture& rgt  = g_poseCaps[5];
  const PoseCapture& up90 = g_poseCaps[7];
  const PoseCapture& dn90 = g_poseCaps[8];

  Serial.printf("  accel az level        %+7.3f g   -> %s\n", lvl015.az,
                lvl015.az < -0.5f ? "specific force; gravity-down = MINUS accel  [as assumed]"
                                  : "!! level reads +1g; gravity-down = PLUS accel");
  Serial.printf("  accel ax nose-up      %+7.3f g   -> %s\n", up90.ax,
                up90.ax > 0.5f ? "+X is the nose  [as assumed]"
                               : "!! +X points AFT, not forward");
  Serial.printf("  accel ax nose-down    %+7.3f g   -> %s\n", dn90.ax,
                dn90.ax < -0.5f ? "consistent with nose-up" : "!! inconsistent with nose-up");
  Serial.printf("  accel ay right-down   %+7.3f g   -> %s\n", rgt.ay,
                rgt.ay < -0.5f ? "+Y is the RIGHT side  [as assumed]"
                               : "!! +Y points LEFT");
  Serial.printf("  accel az inverted     %+7.3f g   -> %s\n", inv.az,
                inv.az > 0.5f ? "consistent with level" : "!! inconsistent with level");
  Serial.printf("  mag mx  015 / 195   %+9.1f / %+9.1f -> %s\n", lvl015.cmx, lvl195.cmx,
                lvl015.cmx > lvl195.cmx ? "mag +X agrees with the nose  [as assumed]"
                                        : "!! mag +X points AFT");
  Serial.printf("  mag mz  level/inv   %+9.1f / %+9.1f -> %s\n", lvl015.cmz, inv.cmz,
                (lvl015.cmz * inv.cmz) < 0.0f ? "mag +Z agrees with accel +Z  [as assumed]"
                                              : "!! mag Z does not flip when the unit does");

  // The one that matters. Pointing east in a genuinely right-handed NED body
  // frame, the field's northward component lies to the device's LEFT, so my
  // must read NEGATIVE. A positive reading means the delivered mag frame is
  // mirrored in Y relative to accel -- which is invisible at level (the mirror
  // cancels against the atan2(my,mx) heading formula) and corrupts everything
  // the moment a rotation derived from accel gets applied to the mag vector.
  Serial.printf("\n  mag my bearing 105  %+9.1f          -> %s\n", lvl105.cmy,
                lvl105.cmy > 0.0f
                  ? "MIRRORED: mag Y is left-handed vs accel Y"
                  : "mag Y is right-handed, matching accel Y");
  Serial.printf("  mag my bearing 285  %+9.1f          (should be the opposite sign)\n", lvl285.cmy);

  // -------------------------------------------------------------------- fixture
  Serial.println("\n========================================================================");
  Serial.println("FIXTURE -- save everything below as a file in dpv-nav/tools/fixtures/");
  Serial.println("========================================================================");
  Serial.println("---8<--- frame_fixture.csv ---8<---");
  Serial.println("pose,true_hdg_deg,true_pitch_deg,true_roll_deg,mx,my,mz,ax,ay,az,gx,gy,gz,verified,still");
  for (int i = 0; i < kFramePoseCount; i++) {
    const FramePose& p = kFramePoses[i];
    const PoseCapture& c = g_poseCaps[i];
    Serial.printf("%s,", p.slug);
    if (isnan(p.hdgDeg)) Serial.print("nan,"); else Serial.printf("%.1f,", p.hdgDeg);
    Serial.printf("%.1f,", p.pitchDeg);
    if (isnan(p.rollDeg)) Serial.print("nan,"); else Serial.printf("%.1f,", p.rollDeg);
    Serial.printf("%.1f,%.1f,%.1f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%d,%d\n",
                  c.mx, c.my, c.mz,
                  (double)c.ax, (double)c.ay, (double)c.az,
                  (double)c.gx, (double)c.gy, (double)c.gz,
                  c.verified ? 1 : 0, c.still ? 1 : 0);
  }
  Serial.println("---8<--- end ---8<---");

  // The counts above are meaningless without the calibration they were taken
  // under -- a fixture that silently picks up whatever cal happens to be
  // installed when it is replayed is not ground truth.
  Serial.println("\n---8<--- frame_fixture_cal.json ---8<---");
  Serial.printf("{\"bias\":[%.6f,%.6f,%.6f],\"soft_iron\":[[%.6f,%.6f,%.6f],[%.6f,%.6f,%.6f],[%.6f,%.6f,%.6f]]}\n",
                (double)magCal.bias.x, (double)magCal.bias.y, (double)magCal.bias.z,
                (double)magCal.softIron[0][0], (double)magCal.softIron[0][1], (double)magCal.softIron[0][2],
                (double)magCal.softIron[1][0], (double)magCal.softIron[1][1], (double)magCal.softIron[1][2],
                (double)magCal.softIron[2][0], (double)magCal.softIron[2][1], (double)magCal.softIron[2][2]);
  Serial.println("---8<--- end ---8<---");
  Serial.println("\n========================================================================\n");
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
