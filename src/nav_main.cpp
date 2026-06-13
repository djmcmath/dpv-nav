// nav_main.cpp — Nav device entry point
// Sensors, AHRS, GPS, flow sensor, dead reckoning.
// Sends NavPacket to display device over Serial1 at ~10 Hz.

#include <Arduino.h>
#include <Wire.h>
#include <LittleFS.h>
#include <time.h>
#include <esp_sleep.h>

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
#include "util/nvs_state.h"
#include "util/speed_cal.h"
#include "util/hdg_cal.h"
#include "util/waypoints.h"
#include <dpvlink.h>

// ---- AHRS state -----------------------------------------------------------
static MahonyState ahrs;
//kp: 0.5 to 2.0 is a reasonable range.  Higher: mag/accel corrections dominate faster, with less gyro drift.  Lower: gyro dominates longer, with more drift but smoother response.  
//ki: 0.0 to 0.1 -- builds up long term gyro bias estimate, correcting for drift.  Too high -- windup and oscillation.  
static MahonyParams mahonyParams{ .kp = 1.0f, .ki = 0.002f, .useMag = true };

// ---- Calibration data -----------------------------------------------------
static MagCalib         magCal;
static Calib3           gyroCal;
static Calib3           accelCal;
static hdg_cal::HdgCal  gHdgCal;
static bool             gHdgCalValid = false;

// Fourier heading cal sample collection (filled during CAPTURE_HDG_POINT commands)
static constexpr int HDG_SAMPLE_MAX = 24;
static float gHdgSamplesTarget[HDG_SAMPLE_MAX];
static float gHdgSamplesIndicated[HDG_SAMPLE_MAX];
static int   gHdgSampleCount = 0;
static float gCurrentHeadingRawDeg = 0.0f;  // updated each loop; read by CAPTURE handler

// ---- Nav state -------------------------------------------------------------
static SystemState sysState = SystemState::BOOT;
static uint32_t lastLoopUs    = 0;
static uint32_t lastSendMs    = 0;
static uint32_t lastDiagMs    = 0;
static uint32_t lastPosSaveMs = 0;
static uint32_t lastFlowLogMs = 0;
static uint32_t lastWpSendMs  = 0;
static uint32_t lastWpSaveMs  = 0;
static uint16_t gBattMv       = 0;  // EMA-smoothed battery voltage (mV), 0 = not yet read
static constexpr uint32_t SEND_INTERVAL_MS     = 100;   // 10 Hz link rate
static constexpr uint32_t LOOP_INTERVAL_US     = 10000; // 100 Hz loop rate gate
static constexpr uint32_t DIAG_INTERVAL_MS     = 1000;  // 1 Hz sensor diagnostics
static constexpr uint32_t FLOW_LOG_INTERVAL_MS = 250;   // 4 Hz flow debug logging
static constexpr bool FULL_DIAG_ENABLE    = false;  // set to false to disable periodic diagnostic prints
static constexpr bool GPS_DIAG_ENABLE     = false;   // GPS position/speed/COG coherence diagnostics
static constexpr bool GPS_RAW_NMEA_ENABLE = false;  // print every raw NMEA sentence (noisy — use to confirm PGTOP)
static constexpr bool FLOW_LOG_ENABLE     = false;  // set to false to disable flow debug logging

// ---- Calibration mode tracking -----------------------------------------------
// cal_mode in NavPacket:
//   0 = (legacy) quick mag cal hard-iron
//   1 = (legacy) full mag cal soft-iron collection
//   2 = speed cal — waiting for flow
//   3 = speed cal — run in progress
//   4 = speed cal — result ready, awaiting display accept/reject
//   5 = baseline mag cal (bin-aware)
//   6 = mounted mag cal (bin-aware)
static bool    gInCal    = false;
static uint8_t gCalMode  = 0;

// Bin-aware cal: interval tracking for CalProgressPacket (2 Hz)
static constexpr uint32_t CAL_PROGRESS_INTERVAL_MS = 500;
static uint32_t gLastCalProgressMs = 0;

// CSV output file path for active bin cal (set when cal starts)
static const char* gBinCalCsvPath = nullptr;

// ---- Speed calibration state ------------------------------------------------
static uint16_t gSpeedCalDist_ft     = 300;    // target distance selected by user
static uint32_t gSpeedCalStartMs     = 0;      // millis() when run started
static uint32_t gSpeedCalPulseTotal  = 0;      // pulses accumulated during run
static float    gSpeedCalHdgSinEMA   = 0.0f;
static float    gSpeedCalHdgCosEMA   = 0.0f;
static bool     gSpeedCalHdgPrimed   = false;
// Result values — populated when run ends, sent until display acknowledges
static float    gSpeedCalKExisting   = 0.0f;
static float    gSpeedCalKProposed   = 0.0f;
static uint16_t gSpeedCalElapsedS    = 0;

// ---- Boot status flags (set during setup, sent in every NavPacket) ---------
static uint8_t gBootFlags = 0;

// ---- Toggle states (shared between command handler and sendNavPacket) ------
static bool gGpsPosEnabled = DEFAULT_USE_GPS_POSITION;
static bool gGpsSpdEnabled = true;   // GPS speed source enabled (stub — always true for now)
static bool gWifiEnabled   = true;   // WiFi radio enabled (surface mode default)
static bool gDiveMode      = false;  // Dive mode active (persisted to NVS)

// ---- Serial link buffer ----------------------------------------------------
static char linkBuf[256];
static char wpBuf[3200];  // waypoint list packet — up to 50 waypoints in JSON

// ---- NVS helpers -----------------------------------------------------------
// Build full nav NVS state from current globals (avoids stale-read-then-write).
static nvs_nav::State currentNavNvsState() {
    nav::Position pos = nav::getPosition();
    nvs_nav::State s;
    s.gps_pos   = gGpsPosEnabled;
    s.gps_spd   = gGpsSpdEnabled;
    s.wifi      = gWifiEnabled;
    s.dive_mode = gDiveMode;
    s.log_level = static_cast<uint8_t>(logging::getLevel());
    s.pos_x     = pos.x_m;
    s.pos_y     = pos.y_m;
    return s;
}

// ---- Forward declarations --------------------------------------------------
static void loadCalibration();
static void sendNavPacket(float heading, float headingRaw, float pitch, float roll,
                          float speed, bool gpsSpeed,
                          float distHome, float bearHome,
                          float posX, float posY,
                          const GpsFix& fix);
static void sendWaypointListPacket();
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
        gBootFlags |= BOOT_IMU_OK;

        // Load or run calibration (sets BOOT_*_CAL_OK flags)
        loadCalibration();

        // GPS
        if (gps::init()) {
            Serial.println("GPS init OK");
            gBootFlags |= BOOT_GPS_OK;
        } else {
            Serial.println("WARNING: GPS init failed");
        }
        gps::setRawNmeaDebug(GPS_RAW_NMEA_ENABLE);

        // Flow sensor
        {
            speed_cal::History hist = speed_cal::load();
            float k = speed_cal::averageK(hist, FLOW_K_FACTOR);
            flow::FlowConfig flowCfg{
                .k_factor         = k,
                .cross_section_m2 = FLOW_CROSS_SECTION_M2,
                .avg_period_s     = FLOW_AVG_PERIOD_S
            };
            flow::init(flowCfg);
            if (hist.count > 0) {
                Serial.printf("Flow sensor init OK (speed_cal: k=%.4f, %u runs)\n", k, hist.count);
            } else {
                Serial.printf("Flow sensor init OK (speed_cal: no history, using default k=%.4f)\n", k);
            }
        }

        // Position model
        nav::init(DEFAULT_BASELINE_LAT, DEFAULT_BASELINE_LON);
        nav::setUseGps(DEFAULT_USE_GPS_POSITION);

        // Waypoints (must be after LittleFS.begin())
        waypoints::load();

        // AHRS
        mahonyInit(ahrs);

        // Data logging (starts in OFF state)
        logging::init();

        sysState = SystemState::READY;
    }

    // Set local timezone (Seattle / Pacific Time).
    // Must be done before wifi::init() so the NTP callback picks it up,
    // and before any GPS-based clock sync in the loop.
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();

    // WiFi + web server — always start, even if IMU failed.
    // Placed after calibration so blocking cal doesn't starve the connection.
    wifi::init();
    web::init();

    // Restore state from NVS (previous session)
    {
        nvs_nav::State nvsState = nvs_nav::load();
        gGpsPosEnabled = nvsState.gps_pos;
        gGpsSpdEnabled = nvsState.gps_spd;
        gDiveMode      = nvsState.dive_mode;
        nav::setUseGps(gGpsPosEnabled);
        nav::setPosition(nvsState.pos_x, nvsState.pos_y);
        if (nvsState.log_level > 0) {
            logging::setLevel(static_cast<logging::LogLevel>(nvsState.log_level));
        }
        if (gDiveMode) {
            gps::setEnabled(false);
            wifi::stop();
            gWifiEnabled = false;
        } else {
            gWifiEnabled = nvsState.wifi;
            if (!gWifiEnabled) wifi::stop();
        }
        Serial.printf("[NVS] Restored: gps_pos=%d gps_spd=%d wifi=%d dive=%d log=%d pos=(%.1f,%.1f)\n",
                      gGpsPosEnabled, gGpsSpdEnabled, gWifiEnabled, gDiveMode,
                      nvsState.log_level, nvsState.pos_x, nvsState.pos_y);
    }

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
    // Logical-frame raw counts (axis map applied) for bin cal CSV export.
    // The calibration JSON must be in the same frame that readMag() reads in,
    // which is post-axis-map (logical frame).  readMagRaw_SensorFrame() is WRONG
    // here — it would store sensor-frame counts and cause the Python-derived bias
    // to have the wrong sign for axes that the map negates (Y in this board's map).
    imu::Vec3i16 magRaw_logical{};
    imu::readMagRaw(magRaw_logical);

    // --- AHRS update --------------------------------------------------------
    // The mag axis map {+1,-2,+3} puts mag Y in left-handed frame (positive=Left)
    // while accel is in right-handed NED (positive Y=Right). Mahony needs both
    // in the same NED frame, so negate mag Y before passing to the filter.
    imu::Vec3f magNED = { mag.x, -mag.y, mag.z };
    mahonyUpdate(ahrs, mahonyParams, gyro, accel, magNED, dt);

    // --- Extract Euler angles -----------------------------------------------
    Euler euler = quatToEulerRad(ahrs.q);
    float headingRawDeg = headingDegFromYawRad(euler.yaw, DEFAULT_DECLINATION_DEG);
    // Fourier cal is collected using a physical magnetic compass as reference,
    // so apply/store the correction in the magnetic domain, then restore declination.
    float headingMagDeg = headingRawDeg - DEFAULT_DECLINATION_DEG;
    if (headingMagDeg <    0.0f) headingMagDeg += 360.0f;
    if (headingMagDeg >= 360.0f) headingMagDeg -= 360.0f;
    gCurrentHeadingRawDeg = headingMagDeg;  // expose magnetic heading for CAPTURE_HDG_POINT
    float headingDeg;
    if (gHdgCalValid) {
        float headingMagCorr = hdg_cal::apply(headingMagDeg, gHdgCal);
        headingDeg = headingMagCorr + DEFAULT_DECLINATION_DEG;
        if (headingDeg >= 360.0f) headingDeg -= 360.0f;
        if (headingDeg <    0.0f) headingDeg += 360.0f;
    } else {
        headingDeg = headingRawDeg;
    }
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

    // --- GPS clock sync (one-shot) -------------------------------------------
    // Sync system clock from GPS UTC when we first get a valid position fix
    // with a parsed date/time. Only runs if NTP hasn't already set the clock
    // (i.e., time(nullptr) < Nov 2023, which an unsynced ESP32 never exceeds).
    {
        static bool gGpsTimeSynced = false;
        if (!gGpsTimeSynced && fix.has_fix && fix.has_time) {
            if (time(nullptr) < 1700000000L) {
                // mktime() interprets struct tm as local time; temporarily
                // switch to UTC to compute the correct UTC epoch, then restore.
                struct tm t{};
                t.tm_year  = (2000 + fix.utc_year) - 1900;
                t.tm_mon   = fix.utc_month - 1;
                t.tm_mday  = fix.utc_day;
                t.tm_hour  = fix.utc_hour;
                t.tm_min   = fix.utc_minute;
                t.tm_sec   = fix.utc_second;
                t.tm_isdst = 0;
                setenv("TZ", "UTC0", 1); tzset();
                time_t utcEpoch = mktime(&t);
                setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1); tzset();
                struct timeval tv{ .tv_sec = utcEpoch, .tv_usec = 0 };
                settimeofday(&tv, nullptr);
                Serial.printf("[GPS] Clock set: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                              2000 + fix.utc_year, fix.utc_month, fix.utc_day,
                              fix.utc_hour, fix.utc_minute, fix.utc_second);
            }
            gGpsTimeSynced = true;  // attempt only once regardless of outcome
        }
    }

    uint8_t gpsBars = gps::computeSignalBars(fix);

    // Auto-update HOME waypoint when GPS quality is good (>= 3/4 bars ≈ 75%).
    // Only in memory — flushed to flash every 60 s by lastWpSaveMs timer below.
    if (gpsFresh && gpsBars >= 3) {
        waypoints::updateHome(fix.lat, fix.lon);
    }

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
    // Suppress DR integration when flow speed is below threshold (treats sensor
    // noise and near-stationary drift as zero rather than accumulating error).
    if (!useGpsSpeed && speed < DR_MIN_FLOW_SPEED_MS) speed = 0.0f;
    nav::updateDR(headingDeg, speed, dt);

    // --- Send NavPacket at link rate ----------------------------------------
    uint32_t nowMs = millis();
    if (nowMs - lastSendMs >= SEND_INTERVAL_MS) {
        lastSendMs = nowMs;
        nav::Position pos = nav::getPosition();
        sendNavPacket(headingDeg, headingMagDeg, pitchDeg, rollDeg, speed, useGpsSpeed,
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
            ld.lat           = pos.lat;
            ld.lon           = pos.lon;
            ld.pos_src       = (nav::isUsingGps() && gpsFresh) ? 'G' : 'E';
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

    // --- Periodic NVS position save ------------------------------------------
    if (nowMs - lastPosSaveMs >= NVS_POS_SAVE_INTERVAL_MS) {
        lastPosSaveMs = nowMs;
        nav::Position pos = nav::getPosition();
        nvs_nav::savePosition(pos.x_m, pos.y_m);
    }

    // --- Waypoint list broadcast at 1 Hz ------------------------------------
    if (nowMs - lastWpSendMs >= 1000) {
        lastWpSendMs = nowMs;
        sendWaypointListPacket();
    }

    // --- Periodic waypoint save (HOME updates from GPS) ---------------------
    if (nowMs - lastWpSaveMs >= 60000) {
        lastWpSaveMs = nowMs;
        waypoints::save();
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
            Serial.printf("[GPS ] fix=%d  3d=%d  sat=%d  hdop=%.1f  bars=%d/4\n",
                          fix.has_fix, fix.fix_type_3d, fix.satellites,
                          (double)fix.hdop, gpsBars);
            Serial.printf("[GPS ] pos=(%.6f, %.6f)  alt=%.1f m\n",
                          (double)fix.lat, (double)fix.lon, (double)fix.altitude_m);
            Serial.printf("[GPS ] SOG=%.2f kn  COG=%.1f deg  coherence=%.3f [%s]  speed=%.2f m/s (%s)\n",
                          (double)fix.speed_knots, (double)fix.course_deg,
                          (double)cogCoherence,
                          cogCoherence >= GPS_COG_COHERENCE_THRESH ? "PASS" : "FAIL",
                          (double)speed, useGpsSpeed ? "GPS" : "FLOW");
            int gsvN = gps::getGsvLineCount();
            if (gsvN == 0) {
                Serial.println("[GSV ] (none yet)");
            } else {
                for (int i = 0; i < gsvN; i++)
                    Serial.printf("[GSV ] %s\n", gps::getGsvLine(i));
            }
        }

        mStatN = 0;  // reset for next window
    }

    // --- Flow sensor debug at 4 Hz -----------------------------------------
    if (FLOW_LOG_ENABLE && (nowMs - lastFlowLogMs >= FLOW_LOG_INTERVAL_MS)) {
        lastFlowLogMs = nowMs;
        Serial.printf("[FLOW] pulses=%u  freq=%.2f Hz  raw=%.4f lpm  speed=%.4f m/s\n",
                      flow::getLastPulseCount(),
                      flow::getFrequency_hz(),
                      flow::getFlowRate_lpm(),
                      flow::getSpeed_ms());
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
    if (web::isReloadCalRequested()) {
        web::clearReloadCalRequest();
        loadCalibration();
    }

    // --- Calibration tick and completion detection ---------------------------
    if (gInCal) {
        if (gCalMode == 5 || gCalMode == 6) {
            // Bin-aware mag cal (baseline=5, mounted=6)
            // Feed current raw mag sample into the bin collector
            imu::magBinCalTick(pitchDeg, headingDeg, magRaw_logical);

            // Emit CalProgressPacket at 2 Hz
            if (nowMs - gLastCalProgressMs >= CAL_PROGRESS_INTERVAL_MS) {
                gLastCalProgressMs = nowMs;
                CalProgressPacket cpkt{};
                imu::magBinCalGetProgress(cpkt);
                char calBuf[512];
                size_t cn = calProgressPacketToBytes(cpkt, calBuf, sizeof(calBuf));
                if (cn > 0) Serial1.write(calBuf, cn);
            }

            // Check completion
            if (imu::magBinCalIsComplete()) {
                // Send final CalProgressPacket with complete=true immediately —
                // don't wait for the 2 Hz timer, or the display may never see it.
                {
                    CalProgressPacket cpkt{};
                    imu::magBinCalGetProgress(cpkt);  // complete=true, all bins green
                    char calBuf[512];
                    size_t cn = calProgressPacketToBytes(cpkt, calBuf, sizeof(calBuf));
                    if (cn > 0) Serial1.write(calBuf, cn);
                    Serial.println("[BIN_CAL] Sent completion packet");
                }

                Serial.println("[BIN_CAL] All bins green — dumping CSV");
                if (gBinCalCsvPath) {
                    File f = LittleFS.open(gBinCalCsvPath, FILE_WRITE);
                    if (f) {
                        imu::magBinCalDumpCSV(&f);
                        f.close();
                        Serial.printf("[BIN_CAL] CSV saved to %s\n", gBinCalCsvPath);
                    } else {
                        Serial.printf("[BIN_CAL] ERROR: could not open %s\n", gBinCalCsvPath);
                    }
                }
                imu::magBinCalEnd();
                gInCal   = false;
                sysState = SystemState::READY;
                Serial.println("[BIN_CAL] Cal complete, returning to READY");
            }
        } else if (gCalMode == 0) {
            // Quick cal (non-blocking hard-iron sweep, legacy)
            if (imu::magCalNBTick()) {
                imu::magCalNBGetResult(magCal);
                storage::saveMagCalibration(storage::MAG_LEGACY_FILE, magCal);
                gInCal   = false;
                sysState = SystemState::READY;
                Serial.println("[CAL] Quick cal saved, returning to READY");
            }
        } else if (gCalMode == 1) {
            // Full cal (soft-iron data collection via mag_cal::, legacy)
            if (mag_cal::isCollecting()) {
                mag_cal::logSample();
            } else {
                gInCal   = false;
                sysState = SystemState::READY;
                Serial.println("[CAL] Full cal collection done, returning to READY");
            }
        } else if (gCalMode == 2) {
            // Speed cal — WAITING: watch for flow to exceed start threshold
            float flowFreq = flow::getFrequency_hz();
            if (flowFreq >= SPEED_CAL_START_THRESHOLD_HZ) {
                // Flow detected — start the run
                gCalMode             = 3;
                gSpeedCalStartMs     = millis();
                gSpeedCalPulseTotal  = 0;
                gSpeedCalHdgPrimed   = false;
                Serial.printf("[SPEED_CAL] Run started, target %uft\n",
                              (unsigned)gSpeedCalDist_ft);
            }
        } else if (gCalMode == 3) {
            // Speed cal — RUNNING: accumulate pulses, watch stop conditions
            gSpeedCalPulseTotal += flow::getLastPulseCount();
            uint32_t elapsedMs  = millis() - gSpeedCalStartMs;
            float    elapsedS   = elapsedMs / 1000.0f;

            // Update heading EMA (circular, handles 0°/360° wrap)
            float hdgRad = headingDeg * (float)(M_PI / 180.0);
            if (!gSpeedCalHdgPrimed) {
                gSpeedCalHdgSinEMA = sinf(hdgRad);
                gSpeedCalHdgCosEMA = cosf(hdgRad);
                gSpeedCalHdgPrimed = true;
            } else {
                gSpeedCalHdgSinEMA += SPEED_CAL_HDG_EMA_ALPHA *
                                      (sinf(hdgRad) - gSpeedCalHdgSinEMA);
                gSpeedCalHdgCosEMA += SPEED_CAL_HDG_EMA_ALPHA *
                                      (cosf(hdgRad) - gSpeedCalHdgCosEMA);
            }

            // --- Stop conditions ---
            bool runDone = false;

            // 1) Flow drops: DPV has stopped
            if (flow::getSpeed_ms() < SPEED_CAL_STOP_THRESHOLD_MS) {
                runDone = true;
                Serial.println("[SPEED_CAL] Stop: flow dropped");
            }

            // 2) After minimum run time, large heading deviation signals end
            if (!runDone && elapsedS >= SPEED_CAL_MIN_RUN_S) {
                float emaHdgRad = atan2f(gSpeedCalHdgSinEMA, gSpeedCalHdgCosEMA);
                float diffRad   = hdgRad - emaHdgRad;
                // Normalise to [-π, π]
                while (diffRad >  (float)M_PI) diffRad -= 2.0f * (float)M_PI;
                while (diffRad < -(float)M_PI) diffRad += 2.0f * (float)M_PI;
                if (fabsf(diffRad) * (180.0f / (float)M_PI) >= SPEED_CAL_HEADING_STOP_DEG) {
                    runDone = true;
                    Serial.printf("[SPEED_CAL] Stop: heading change %.1f° after %.1fs\n",
                                  fabsf(diffRad) * (180.0f / (float)M_PI), elapsedS);
                }
            }

            if (runDone && elapsedS >= 5.0f) {
                // Compute proposed k-factor
                // Formula: k_new = total_pulses / (dist_m * 60 * 1000 * cross_section_m2)
                // This is derived from:
                //   speed_ms = (freq_hz / k) / 60 / 1000 / cross_section_m2
                //   where freq_hz = total_pulses / elapsed_s, speed_ms = dist_m / elapsed_s
                //   elapsed_s cancels → k = total_pulses / (dist_m * 60 * 1000 * cross_section)
                float distM  = gSpeedCalDist_ft * 0.3048f;
                float k_new  = (float)gSpeedCalPulseTotal /
                               (distM * 60.0f * 1000.0f * FLOW_CROSS_SECTION_M2);

                // Load current history for the existing average
                speed_cal::History hist = speed_cal::load();
                float k_existing = speed_cal::averageK(hist, FLOW_K_FACTOR);

                gSpeedCalKExisting = k_existing;
                gSpeedCalKProposed = k_new;
                gSpeedCalElapsedS  = (elapsedS > 65535.0f) ? 65535 :
                                     (uint16_t)elapsedS;

                gCalMode = 4;  // transition to RESULT state
                Serial.printf("[SPEED_CAL] Result: %upulses dist=%.1fm t=%.1fs "
                              "k_exist=%.4f k_new=%.4f\n",
                              (unsigned)gSpeedCalPulseTotal, distM, elapsedS,
                              k_existing, k_new);
            }
            // If run ended too quickly (< 5 s), abort back to WAITING
            else if (runDone) {
                gCalMode = 2;
                Serial.println("[SPEED_CAL] Run too short, returning to WAITING");
            }
        }
        // cal_mode == 4 (RESULT): no tick needed; nav device just keeps broadcasting
        // result fields in NavPacket until display sends accept/reject command.
    }

    // --- Serial commands and mag cal collection (legacy serial-triggered) ----
    serial_cmd::processInput(ahrs);
    serial_cmd::updateStabilityTest(mag, accel);
    // Note: mag_cal::logSample() is now called in the gInCal block above for menu-triggered cals.
    // The serial_cmd path (e.g., 'collect_mag') still calls mag_cal::startCollection() directly;
    // handle that here to avoid missing samples if triggered via serial.
    if (!gInCal && mag_cal::isCollecting()) mag_cal::logSample();

    // --- Check for commands from display ------------------------------------
    handleDisplayCmd();
}

// ===========================================================================
// Helpers
// ===========================================================================

static void loadCalibration() {
    // Magnetometer — try two-stage chain (mag_base.json + mag_mount.json),
    // falling back to legacy mag_cal.json, then blocking sweep as last resort.
    bool hasBase = false, hasMount = false;
    if (storage::loadMagCalibrationChain(magCal, hasBase, hasMount)) {
        imu::setMagCalibration(magCal);
        gBootFlags |= BOOT_MAG_CAL_OK;
        Serial.printf("Mag cal loaded: base=%d mount=%d\n", hasBase, hasMount);
    } else {
        //TODO: this legacy fallback is no longer ideal, since it blocks for 90 seconds and doesn't provide any progress feedback. Better to implement a non-blocking sweep with progress reporting, like the mag_bin_cal does.
        //TODO: make this better.  Fall back to a baseline mediocre calibration, set "calibration quality" to "poor", let the user cal when they can.
        Serial.println("No mag calibration found — running min/max sweep (90 s)");
        imu::calibrateMagnetometer(magCal, 90000);
        storage::saveMagCalibration(storage::MAG_LEGACY_FILE, magCal);
    }

    // Gyroscope
    if (storage::loadCalib3("/gyro_cal.json", gyroCal)) {
        imu::setGyroCalibration(gyroCal);
        gBootFlags |= BOOT_GYRO_CAL_OK;
        Serial.println("Gyro calibration loaded from LittleFS");
    } else {
        delay(2500);  //give the user a second to realize what's happened
        Serial.println("No gyro calibration found — sampling at rest (10 s)");
        delay(2500);  //give the user a second to realize what's happened
        imu::calibrateGyroscope(gyroCal, 10000);
        storage::saveCalib3("/gyro_cal.json", gyroCal);
    }

    // Accelerometer
    if (storage::loadCalib3("/accel_cal.json", accelCal)) {
        imu::setAccelCalibration(accelCal);
        gBootFlags |= BOOT_ACCEL_CAL_OK;
        Serial.println("Accel calibration loaded from LittleFS");
    } else {
        delay(2500);  //give the user a second to realize what's happened
        Serial.println("No accel calibration found — 6-point cal");
        delay(2500);  //give the user a second to realize what's happened
        imu::calibrateAccelerometer(accelCal, 2500);
        storage::saveCalib3("/accel_cal.json", accelCal);
    }

    // Fourier heading calibration (optional — silently skip if absent)
    if (hdg_cal::load(gHdgCal)) {
        gHdgCalValid = true;
        gBootFlags |= BOOT_HDG_CAL_OK;
        Serial.printf("Fourier heading cal loaded (%d harmonic(s))\n", gHdgCal.n);
    }
}

// Reload calibration JSON files without blocking fallbacks.
// Safe to call at runtime (e.g., after uploading a new cal file via web UI).
static void reloadCalibrationFiles() {
    gBootFlags &= ~(BOOT_MAG_CAL_OK | BOOT_GYRO_CAL_OK | BOOT_ACCEL_CAL_OK | BOOT_HDG_CAL_OK);
    gHdgCalValid = false;

    bool hasBase = false, hasMount = false;
    if (storage::loadMagCalibrationChain(magCal, hasBase, hasMount)) {
        imu::setMagCalibration(magCal);
        gBootFlags |= BOOT_MAG_CAL_OK;
        Serial.printf("[Reload] Mag cal: base=%d mount=%d\n", hasBase, hasMount);
    } else {
        Serial.println("[Reload] No mag calibration found");
    }

    if (storage::loadCalib3("/gyro_cal.json", gyroCal)) {
        imu::setGyroCalibration(gyroCal);
        gBootFlags |= BOOT_GYRO_CAL_OK;
        Serial.println("[Reload] Gyro cal loaded");
    } else {
        Serial.println("[Reload] No gyro calibration found");
    }

    if (storage::loadCalib3("/accel_cal.json", accelCal)) {
        imu::setAccelCalibration(accelCal);
        gBootFlags |= BOOT_ACCEL_CAL_OK;
        Serial.println("[Reload] Accel cal loaded");
    } else {
        Serial.println("[Reload] No accel calibration found");
    }

    if (hdg_cal::load(gHdgCal)) {
        gHdgCalValid = true;
        gBootFlags |= BOOT_HDG_CAL_OK;
        Serial.printf("[Reload] Fourier heading cal loaded (%d harmonic(s))\n", gHdgCal.n);
    } else {
        Serial.println("[Reload] No Fourier heading cal found");
    }
}

// Average BATT_ADC_AVG_N ADC samples and fold in via EMA.
// GPIO35 reads VBAT through a 2:1 divider, so adc_mV * 2 = battery_mV.
// Call once per sendNavPacket (10 Hz); EMA alpha gives a ~5 s smoothing window.
static void updateBattMv() {
    uint32_t sum = 0;
    for (int i = 0; i < BATT_ADC_AVG_N; i++) sum += analogRead(BATT_ADC_PIN);
    uint32_t raw_adc = sum / BATT_ADC_AVG_N;
    uint16_t sample_mv = (uint16_t)((raw_adc * 3300UL * 2) / 4095);
    if (gBattMv == 0) {
        gBattMv = sample_mv;  // seed EMA on first read
    } else {
        // EMA alpha ≈ 0.02 → ~50-sample (~5 s) smoothing window
        gBattMv = (uint16_t)(gBattMv + (int32_t)(sample_mv - gBattMv) * 2 / 100);
    }
}

static void sendNavPacket(float heading, float headingRaw, float pitch, float roll,
                          float speed, bool gpsSpeed,
                          float distHome, float bearHome,
                          float posX, float posY,
                          const GpsFix& fix) {
    NavPacket pkt{};
    pkt.heading_deg      = heading;
    pkt.heading_raw_deg  = headingRaw;
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
    pkt.gps_signal_bars  = gps::computeSignalBars(fix);
    pkt.uptime_ms        = millis();

    uint8_t flags = 0;
    if (gpsSpeed) flags |= FLAG_GPS_SPEED;
    if (nav::hasHome()) flags |= FLAG_HAS_HOME;
    flags |= FLAG_TRUE_HEADING;  // declination applied in heading calculation
    if (gGpsPosEnabled) flags |= FLAG_GPS_POS_ENABLED;
    if (gWifiEnabled)   flags |= FLAG_WIFI_ENABLED;
    if (gGpsSpdEnabled) flags |= FLAG_GPS_SPD_ENABLED;
    flags |= (static_cast<uint8_t>(logging::getLevel()) << FLAG_LOG_LEVEL_SHIFT) & FLAG_LOG_LEVEL_MASK;
    pkt.flags      = flags;
    pkt.boot_flags = gBootFlags;

    // GPS detail fields
    pkt.gps_antenna  = fix.antenna_status;
    float hdop = fix.hdop;
    pkt.gps_hdop_x10 = (hdop <= 0.0f || !fix.has_fix) ? 0
                     : (hdop >= 25.5f) ? 255
                     : (uint8_t)(hdop * 10.0f + 0.5f);

    // WiFi mode: client (connected to stored AP) vs. own AP
    uint8_t flags2 = 0;
    if (gWifiEnabled && wifi::isStaConnected()) flags2 |= FLAG2_WIFI_CLIENT;
    pkt.flags2 = flags2;

    updateBattMv();
    pkt.batt_mv = gBattMv;

    // Pack calibration progress when in CALIBRATION state
    if (sysState == SystemState::CALIBRATION && gInCal) {
        pkt.cal_mode = gCalMode;
        if (gCalMode == 0) {
            // Quick cal: progress from non-blocking mag cal state machine
            uint32_t elapsed_ms, remaining_ms;
            int covX, covY, covZ;
            imu::magCalNBGetProgress(elapsed_ms, remaining_ms, covX, covY, covZ);
            pkt.cal_remaining_s  = (uint8_t)(remaining_ms / 1000);
            pkt.cal_coverage_pct = (uint8_t)((covX + covY + covZ) / 3);
        } else if (gCalMode == 1) {
            // Full cal: progress from mag_cal collection
            uint32_t remaining_ms = mag_cal::getRemainingMs();
            pkt.cal_remaining_s   = (uint8_t)(remaining_ms / 1000);
            pkt.cal_coverage_pct  = mag_cal::getSpatialCoverage();
        } else {
            // Speed cal (cal_mode 2/3/4): pack run data
            pkt.speed_cal_dist_ft    = gSpeedCalDist_ft;
            pkt.speed_cal_k_existing = gSpeedCalKExisting;
            pkt.speed_cal_k_proposed = gSpeedCalKProposed;
            if (gCalMode == 3) {
                // Running: send live elapsed time
                uint32_t elapsedMs = millis() - gSpeedCalStartMs;
                pkt.speed_cal_elapsed_s = (elapsedMs / 1000 > 65535) ?
                                          65535 : (uint16_t)(elapsedMs / 1000);
            } else {
                // Waiting (2) or result (4): send recorded elapsed
                pkt.speed_cal_elapsed_s = gSpeedCalElapsedS;
            }
        }
    }

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

static void sendWaypointListPacket() {
    WaypointListPacket pkt{};
    int n = waypoints::count();
    int toSend = (n > WP_PACKET_MAX) ? WP_PACKET_MAX : n;
    pkt.count       = (uint8_t)toSend;
    pkt.total_count = (uint8_t)(n > 255 ? 255 : n);
    for (int i = 0; i < toSend; i++) {
        const waypoints::Waypoint* wp = waypoints::get(i);
        if (!wp) break;
        pkt.waypoints[i].idx = (uint8_t)i;
        strncpy(pkt.waypoints[i].name, wp->name, 12);
        pkt.waypoints[i].name[12] = '\0';
        pkt.waypoints[i].lat = wp->lat;
        pkt.waypoints[i].lon = wp->lon;
    }
    size_t nb = waypointListPacketToBytes(pkt, wpBuf, sizeof(wpBuf));
    if (nb > 0) Serial1.write(wpBuf, nb);
}

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
                        Serial.println("CMD: START_MAG_CAL (30s quick hard-iron)");
                        sysState = SystemState::CALIBRATION;
                        gInCal   = true;
                        gCalMode = 0;
                        imu::magCalNBBegin(30000);
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
                        Serial.println("CMD: START_FULL_CAL (legacy 120s soft-iron data collection)");
                        sysState = SystemState::CALIBRATION;
                        gInCal   = true;
                        gCalMode = 1;
                        mag_cal::startCollection(180000);
                        break;
                    case DisplayCmd::START_BASELINE_CAL:
                        Serial.println("CMD: START_BASELINE_CAL (bin-aware, off-scooter)");
                        sysState = SystemState::CALIBRATION;
                        gInCal   = true;
                        gCalMode = 5;
                        gBinCalCsvPath = "/mag_baseline_samples.csv";
                        gLastCalProgressMs = 0;
                        imu::magBinCalBegin(false);
                        break;
                    case DisplayCmd::START_MOUNTED_CAL:
                        Serial.println("CMD: START_MOUNTED_CAL (bin-aware, on-scooter)");
                        sysState = SystemState::CALIBRATION;
                        gInCal   = true;
                        gCalMode = 6;
                        gBinCalCsvPath = "/mag_mounted_samples.csv";
                        gLastCalProgressMs = 0;
                        imu::magBinCalBegin(true);
                        break;
                    case DisplayCmd::START_SPEED_CAL: {
                        gSpeedCalDist_ft = parseSpeedCalDist(cmdBuf, cmdPos);
                        Serial.printf("CMD: START_SPEED_CAL dist=%uft\n",
                                      (unsigned)gSpeedCalDist_ft);
                        // Load current k-factor average for reporting
                        {
                            speed_cal::History hist = speed_cal::load();
                            gSpeedCalKExisting = speed_cal::averageK(hist, FLOW_K_FACTOR);
                        }
                        gSpeedCalKProposed   = gSpeedCalKExisting;  // no proposal yet
                        gSpeedCalElapsedS    = 0;
                        gSpeedCalPulseTotal  = 0;
                        gSpeedCalHdgPrimed   = false;
                        sysState = SystemState::CALIBRATION;
                        gInCal   = true;
                        gCalMode = 2;  // WAITING
                        break;
                    }
                    case DisplayCmd::SPEED_CAL_ACCEPT_RESET: {
                        Serial.println("CMD: SPEED_CAL_ACCEPT_RESET");
                        speed_cal::History hist{};
                        speed_cal::addMeasurement(hist, gSpeedCalKProposed);
                        speed_cal::save(hist);
                        // Apply new k-factor immediately
                        flow::setConfig({ gSpeedCalKProposed,
                                          FLOW_CROSS_SECTION_M2,
                                          FLOW_AVG_PERIOD_S });
                        Serial.printf("[SPEED_CAL] k-factor reset to %.4f\n",
                                      gSpeedCalKProposed);
                        gInCal   = false;
                        sysState = SystemState::READY;
                        break;
                    }
                    case DisplayCmd::SPEED_CAL_ACCEPT: {
                        Serial.println("CMD: SPEED_CAL_ACCEPT");
                        speed_cal::History hist = speed_cal::load();
                        speed_cal::addMeasurement(hist, gSpeedCalKProposed);
                        speed_cal::save(hist);
                        float newK = speed_cal::averageK(hist, FLOW_K_FACTOR);
                        flow::setConfig({ newK,
                                          FLOW_CROSS_SECTION_M2,
                                          FLOW_AVG_PERIOD_S });
                        Serial.printf("[SPEED_CAL] k-factor updated to %.4f (avg of %d)\n",
                                      newK, (int)hist.count);
                        gInCal   = false;
                        sysState = SystemState::READY;
                        break;
                    }
                    case DisplayCmd::SPEED_CAL_REJECT:
                        Serial.println("CMD: SPEED_CAL_REJECT — discarding result");
                        gInCal   = false;
                        sysState = SystemState::READY;
                        break;
                    case DisplayCmd::TOGGLE_GPS_POS:
                        gGpsPosEnabled = !gGpsPosEnabled;
                        nav::setUseGps(gGpsPosEnabled);
                        Serial.print("CMD: TOGGLE_GPS_POS -> ");
                        Serial.println(gGpsPosEnabled ? "ON" : "OFF");
                        nvs_nav::save(currentNavNvsState());
                        break;
                    case DisplayCmd::TOGGLE_GPS_SPD:
                        gGpsSpdEnabled = !gGpsSpdEnabled;
                        Serial.print("CMD: TOGGLE_GPS_SPD -> ");
                        Serial.println(gGpsSpdEnabled ? "ON" : "OFF");
                        // TODO: gate GPS speed selection on gGpsSpdEnabled
                        nvs_nav::save(currentNavNvsState());
                        break;
                    case DisplayCmd::TOGGLE_WIFI:
                        gWifiEnabled = !gWifiEnabled;
                        Serial.print("CMD: TOGGLE_WIFI -> ");
                        Serial.println(gWifiEnabled ? "ON" : "OFF");
                        // TODO: call wifi::stop() / wifi::init() based on state
                        nvs_nav::save(currentNavNvsState());
                        break;
                    case DisplayCmd::CYCLE_LOG_LEVEL:
                        logging::cycleLevel();
                        nvs_nav::save(currentNavNvsState());
                        break;
                    case DisplayCmd::TOGGLE_OP_MODE:
                        gDiveMode = !gDiveMode;
                        if (gDiveMode) {
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
                        nvs_nav::save(currentNavNvsState());
                        break;
                    case DisplayCmd::START_HDG_FOURIER_CAL:
                        gHdgSampleCount = 0;
                        Serial.println("[HDG_CAL] Collection started — buffer reset");
                        break;
                    case DisplayCmd::CAPTURE_HDG_POINT: {
                        float target = parseCaptureHdgPoint(cmdBuf, cmdPos);
                        if (gHdgSampleCount < HDG_SAMPLE_MAX) {
                            gHdgSamplesTarget[gHdgSampleCount]    = target;
                            gHdgSamplesIndicated[gHdgSampleCount] = gCurrentHeadingRawDeg;
                            Serial.printf("[HDG_CAL] Point %d: target=%.0f indicated=%.1f\n",
                                          gHdgSampleCount + 1, target, gCurrentHeadingRawDeg);
                            gHdgSampleCount++;
                        } else {
                            Serial.println("[HDG_CAL] WARNING: sample buffer full, point discarded");
                        }
                        break;
                    }
                    case DisplayCmd::FINALIZE_HDG_CAL: {
                        File f = LittleFS.open(hdg_cal::SAMPLES_FILE_PATH, FILE_WRITE);
                        if (f) {
                            f.println("actual,indicated");
                            for (int i = 0; i < gHdgSampleCount; i++) {
                                f.printf("%.1f,%.1f\n",
                                         gHdgSamplesTarget[i],
                                         gHdgSamplesIndicated[i]);
                            }
                            f.close();
                            Serial.printf("[HDG_CAL] Saved %d samples to %s\n",
                                          gHdgSampleCount, hdg_cal::SAMPLES_FILE_PATH);
                        } else {
                            Serial.printf("[HDG_CAL] ERROR: could not open %s\n",
                                          hdg_cal::SAMPLES_FILE_PATH);
                        }
                        gHdgSampleCount = 0;
                        break;
                    }
                    case DisplayCmd::SELECT_WAYPOINT: {
                        uint8_t idx = parseWaypointIndex(cmdBuf, cmdPos);
                        const waypoints::Waypoint* wp = waypoints::get(idx);
                        if (wp) {
                            nav::setTargetLatLon(wp->lat, wp->lon);
                            sysState = SystemState::NAVIGATING;
                            Serial.printf("CMD: SELECT_WAYPOINT idx=%u name=%s lat=%.6f lon=%.6f\n",
                                          idx, wp->name, wp->lat, wp->lon);
                        } else {
                            Serial.printf("CMD: SELECT_WAYPOINT idx=%u — out of range (count=%d)\n",
                                          idx, waypoints::count());
                        }
                        break;
                    }
                    case DisplayCmd::ARRIVE_WAYPOINT: {
                        uint8_t idx = parseWaypointIndex(cmdBuf, cmdPos);
                        const waypoints::Waypoint* wp = waypoints::get(idx);
                        if (wp) {
                            nav::snapToLatLon(wp->lat, wp->lon);
                            Serial.printf("CMD: ARRIVE_WAYPOINT idx=%u name=%s — position snapped\n",
                                          idx, wp->name);
                            // Log the position correction as a waypoint-type entry
                            if (logging::isLogging()) {
                                nav::Position pos = nav::getPosition();
                                logging::LogData ld{};
                                ld.timestamp_ms = millis();
                                ld.heading_deg  = 0.0f;
                                ld.speed_ms     = 0.0f;
                                ld.gpsSpeed     = false;
                                ld.pos_x_m      = pos.x_m;
                                ld.pos_y_m      = pos.y_m;
                                ld.lat          = wp->lat;
                                ld.lon          = wp->lon;
                                ld.pos_src      = 'W';
                                logging::logImmediate(ld);
                            }
                        } else {
                            Serial.printf("CMD: ARRIVE_WAYPOINT idx=%u — out of range (count=%d)\n",
                                          idx, waypoints::count());
                        }
                        break;
                    }
                    case DisplayCmd::POWER_OFF: {
                        Serial.println("CMD: POWER_OFF — saving state and entering deep sleep");
                        // Persist current position and toggle states before sleeping.
                        {
                            nav::Position pos = nav::getPosition();
                            nvs_nav::savePosition(pos.x_m, pos.y_m);
                        }
                        nvs_nav::save(currentNavNvsState());
                        // Power down GPS (fix is retained by module's backup battery).
                        gps::setEnabled(false);
                        Serial.flush();
                        // Wake when the display device sends a byte over the serial link:
                        // the UART start bit pulls LINK_RX_PIN LOW, triggering ext0.
                        esp_sleep_enable_ext0_wakeup(
                            static_cast<gpio_num_t>(LINK_RX_PIN), 0 /* wake on LOW */);
                        esp_deep_sleep_start();
                        break;  // unreachable — silences compiler warning
                    }
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
