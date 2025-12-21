
#include "../types/types.h"

namespace imu {
    void init();
    void initMag();
    mag_reading readMagRaw(int16_t &mx, int16_t &my, int16_t &mz);
}