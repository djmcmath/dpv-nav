// Host-side harness for tools/orient_equivalence.py.
// Reads "mx,my,mz,ax,ay,az" rows on stdin (calibrated mag counts, accel in g)
// and prints "pitch_deg,heading_deg,bin,roll_deg,roll_sector" per row, using
// the *firmware* port. Not compiled into any PlatformIO env -- it exists
// only so the port can be diffed against callib/coverage.py without
// hardware.
#include "../../src/util/mag_cal_orient.h"
#include <cstdio>

int main() {
    double mx, my, mz, ax, ay, az;
    while (scanf("%lf,%lf,%lf,%lf,%lf,%lf", &mx, &my, &mz, &ax, &ay, &az) == 6) {
        float pitch = 0.0f, hdg = 0.0f;
        mag_orient::reconstructOrientation((float)mx, (float)my, (float)mz,
                                           (float)ax, (float)ay, (float)az,
                                           pitch, hdg);
        float roll = mag_orient::reconstructRoll((float)ax, (float)ay, (float)az);
        printf("%.9f,%.9f,%d,%.9f,%d\n", (double)pitch, (double)hdg,
               mag_orient::binIndex(pitch, hdg), (double)roll,
               mag_orient::rollSector(roll));
    }
    return 0;
}
