// nav_main.cpp — Nav device entry point
// Sensors, AHRS, GPS, flow sensor, dead reckoning.
// Sends NavPacket to display device over Serial1 at ~10 Hz.

#include <Arduino.h>
#include <Wire.h>
#include <LittleFS.h>

#include "board_pins.h"
#include "config.h"
#include "types/types.h"
#include "sensors/imu.h"
#include "sensors/calib.h"
#include "math/mahony.h"
#include "math/orientation.h"
#include "drivers/flow_sensor.h"
#include "drivers/gps.h"
#include "util/storage.h"
#include "util/logging.h"
#include "nav/state.h"
#include "nav/nav_model.h"
#include "net/wifi_manager.h"
#include "net/web_server.h"
#include "util/serial_commands.h"
#include "util/mag_cal_collect.h"
#include <dpvlink.h>

// ---- AHRS state -----------------------------------------------------------
static MahonyState ahrs;
//kp: 0.5 to 2.0 is a reasonable range.  Higher: mag/accel corrections dominate faster, with less gyro drift.  Lower: gyro dominates longer, with more drift but smoother response.  
//ki: 0.0 to 0.1 -- builds up long term gyro bias estimate, correcting for drift.  Too high -- windup and oscillation.  
static MahonyParams mahonyParams{ .kp = 1.0f, .ki = 0.002f, .useMag = true };

// ---- Calibration data -----------------------------------------------------
static MagCalib  magCal;
static Calib3    gyroCal;
static Calib3    accelCal;

// ---- Nav state -------------------------------------------------------------
static SystemState sysState = SystemState::BOOT;
static uint32_t lastLoopUs  = 0;
static uint32_t lastSendMs  = 0;
static uint32_t lastDiagMs  = 0;
static constexpr uint32_t SEND_INTERVAL_MS = 100;  // 10 Hz link rate
static constexpr uint32_t LOOP_INTERVAL_US = 10000; // 100 Hz loop rate gate
static constexpr uint32_t DIAG_INTERVAL_MS = 1000;  // 0.2 Hz sensor diagnostics
static constexpr bool FULL_DIAG_ENABLE = false;  // set to false to disable periodic diagnostic prints
static constexpr bool GPS_DIAG_ENABLE  = true;   // GPS position/speed/COG coherence diagnostics

// ---- Toggle states (shared between command handler and sendNavPacket) ------
static bool gGpsPosEnabled = DEFAULT_USE_GPS_POSITION;
static bool gGpsSpdEnabled = true;   // GPS speed source enabled (stub — always true for now)
static bool gWifiEnabled   = true;   // WiFi radio enabled (surface mode default)

// ---- Serial link buffer ----------------------------------------------------
static char linkBuf[256];

// ---- Forward declarations --------------------------------------------------
static void loadCalibration();
static void sendNavPacket(float heading, float pitch, float roll,
                          float speed, bool gpsSpeed,
                          float distHome, float bearHome,
                          float posX, float posY,
                          const GpsFix& fix);
static void handleDisplayCmd();

// ===========================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n=== DPV-NAV (nav device) ===");

    // Serial link to display device
    Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);

    // I2C bus
    Wire.begin(SDA_PIN, SCL_PIN);

    // LittleFS for calibration persistence
    if (!LittleFS.begin(true)) {
        Serial.println("ERROR: LittleFS mount failed");
    }

    // IMU configuration
    imu::ImuConfig imuConfig{
        .accel_g_fullscale  = 16.0f,
        .gyro_dps_fullscale = 2000.0f,
        .mag_uT_fullscale   = 4.0f,
        .sample_hz          = 100
    };
    // AXIS MAPPING — TWO COORDINATE FRAMES
    // The LSM6DS33 (accel/gyro) and LIS3MDL (mag) have different physical Y-axis
    // directions on this board. The accelGyroMap defines the NED reference frame.
    // The magMap's -2 corrects the LIS3MDL's physical Y to match the LSM6DS33 Y.
    //
    // HOWEVER: after this mapping + calibration, the mag output uses atan2(my, mx)
    // for heading (not the NED formula atan2(-my, mx)). This means the mag Y as
    // delivered by imu::readMag*() is actually -NED_Y (left-handed for Y axis).
    // The Mahony filter requires all sensors in the SAME NED frame, so nav_main
    // negates mag.y before passing to mahonyUpdate(). See the magNED variable below.
    // DO NOT REMOVE the mag.y negation without also changing this axis map and
    // recomputing all magnetometer calibration data.
    imu::AxisMap accelGyroMap{ .x_axis = +1, .y_axis = +2, .z_axis = +3 };
    imu::AxisMap magMap{ .x_axis = +1, .y_axis = -2, .z_axis = +3 };

    sysState = SystemState::SELF_TEST;

    if (!imu::init(imuConfig, accelGyroMap, magMap)) {
        Serial.println("ERROR: IMU init failed");
        sysState = SystemState::ERROR;
    } else {
        Serial.println("IMU init OK");

        // Load or run calibration
        loadCalibration();

        // GPS
        if (gps::init()) {
            Serial.println("GPS init OK");
        } else {
            Serial.println("WARNING: GPS init failed");
        }

        // Flow sensor
        flow::FlowConfig flowCfg{
            .k_factor         = FLOW_K_FACTOR,
            .cross_section_m2 = FLOW_CROSS_SECTION_M2,
            .avg_period_s     = FLOW_AVG_PERIOD_S
        };
        flow::init(flowCfg);
        Serial.println("Flow sensor init OK");

        // Position model
        nav::init(DEFAULT_BASELINE_LAT, DEFAULT_BASELINE_LON);
        nav::setUseGps(DEFAULT_USE_GPS_POSITION);

        // AHRS
        mahonyInit(ahrs);

        // Data logging (starts in OFF state)
        logging::init();

        sysState = SystemState::READY;
    }

    // WiFi + web server — always start, even if IMU failed.
    // Placed after calibration so blocking cal doesn't starve the connection.
    wifi::init();
    web::init();
    lastLoopUs = micros();
    Serial.println("Nav device ready");
    serial_cmd::printHelp();
}

// ===========================================================================
void loop() {
    // Keep web server alive even in error state
    web::update();

    if (sysState == SystemState::ERROR) {
        delay(1);
        return;
    }

    // --- Rate gate to 100 Hz ------------------------------------------------
    uint32_t nowUs = micros();
    if (nowUs - lastLoopUs < LOOP_INTERVAL_US) return;
    float dt = (nowUs - lastLoopUs) * 1e-6f;
    lastLoopUs = nowUs;
    if (dt > 0.5f) dt = 0.01f;  // clamp on overflow

    // --- Read sensors -------------------------------------------------------
    imu::Vec3f accel, gyro, mag, magRaw, accelRaw, gyroRaw;
    imu::readAccel_g_raw_cal(accelRaw, accel);
    imu::readGyro_rad_s_raw_cal(gyroRaw, gyro);
    imu::readMag_raw_cal(magRaw, mag);  // magRaw = uncalibrated µT, mag = calibrated µT

    // --- AHRS update --------------------------------------------------------
    // The mag axis map {+1,-2,+3} puts mag Y in left-handed frame (positive=Left)
    // while accel is in right-handed NED (positive Y=Right). Mahony needs both
    // in the same NED frame, so negate mag Y before passing to the filter.
    imu::Vec3f magNED = { mag.x, -mag.y, mag.z };
    mahonyUpdate(ahrs, mahonyParams, gyro, accel, magNED, dt);

    // --- Extract Euler angles -----------------------------------------------
    Euler euler = quatToEulerRad(ahrs.q);
    float headingDeg = headingDegFromYawRad(euler.yaw, DEFAULT_DECLINATION_DEG);
    float pitchDeg   = euler.pitch * (180.0f / M_PI);
    float rollDeg    = euler.roll  * (180.0f / M_PI);

    // --- Peripheral sensors -------------------------------------------------
    gps::update();
    flow::update();

    // --- GPS COG coherence filter -------------------------------------------
    // Track consistency of Course Over Ground to distinguish real motion from
    // position jitter.  EMA of sin/cos avoids 0°/360° wrapping issues.
    // Coherence (resultant length) near 1.0 = consistent heading = real motion;
    // near 0.0 = random headings = stationary GPS wander.
    static float cogSinEma = 0.0f, cogCosEma = 0.0f;
    static bool  cogPrimed = false;

    GpsFix fix = gps::getFix();
    bool gpsFresh = fix.has_fix && (millis() - fix.fix_age_ms) < GPS_FIX_STALE_MS;

    if (gpsFresh) {
        float cogRad = fix.course_deg * (M_PI / 180.0f);
        float s = sinf(cogRad);
        float c = cosf(cogRad);
        if (!cogPrimed) {
            cogSinEma = s;
            cogCosEma = c;
            cogPrimed = true;
        } else {
            cogSinEma += GPS_COG_EMA_ALPHA * (s - cogSinEma);
            cogCosEma += GPS_COG_EMA_ALPHA * (c - cogCosEma);
        }
    }

    float cogCoherence = sqrtf(cogSinEma * cogSinEma + cogCosEma * cogCosEma);

    // Select speed source: GPS speed must pass both SOG deadband and COG coherence.
    //  - SOG < NOISE_FLOOR: always noise (position jitter at rest)
    //  - SOG > TRUST_FLOOR: always trust (clearly moving, COG drifts slowly at low speed)
    //  - In between: require COG coherence to confirm real motion
    // When GPS speed is rejected, fall back to flow sensor. If flow sensor also
    // reads 0 (no sensor connected), use GPS speed anyway — noisy GPS speed is
    // always better than zero for DR integration.
    float speed;
    bool useGpsSpeed = false;
    if (gpsFresh) {
        nav::updateGPS(fix.lat, fix.lon);
        bool sogTrusted = false;
        if (fix.speed_knots >= GPS_SOG_TRUST_FLOOR_KN) {
            sogTrusted = true;
        } else if (fix.speed_knots >= GPS_SOG_NOISE_FLOOR_KN
                   && cogCoherence >= GPS_COG_COHERENCE_THRESH) {
            sogTrusted = true;
        }
        if (sogTrusted) {
            speed = fix.speed_knots * KNOTS_TO_MS;
            useGpsSpeed = true;
        } else {
            float flowSpeed = flow::getSpeed_ms();
            if (flowSpeed > 0.0f) {
                speed = flowSpeed;
            } else {
                // No flow sensor: use GPS speed if above noise floor,
                // otherwise treat as stationary
                if (fix.speed_knots >= GPS_SOG_NOISE_FLOOR_KN) {
                    speed = fix.speed_knots * KNOTS_TO_MS;
                    useGpsSpeed = true;
                } else {
                    speed = 0.0f;
                }
            }
        }
    } else {
        speed = flow::getSpeed_ms();
    }

    // --- Dead-reckoning position update ------------------------------------
    nav::updateDR(headingDeg, speed, dt);

    // --- Send NavPacket at link rate ----------------------------------------
    uint32_t nowMs = millis();
    if (nowMs - lastSendMs >= SEND_INTERVAL_MS) {
        lastSendMs = nowMs;
        nav::Position pos = nav::getPosition();
        sendNavPacket(headingDeg, pitchDeg, rollDeg, speed, useGpsSpeed,
                     nav::distanceToHome_m(), nav::bearingToHome_deg(),
                     pos.x_m, pos.y_m, fix);

        // Data logging at 10 Hz (same rate as NavPacket)
        if (logging::isLogging()) {
            logging::LogData ld{};
            ld.timestamp_ms = nowMs;
            ld.heading_deg  = headingDeg;
            ld.speed_ms     = speed;
            ld.gpsSpeed     = useGpsSpeed;
            ld.pos_x_m      = pos.x_m;
            ld.pos_y_m      = pos.y_m;
            ld.gpsPos        = nav::isUsingGps() && gpsFresh;
            ld.mag_raw       = magRaw;
            ld.accel_raw     = accelRaw;
            ld.gyro_raw      = gyroRaw;
            ld.mag_cal       = mag;
            ld.accel_cal     = accel;
            ld.gyro_cal      = gyro;
            ld.pitch_deg     = pitchDeg;
            ld.roll_deg      = rollDeg;
            logging::log(ld);
        }
    }

    // --- Accumulate mag stats for diagnostics --------------------------------
    static float mStatMinX, mStatMaxX, mStatMinY, mStatMaxY, mStatMinZ, mStatMaxZ;
    static float mStatMinMag, mStatMaxMag;
    static uint32_t mStatN = 0;

    float magMag = sqrtf(mag.x*mag.x + mag.y*mag.y + mag.z*mag.z);
    if (mStatN == 0) {
        mStatMinX = mStatMaxX = mag.x;
        mStatMinY = mStatMaxY = mag.y;
        mStatMinZ = mStatMaxZ = mag.z;
        mStatMinMag = mStatMaxMag = magMag;
    } else {
        if (mag.x < mStatMinX) mStatMinX = mag.x; if (mag.x > mStatMaxX) mStatMaxX = mag.x;
        if (mag.y < mStatMinY) mStatMinY = mag.y; if (mag.y > mStatMaxY) mStatMaxY = mag.y;
        if (mag.z < mStatMinZ) mStatMinZ = mag.z; if (mag.z > mStatMaxZ) mStatMaxZ = mag.z;
        if (magMag < mStatMinMag) mStatMinMag = magMag;
        if (magMag > mStatMaxMag) mStatMaxMag = magMag;
    }
    mStatN++;

    // --- Sensor diagnostics at 1 Hz ----------------------------------------
    if (nowMs - lastDiagMs >= DIAG_INTERVAL_MS) {
        lastDiagMs = nowMs;
        Serial.printf("[DIAG] dt=%.4f  hdg=%.1f  pitch=%.1f  roll=%.1f\t\n",
                      dt, headingDeg, pitchDeg, rollDeg);
        //Serial.printf("[DIAG] accel(g)  X=%+.3f Y=%+.3f Z=%+.3f\t",
        //              accel.x, accel.y, accel.z);
        //Serial.printf("[DIAG] gyro(r/s) X=%+.4f Y=%+.4f Z=%+.4f\t",
        //              gyro.x, gyro.y, gyro.z);

        if (FULL_DIAG_ENABLE) {
            // Mag: current calibrated reading in µT
            Serial.printf("[MAG ] cal(uT)   X=%+.2f Y=%+.2f Z=%+.2f  |B|=%.2f\t",
                        mag.x, mag.y, mag.z, magMag);
            Serial.printf("[MAG ] raw(uT)   X=%+.2f Y=%+.2f Z=%+.2f\t",
                        magRaw.x, magRaw.y, magRaw.z);

            // Horizontal component: project out gravity direction
            float mDotA = mag.x*accel.x + mag.y*accel.y + mag.z*accel.z;
            float aMag2 = accel.x*accel.x + accel.y*accel.y + accel.z*accel.z;
            float hMag = 0.0f;
            if (aMag2 > 0.01f) {
                float s = mDotA / aMag2;
                float mhX = mag.x - s*accel.x;
                float mhY = mag.y - s*accel.y;
                float mhZ = mag.z - s*accel.z;
                hMag = sqrtf(mhX*mhX + mhY*mhY + mhZ*mhZ);
            }
            Serial.printf("[MAG ] horiz=%.2f uT  samples=%u\t", hMag, mStatN);

            // 1-second stability: per-axis and magnitude spread
            Serial.printf("[MAG ] spread: X=[%+.2f,%+.2f] Y=[%+.2f,%+.2f] Z=[%+.2f,%+.2f]\t",
                        mStatMinX, mStatMaxX, mStatMinY, mStatMaxY, mStatMinZ, mStatMaxZ);
            Serial.printf("[MAG ] |B| range: [%.2f, %.2f] uT\n", mStatMinMag, mStatMaxMag);

            // Warnings
            if (magMag < 20.0f || magMag > 80.0f)
                Serial.println("[MAG ] WARNING: |B| outside Earth range (25-65 uT)");
            if (hMag < 5.0f)
                Serial.println("[MAG ] WARNING: Horizontal component < 5 uT — heading unreliable");
            //if (mStatMaxMag - mStatMinMag > 10.0f)
            //    Serial.println("[MAG ] WARNING: |B| unstable (>10 uT swing) — interference or bad cal");
            //float spreadX = mStatMaxX - mStatMinX;
            //float spreadY = mStatMaxY - mStatMinY;
            //float spreadZ = mStatMaxZ - mStatMinZ;
            //if (spreadX > 3.0f || spreadY > 3.0f || spreadZ > 3.0f)
            //    Serial.printf("[MAG ] WARNING: Noisy at rest (spread X=%.1f Y=%.1f Z=%.1f uT)\n",
            //                  spreadX, spreadY, spreadZ);
        }

        if (GPS_DIAG_ENABLE) {
            Serial.printf("[GPS ] fix=%d  sat=%d  hdop=%.1f\t",
                          fix.has_fix, fix.satellites, fix.hdop);
            Serial.printf("pos=(%.6f, %.6f)  alt=%.1f m\n",
                          fix.lat, fix.lon, fix.altitude_m);
            Serial.printf("[GPS ] SOG=%.2f kn (%.2f m/s)  COG=%.1f deg\t",
                          fix.speed_knots, fix.speed_knots * KNOTS_TO_MS,
                          fix.course_deg);
            Serial.printf("coherence=%.3f [%s]  speed_used=%.2f m/s (%s)\n",
                          cogCoherence,
                          cogCoherence >= GPS_COG_COHERENCE_THRESH ? "PASS" : "FAIL",
                          speed,
                          useGpsSpeed ? "GPS" : "FLOW");
        }

        mStatN = 0;  // reset for next window
    }

#if ENABLE_DEBUG_PACKET
    // --- Send DebugPacket at debug rate ------------------------------------
    static uint32_t lastDebugSendMs = 0;
    if (nowMs - lastDebugSendMs >= DEBUG_SEND_INTERVAL_MS) {
        lastDebugSendMs = nowMs;
        sendDebugPacket(accel, gyro, mag, headingDeg, pitchDeg, rollDeg);
    }
#endif

    // --- WiFi / web server housekeeping --------------------------------------
    wifi::update();
    web::update();

    // --- Serial commands and mag cal collection ------------------------------
    serial_cmd::processInput(ahrs);
    serial_cmd::updateStabilityTest(mag, accel);
    if (mag_cal::isCollecting()) mag_cal::logSample();

    // --- Check for commands from display ------------------------------------
    handleDisplayCmd();
}

// ===========================================================================
// Helpers
// ===========================================================================

static void loadCalibration() {
    // Magnetometer
    if (storage::loadMagCalibration("/mag_cal.json", magCal)) {
        imu::setMagCalibration(magCal);
        Serial.println("Mag calibration loaded from LittleFS");
    } else {
        Serial.println("No mag calibration found — running min/max sweep (90 s)");
        imu::calibrateMagnetometer(magCal, 90000);
        storage::saveMagCalibration("/mag_cal.json", magCal);
    }

    // Gyroscope
    if (storage::loadCalib3("/gyro_cal.json", gyroCal)) {
        imu::setGyroCalibration(gyroCal);
        Serial.println("Gyro calibration loaded from LittleFS");
    } else {
        Serial.println("No gyro calibration found — sampling at rest (10 s)");
        imu::calibrateGyroscope(gyroCal, 10000);
        storage::saveCalib3("/gyro_cal.json", gyroCal);
    }

    // Accelerometer
    if (storage::loadCalib3("/accel_cal.json", accelCal)) {
        imu::setAccelCalibration(accelCal);
        Serial.println("Accel calibration loaded from LittleFS");
    } else {
        Serial.println("No accel calibration found — 6-point cal");
        imu::calibrateAccelerometer(accelCal, 2500);
        storage::saveCalib3("/accel_cal.json", accelCal);
    }
}

static void sendNavPacket(float heading, float pitch, float roll,
                          float speed, bool gpsSpeed,
                          float distHome, float bearHome,
                          float posX, float posY,
                          const GpsFix& fix) {
    NavPacket pkt{};
    pkt.heading_deg      = heading;
    pkt.pitch_deg        = pitch;
    pkt.roll_deg         = roll;
    pkt.speed_ms         = speed;
    pkt.distance_home_m  = distHome;
    pkt.bearing_home_deg = bearHome;
    pkt.pos_x_m          = posX;
    pkt.pos_y_m          = posY;
    pkt.system_state     = static_cast<uint8_t>(sysState);
    pkt.gps_fix_quality  = fix.fix_quality;
    pkt.gps_satellites   = fix.satellites;
    pkt.uptime_ms        = millis();

    uint8_t flags = 0;
    if (gpsSpeed) flags |= FLAG_GPS_SPEED;
    if (nav::hasHome()) flags |= FLAG_HAS_HOME;
    flags |= FLAG_TRUE_HEADING;  // declination applied in heading calculation
    if (gGpsPosEnabled) flags |= FLAG_GPS_POS_ENABLED;
    if (gWifiEnabled)   flags |= FLAG_WIFI_ENABLED;
    if (gGpsSpdEnabled) flags |= FLAG_GPS_SPD_ENABLED;
    flags |= (static_cast<uint8_t>(logging::getLevel()) << FLAG_LOG_LEVEL_SHIFT) & FLAG_LOG_LEVEL_MASK;
    pkt.flags = flags;

    size_t n = navPacketToBytes(pkt, linkBuf, sizeof(linkBuf));
    if (n > 0) {
        Serial1.write(linkBuf, n);
    }
}

#if ENABLE_DEBUG_PACKET
static void sendDebugPacket(const imu::Vec3f& accel, const imu::Vec3f& gyro,
                            const imu::Vec3f& mag,
                            float headingDeg, float pitchDeg, float rollDeg) {
    DebugPacket dpkt{};
    dpkt.mag_x   = mag.x;
    dpkt.mag_y   = mag.y;
    dpkt.mag_z   = mag.z;
    dpkt.accel_x = accel.x;
    dpkt.accel_y = accel.y;
    dpkt.accel_z = accel.z;
    dpkt.gyro_x  = gyro.x;
    dpkt.gyro_y  = gyro.y;
    dpkt.gyro_z  = gyro.z;
    dpkt.fused_heading_deg = headingDeg;
    // Raw mag heading: atan2(Y, X), no tilt compensation
    float rawRad = atan2f(mag.y, mag.x);
    float rawDeg = rawRad * (180.0f / M_PI);
    if (rawDeg < 0.0f) rawDeg += 360.0f;
    dpkt.raw_mag_heading_deg = rawDeg;
    dpkt.pitch_deg = pitchDeg;
    dpkt.roll_deg  = rollDeg;

    size_t n = debugPacketToBytes(dpkt, linkBuf, sizeof(linkBuf));
    if (n > 0) {
        Serial1.write(linkBuf, n);
    }
}
#endif

static void handleDisplayCmd() {
    static char cmdBuf[64];
    static size_t cmdPos = 0;

    while (Serial1.available()) {
        char c = Serial1.read();
        if (c == '\n' || cmdPos >= sizeof(cmdBuf) - 1) {
            cmdBuf[cmdPos] = '\0';
            DisplayCmd cmd;
            if (bytesToDisplayCmd(cmdBuf, cmdPos, cmd)) {
                switch (cmd) {
                    case DisplayCmd::SET_HOME:
                        Serial.println("CMD: SET_HOME");
                        nav::setHome();
                        sysState = SystemState::NAVIGATING;
                        break;
                    case DisplayCmd::CLEAR_HOME:
                        Serial.println("CMD: CLEAR_HOME");
                        nav::clearHome();
                        sysState = SystemState::READY;
                        break;
                    case DisplayCmd::START_MAG_CAL:
                        Serial.println("CMD: START_MAG_CAL");
                        sysState = SystemState::CALIBRATION;
                        imu::calibrateMagnetometer(magCal, 10000);
                        storage::saveMagCalibration("/mag_cal.json", magCal);
                        sysState = SystemState::READY;
                        break;
                    case DisplayCmd::START_GYRO_CAL:
                        Serial.println("CMD: START_GYRO_CAL");
                        sysState = SystemState::CALIBRATION;
                        imu::calibrateGyroscope(gyroCal, 10000);
                        storage::saveCalib3("/gyro_cal.json", gyroCal);
                        sysState = SystemState::READY;
                        break;
                    case DisplayCmd::RESET:
                        Serial.println("CMD: RESET");
                        ESP.restart();
                        break;
                    case DisplayCmd::NAV_OUTBOUND: {
                        Serial.println("CMD: NAV_OUTBOUND");
                        // Load waypoint from /config/waypoint.json
                        File wpf = LittleFS.open("/config/waypoint.json", "r");
                        if (wpf) {
                            String wj = wpf.readString();
                            wpf.close();
                            int latIdx = wj.indexOf("\"lat\"");
                            int lonIdx = wj.indexOf("\"lon\"");
                            if (latIdx >= 0 && lonIdx >= 0) {
                                float wlat = wj.substring(wj.indexOf(':', latIdx) + 1).toFloat();
                                float wlon = wj.substring(wj.indexOf(':', lonIdx) + 1).toFloat();
                                nav::setTargetLatLon(wlat, wlon);
                                sysState = SystemState::NAVIGATING;
                                Serial.printf("  Outbound waypoint: %.6f, %.6f\n", wlat, wlon);
                            } else {
                                Serial.println("  ERROR: waypoint.json missing lat/lon");
                            }
                        } else {
                            Serial.println("  ERROR: /config/waypoint.json not found");
                        }
                        break;
                    }
                    case DisplayCmd::NAV_HOME:
                        Serial.println("CMD: NAV_HOME");
                        nav::setHome();
                        sysState = SystemState::NAVIGATING;
                        break;
                    case DisplayCmd::MARK_POSITION: {
                        nav::Position pos = nav::getPosition();
                        Serial.print("CMD: MARK_POSITION x=");
                        Serial.print(pos.x_m, 1);
                        Serial.print(" y=");
                        Serial.println(pos.y_m, 1);
                        // TODO: write to log file
                        break;
                    }
                    case DisplayCmd::START_FULL_CAL:
                        Serial.println("CMD: START_FULL_CAL (120s)");
                        sysState = SystemState::CALIBRATION;
                        mag_cal::startCollection(120000);
                        break;
                    case DisplayCmd::START_SPEED_CAL:
                        Serial.println("CMD: START_SPEED_CAL (stub)");
                        // TODO: implement flow meter speed calibration
                        break;
                    case DisplayCmd::TOGGLE_GPS_POS:
                        gGpsPosEnabled = !gGpsPosEnabled;
                        nav::setUseGps(gGpsPosEnabled);
                        Serial.print("CMD: TOGGLE_GPS_POS -> ");
                        Serial.println(gGpsPosEnabled ? "ON" : "OFF");
                        break;
                    case DisplayCmd::TOGGLE_GPS_SPD:
                        gGpsSpdEnabled = !gGpsSpdEnabled;
                        Serial.print("CMD: TOGGLE_GPS_SPD -> ");
                        Serial.println(gGpsSpdEnabled ? "ON" : "OFF");
                        // TODO: gate GPS speed selection on gGpsSpdEnabled
                        break;
                    case DisplayCmd::TOGGLE_WIFI:
                        gWifiEnabled = !gWifiEnabled;
                        Serial.print("CMD: TOGGLE_WIFI -> ");
                        Serial.println(gWifiEnabled ? "ON" : "OFF");
                        // TODO: call wifi::stop() / wifi::init() based on state
                        break;
                    case DisplayCmd::CYCLE_LOG_LEVEL:
                        logging::cycleLevel();
                        break;
                    case DisplayCmd::TOGGLE_OP_MODE:
                    {
                        static bool diveMode = false;
                        diveMode = !diveMode;
                        if (diveMode) {
                            gps::setEnabled(false);
                            wifi::stop();
                            gWifiEnabled = false;
                            Serial.println("CMD: OP_MODE -> DIVE (GPS+WiFi off)");
                        } else {
                            gps::setEnabled(true);
                            wifi::init();
                            web::init();
                            gWifiEnabled = true;
                            Serial.println("CMD: OP_MODE -> SURFACE (GPS+WiFi on)");
                        }
                    }
                        break;
                    default:
                        break;
                }
            }
            cmdPos = 0;
        } else {
            cmdBuf[cmdPos++] = c;
        }
    }
}
