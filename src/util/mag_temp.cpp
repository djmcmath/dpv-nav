#include "mag_temp.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <math.h>

namespace mag_temp {

static bool coeffOk(float c) {
    return isfinite(c) && fabsf(c) <= MAX_ABS_COEFF_UT_PER_C;
}

LoadResult load(MagTempCal& cal) {
    cal = MagTempCal{};

    File f = LittleFS.open(FILE_PATH, "r");
    if (!f) return LoadResult::Absent;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return LoadResult::Invalid;

    // Every field is required: a missing axis defaulting to 0 would silently
    // compensate two axes and not the third.
    JsonVariantConst c = doc["coeff_uT_per_c"];
    if (!doc["ref_temp_c"].is<float>() || !c["x"].is<float>() ||
        !c["y"].is<float>() || !c["z"].is<float>()) {
        return LoadResult::Invalid;
    }

    MagTempCal loaded;
    loaded.ref_temp_c       = doc["ref_temp_c"].as<float>();
    loaded.coeff_uT_per_c.x = c["x"].as<float>();
    loaded.coeff_uT_per_c.y = c["y"].as<float>();
    loaded.coeff_uT_per_c.z = c["z"].as<float>();

    if (!isfinite(loaded.ref_temp_c) ||
        loaded.ref_temp_c < MIN_REF_TEMP_C || loaded.ref_temp_c > MAX_REF_TEMP_C ||
        !coeffOk(loaded.coeff_uT_per_c.x) || !coeffOk(loaded.coeff_uT_per_c.y) ||
        !coeffOk(loaded.coeff_uT_per_c.z)) {
        return LoadResult::Invalid;
    }

    cal = loaded;
    return LoadResult::Ok;
}

}  // namespace mag_temp
