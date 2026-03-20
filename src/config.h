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

// ---------------------------------------------------------------------------
// Speed calibration thresholds
// ---------------------------------------------------------------------------

// Flow speed (m/s) above which we consider the DPV to be moving and start the run.
// Below this on start: stay in WAITING state.
constexpr float SPEED_CAL_START_THRESHOLD_MS  = 0.3f;   // ~0.6 kt — definite motion

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
