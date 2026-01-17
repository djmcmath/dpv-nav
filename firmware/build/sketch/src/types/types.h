#line 1 "D:\\Documents\\dpv-nav\\firmware\\src\\types\\types.h"
#include <cstdint>

#ifndef mag_reading_h
#define mag_reading_h

struct mag_reading {
    int16_t x;
    int16_t y;
    int16_t z;
};

#endif // mag_reading_h