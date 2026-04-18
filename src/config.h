#pragma once

//Compile-time and runtime config defaults

// WiFi configuration
constexpr const char* WIFI_STA_SSID       = "DansMain";
constexpr const char* WIFI_STA_PASS       = "PassWord99";
constexpr const char* WIFI_AP_SSID        = "Tern";
constexpr const char* WIFI_AP_PASS        = "password";
constexpr uint32_t    WIFI_STA_TIMEOUT_MS = 10000;

// Flow sensor configuration (hall-effect pulse output)
constexpr float FLOW_K_FACTOR         = 1.0f;   // freq_hz = K * flow_lpm (pulses per L/min)
constexpr float FLOW_CROSS_SECTION_M2 = 0.002f;  // Intake cross-section area (m²) — calibrate to match DPV

// Flow speed averaging
constexpr float FLOW_AVG_PERIOD_S = 3.0f;    // EMA time constant (seconds) — smooths out momentary spikes

// Speed source selection
constexpr uint32_t GPS_FIX_STALE_MS = 3000;      // Fall back to flowmeter if GPS fix older than this
constexpr float    KNOTS_TO_MS      = 0.514444f;  // knots → m/s conversion factor

// GPS COG (Course Over Ground) coherence filter
// GPS speed is only trusted when recent COG readings are consistent.
// Coherence is the resultant length of unit-vector EMA of COG (0=random, 1=identical).
constexpr float GPS_COG_EMA_ALPHA       = 0.3f;   // EMA smoothing factor (at 1 Hz: ~3-4 sample window)
constexpr float GPS_COG_COHERENCE_THRESH = 0.85f;  // Below this, GPS speed is treated as noise
constexpr float GPS_SOG_NOISE_FLOOR_KN  = 0.1f;   // SOG below this is certainly noise (knots)
constexpr float GPS_SOG_TRUST_FLOOR_KN  = 2.0f;   // SOG above this is trusted regardless of COG (knots)

// Position estimation
constexpr float DEFAULT_BASELINE_LAT     = 47.5f;   // degrees N (assumed until GPS fix)
constexpr float DEFAULT_BASELINE_LON     = -122.5f;  // degrees W (assumed until GPS fix)
constexpr bool  DEFAULT_USE_GPS_POSITION = true;     // use GPS lat/lon as position truth when available

// Magnetic declination (degrees, positive = East, negative = West)
// Look up your local value at https://www.ngdc.noaa.gov/geomag/declination.shtml
constexpr float DEFAULT_DECLINATION_DEG  = 14.7f;   // ~14.7°E for southern Oregon (42°N, 122°W)

// Display mode (compile-time): 0 = Navigation, 1 = Debug
#ifndef DISPLAY_MODE
#define DISPLAY_MODE 0
#endif

// Display units: 0 = metric (m, m/min), 1 = imperial (ft, ft/min)
#ifndef DISPLAY_UNITS_IMPERIAL
#define DISPLAY_UNITS_IMPERIAL 0
#endif

// Enable debug packet transmission from nav device (0 = off, 1 = on)
#ifndef ENABLE_DEBUG_PACKET
#define ENABLE_DEBUG_PACKET 0
#endif

// Debug packet send interval (ms) — can be slower than nav packet
constexpr uint32_t DEBUG_SEND_INTERVAL_MS = 200;  // 5 Hz

// NVS position save interval (ms) — how often estimated position is persisted to NVS
constexpr uint32_t NVS_POS_SAVE_INTERVAL_MS = 30000;  // 30 seconds

// Data logging intervals (ms) — minimum time between log entries per level
constexpr uint32_t LOG_LOW_INTERVAL_MS  = 5000;   // 1 entry per 5 seconds (LOW level)
constexpr uint32_t LOG_HIGH_INTERVAL_MS = 1000;   // 1 entry per second (HIGH level)

// ---------------------------------------------------------------------------
// Magnetometer calibration bin parameters
// ---------------------------------------------------------------------------

// Sample threshold per bin — green = fully covered, yellow = partial, red = sparse
constexpr uint8_t MAG_CAL_BIN_GREEN_THRESHOLD  = 15;  // samples for "green" (fully covered)
constexpr uint8_t MAG_CAL_BIN_YELLOW_THRESHOLD = 5;   // samples for "yellow" (partial)

// Baseline cal: 12 heading sectors × 5 elevation bands = 60 bins
// Elevation bands (degrees pitch): <-60, -60..-30, -30..+30 (level), +30..+60, >+60
constexpr int MAG_CAL_BASELINE_HDG_SECTORS = 12;
constexpr int MAG_CAL_BASELINE_ELEV_BANDS  = 5;
constexpr int MAG_CAL_BASELINE_BINS        = MAG_CAL_BASELINE_HDG_SECTORS * MAG_CAL_BASELINE_ELEV_BANDS;  // 60

// Mounted cal: 12 heading sectors × 3 elevation bands = 36 bins
// Elevation bands (degrees pitch): -30..-15, -15..+15 (level), +15..+30 (and beyond threshold)
constexpr int MAG_CAL_MOUNTED_HDG_SECTORS  = 12;
constexpr int MAG_CAL_MOUNTED_ELEV_BANDS   = 3;
constexpr int MAG_CAL_MOUNTED_BINS         = MAG_CAL_MOUNTED_HDG_SECTORS * MAG_CAL_MOUNTED_ELEV_BANDS;  // 36

// Elevation band boundaries (degrees, applied to AHRS pitch)
// Baseline: [-90, -60, -30, +30, +60, +90]  → 5 bands
// Mounted:  any pitch outside [-30, +30] range snaps to upper/lower band (no >60° band)
constexpr float MAG_CAL_ELEV_L2 = -60.0f;  // boundary between lowest and low band
constexpr float MAG_CAL_ELEV_L1 = -30.0f;  // boundary between low and level band
constexpr float MAG_CAL_ELEV_H1 = +30.0f;  // boundary between level and high band
constexpr float MAG_CAL_ELEV_H2 = +60.0f;  // boundary between high and highest band

// Weight for out-of-range samples in mounted cal fit (pitch beyond ±30°)
// 0.0 = exclude, 1.0 = full weight
constexpr float MAG_CAL_MOUNTED_OOR_WEIGHT = 0.25f;

// Weighted completion thresholds — number of green sectors required per elevation row.
// The level row must be fully covered; tilted rows allow partial coverage because
// DPV operation is primarily horizontal and extreme orientations are hard to achieve.
// Rationale: ±60° samples constrain the magnetometer Z-axis response, which matters
// less for near-horizontal DPV operation. Collecting any samples at extreme tilt
// still improves fit quality; requiring 100% is unnecessary.
//
//   Level (0°):     12/12 = 100%  — must be fully covered for reliable horizontal heading
//   ±30° rows:      11/12 ≈ 92%  — one missed sector per tilt row is acceptable
//   ±60° rows:       9/12 = 75%  — three missed sectors at extreme tilt is acceptable
constexpr int MAG_CAL_SECTORS_LEVEL   = 12;  // 100% — level row
constexpr int MAG_CAL_SECTORS_MID     = 11;  // ~92% — ±30° rows
constexpr int MAG_CAL_SECTORS_EXTREME =  3;  //  25% — ±60° rows (baseline only)
// Note: extreme rows need only 3/12 sectors because DPV operation rarely exceeds ±45°
// pitch.  A handful of extreme-tilt samples is enough to constrain the Z-axis of the
// ellipsoid fit without requiring sustained upside-down maneuvers.

// ---------------------------------------------------------------------------
// GPS signal quality scoring
// ---------------------------------------------------------------------------
// computeSignalBars() produces a 0–4 bar count from a GpsFix.
// Each component gets a score in [0.0, 1.0], then a weighted sum maps to bars.
//
// NOTE: SNR scoring is defined here but NOT applied — the Adafruit GPS library
// marks GSV sentences as "known but not parseable", so no SNR data is available.
// Weights below are renormalized over the three active components (HDOP / fix / sats).
// Re-enable GPS_WEIGHT_SNR (and renormalize) when SNR becomes available.

// Fix type (GpsFix.fix_type_3d: 1=no fix, 2=2D, 3=3D)
constexpr float GPS_SCORE_FIX_NONE = 0.00f;
constexpr float GPS_SCORE_FIX_2D   = 0.45f;
constexpr float GPS_SCORE_FIX_3D   = 1.00f;

// HDOP (lower = better; score drops in bands)
constexpr float GPS_HDOP_THRESH_1  = 0.8f;  constexpr float GPS_HDOP_SCORE_1  = 1.00f;
constexpr float GPS_HDOP_THRESH_2  = 1.2f;  constexpr float GPS_HDOP_SCORE_2  = 0.90f;
constexpr float GPS_HDOP_THRESH_3  = 1.8f;  constexpr float GPS_HDOP_SCORE_3  = 0.70f;
constexpr float GPS_HDOP_THRESH_4  = 2.5f;  constexpr float GPS_HDOP_SCORE_4  = 0.45f;
constexpr float GPS_HDOP_THRESH_5  = 4.0f;  constexpr float GPS_HDOP_SCORE_5  = 0.20f;
constexpr float GPS_HDOP_SCORE_BAD = 0.00f; // HDOP > 4.0

// Satellites used (from GGA)
constexpr float GPS_SAT_SCORE_LE3  = 0.00f; // <= 3
constexpr float GPS_SAT_SCORE_4    = 0.25f;
constexpr float GPS_SAT_SCORE_5    = 0.45f;
constexpr float GPS_SAT_SCORE_6    = 0.65f;
constexpr float GPS_SAT_SCORE_7    = 0.80f;
constexpr float GPS_SAT_SCORE_GE8  = 1.00f; // >= 8

// SNR scores (median dBHz — not currently enforced; see NOTE above)
constexpr float GPS_SNR_THRESH_20  = 20.0f;  constexpr float GPS_SNR_SCORE_LT20 = 0.00f;
constexpr float GPS_SNR_THRESH_25  = 25.0f;  constexpr float GPS_SNR_SCORE_20   = 0.25f;
constexpr float GPS_SNR_THRESH_30  = 30.0f;  constexpr float GPS_SNR_SCORE_25   = 0.50f;
constexpr float GPS_SNR_THRESH_35  = 35.0f;  constexpr float GPS_SNR_SCORE_30   = 0.75f;
constexpr float GPS_SNR_SCORE_GE35 = 1.00f;

// Component weights (must sum to 1.0 across active components)
// Original: HDOP=45%, fix=20%, sats=15%, SNR=20% → active total = 80%
// Renormalized: 45/80, 20/80, 15/80
constexpr float GPS_WEIGHT_HDOP = 0.5625f;  // 45 / 80
constexpr float GPS_WEIGHT_FIX  = 0.2500f;  // 20 / 80
constexpr float GPS_WEIGHT_SATS = 0.1875f;  // 15 / 80
// constexpr float GPS_WEIGHT_SNR = 0.2000f; // 20 / 100 — disabled

// Sanity caps (applied after weighted score → bar conversion)
constexpr float GPS_CAP_HDOP_THRESH  = 2.5f;  // HDOP above this → max 1 bar
constexpr int   GPS_CAP_HDOP_MAX_BARS = 1;
constexpr int   GPS_CAP_2D_MAX_BARS   = 2;    // 2D-only fix → max 2 bars
// SNR cap (GPS_SNR_THRESH_25 → max 2 bars) — not enforced while SNR is unavailable

// ---------------------------------------------------------------------------
// Speed calibration thresholds
// ---------------------------------------------------------------------------

// Flow sensor pulse frequency (Hz) above which we consider the DPV to be moving and start the run.
// Below this on start: stay in WAITING state.
// Raw pulse count threshold works even if k-factor calibration is wrong.
constexpr float SPEED_CAL_START_THRESHOLD_HZ  = 5.0f;   // ~5 pulses/sec — very low, just needs tiny flow

// Flow speed (m/s) below which we consider the DPV stopped (end of run).
// Fraction of start threshold — catches a clean stop without false-triggering
// on momentary deceleration mid-run.
constexpr float SPEED_CAL_STOP_THRESHOLD_MS   = 0.08f;  // ~0.15 kt

// Heading deviation (degrees) from EMA baseline that triggers end of run.
// Only checked after SPEED_CAL_MIN_RUN_S seconds have elapsed.
constexpr float SPEED_CAL_HEADING_STOP_DEG    = 90.0f;

// Minimum run duration (seconds) before heading-change stop is checked.
// Prevents early termination due to heading wobble at run start.
constexpr float SPEED_CAL_MIN_RUN_S           = 30.0f;

// EMA alpha for heading tracking during the speed cal run.
// Low alpha = slow-moving reference that represents the "stable" course.
constexpr float SPEED_CAL_HDG_EMA_ALPHA       = 0.02f;
