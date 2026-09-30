#include "nav_model.h"
#include <math.h>

static constexpr float DEG_TO_RAD = M_PI / 180.0f;
static constexpr float RAD_TO_DEG = 180.0f / M_PI;
static constexpr float M_PER_DEG_LAT = 111320.0f;  // meters per degree latitude

static float baseLat = 0.0f;
static float baseLon = 0.0f;
static float mPerDegLon = 0.0f;  // meters per degree longitude (depends on latitude)

static float cur_x = 0.0f;   // meters east of baseline
static float cur_y = 0.0f;   // meters north of baseline
static float home_x = 0.0f;
static float home_y = 0.0f;
static bool  homeSet = false;
static float datum_x = 0.0f;
static float datum_y = 0.0f;
static bool  datumSet = false;
static bool  useGps  = true;

// Compute meters-per-degree-longitude at a given latitude
static float mPerDegLonAt(float latDeg) {
    return M_PER_DEG_LAT * cosf(latDeg * DEG_TO_RAD);
}

namespace nav {

void init(float baselineLat, float baselineLon) {
    baseLat = baselineLat;
    baseLon = baselineLon;
    mPerDegLon = mPerDegLonAt(baseLat);
    cur_x = 0.0f;
    cur_y = 0.0f;
    home_x = 0.0f;
    home_y = 0.0f;
    homeSet = false;
    datumSet = false;
}

void setHome() {
    home_x = cur_x;
    home_y = cur_y;
    homeSet = true;
}

void setTargetLatLon(float lat, float lon) {
    home_x = (lon - baseLon) * mPerDegLon;
    home_y = (lat - baseLat) * M_PER_DEG_LAT;
    homeSet = true;
}

void clearHome() {
    homeSet = false;
    home_x = 0.0f;
    home_y = 0.0f;
}

void updateDR(float heading_deg, float speed_ms, float dt) {
    if (dt <= 0.0f || speed_ms <= 0.0f) return;
    float hdg_rad = heading_deg * DEG_TO_RAD;
    cur_x += speed_ms * sinf(hdg_rad) * dt;  // east component
    cur_y += speed_ms * cosf(hdg_rad) * dt;  // north component
}

void updateGPS(float lat, float lon) {
    if (!useGps) return;
    cur_x = (lon - baseLon) * mPerDegLon;
    cur_y = (lat - baseLat) * M_PER_DEG_LAT;
}

void setUseGps(bool enable) {
    useGps = enable;
}

void setPosition(float x_m, float y_m) {
    cur_x = x_m;
    cur_y = y_m;
}

void snapToLatLon(float lat, float lon) {
    float nx = (lon - baseLon) * mPerDegLon;
    float ny = (lat - baseLat) * M_PER_DEG_LAT;
    if (datumSet) {
        datum_x += nx - cur_x;
        datum_y += ny - cur_y;
    }
    cur_x = nx;
    cur_y = ny;
}

void setDatum() {
    datum_x = cur_x;
    datum_y = cur_y;
    datumSet = true;
}

void setDatumXY(float x_m, float y_m) {
    datum_x = x_m;
    datum_y = y_m;
    datumSet = true;
}

void clearDatum() {
    datumSet = false;
    datum_x = 0.0f;
    datum_y = 0.0f;
}

bool hasDatum() {
    return datumSet;
}

Position getDatum() {
    Position p;
    p.x_m = datum_x;
    p.y_m = datum_y;
    p.lat = baseLat + datum_y / M_PER_DEG_LAT;
    p.lon = baseLon + datum_x / mPerDegLon;
    return p;
}

bool retroArrive(float lat, float lon) {
    if (!datumSet) return false;
    float nx = (lon - baseLon) * mPerDegLon;
    float ny = (lat - baseLat) * M_PER_DEG_LAT;
    cur_x += nx - datum_x;
    cur_y += ny - datum_y;
    datum_x = nx;
    datum_y = ny;
    return true;
}

bool targetDatum() {
    if (!datumSet) return false;
    home_x = datum_x;
    home_y = datum_y;
    homeSet = true;
    return true;
}

Position getPosition() {
    Position p;
    p.x_m = cur_x;
    p.y_m = cur_y;
    p.lat = baseLat + cur_y / M_PER_DEG_LAT;
    p.lon = baseLon + cur_x / mPerDegLon;
    return p;
}

bool hasHome() {
    return homeSet;
}

bool isUsingGps() {
    return useGps;
}

float distanceToHome_m() {
    if (!homeSet) return 0.0f;
    float dx = cur_x - home_x;
    float dy = cur_y - home_y;
    return sqrtf(dx * dx + dy * dy);
}

float bearingToHome_deg() {
    if (!homeSet) return 0.0f;
    float dx = home_x - cur_x;  // east component toward home
    float dy = home_y - cur_y;  // north component toward home
    float bearing = atan2f(dx, dy) * RAD_TO_DEG;
    if (bearing < 0.0f) bearing += 360.0f;
    return bearing;
}

}  // namespace nav
