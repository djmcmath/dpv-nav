#include "depth.h"
#include "../drivers/ms5837.h"
#include "../config.h"

namespace depth {

namespace {
float gZeroOffsetMbar = 0.0f;
bool  gZeroed         = false;
float gDensityKgM3    = WATER_DENSITY_SALT_KG_M3;
float gDepthM         = 0.0f;
float gTempC          = 0.0f;
bool  gNewSample      = false;
constexpr float GRAVITY_M_S2 = 9.80665f;
}  // namespace

bool init() {
    return ms5837::init();
}

bool isPresent() {
    return ms5837::isPresent();
}

void update() {
    ms5837::update();
    if (ms5837::hasNewSample()) {
        gTempC = ms5837::temperatureC();
        if (gZeroed) {
            float deltaMbar = ms5837::pressureMbar() - gZeroOffsetMbar;
            gDepthM = (deltaMbar * 100.0f) / (gDensityKgM3 * GRAVITY_M_S2);
        }
        gNewSample = true;
    }
}

bool hasNewSample() {
    if (!gNewSample) return false;
    gNewSample = false;
    return true;
}

void zero() {
    gZeroOffsetMbar = ms5837::pressureMbar();
    gZeroed         = true;
    gDepthM         = 0.0f;
}

void setSaltWater(bool salt) {
    gDensityKgM3 = salt ? WATER_DENSITY_SALT_KG_M3 : WATER_DENSITY_FRESH_KG_M3;
}

float getDepth_m() { return gDepthM; }
float getTemp_c()  { return gTempC; }

}  // namespace depth
