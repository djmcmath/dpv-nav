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

// Position estimation
constexpr float DEFAULT_BASELINE_LAT     = 42.0f;   // degrees N (assumed until GPS fix)
constexpr float DEFAULT_BASELINE_LON     = -122.0f;  // degrees W (assumed until GPS fix)
constexpr bool  DEFAULT_USE_GPS_POSITION = true;     // use GPS lat/lon as position truth when available

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
