// Harness for tools/gapfill_chain_check.py.
//
// Exercises the whole device-side half of the gap-fill chain in one pass:
//   60 server status codes -> CalProgressPacket -> JSON wire -> decode ->
//   per-sample algebraic bin index -> the status the device would render.
//
// stdin:
//   line 1: 60 space-separated status codes (0=ok,1=thin,2=empty,3=over)
//   rest:   "mx,my,mz,ax,ay,az" rows (calibrated mag counts, accel in g)
// stdout: one "bin,status" line per sample row.
#include "../../lib/dpvlink/dpvlink.h"
#include "../../src/util/mag_cal_orient.h"
#include <cstdio>
#include <cstring>

int main() {
    CalProgressPacket tx{};
    tx.cal_type = (uint8_t)CalType::BASELINE;
    tx.phase    = (uint8_t)CalPhase::GAP_FILL;
    tx.bins_total = 60;
    tx.has_targets = true;
    for (int i = 0; i < 60; i++) {
        int v = 0;
        if (scanf("%d", &v) != 1) { fprintf(stderr, "short target list\n"); return 2; }
        tx.targets[i] = (uint8_t)v;
    }

    char buf[512];
    size_t n = calProgressPacketToBytes(tx, buf, sizeof(buf));
    if (n == 0) { fprintf(stderr, "encode failed\n"); return 2; }

    CalProgressPacket rx{};
    if (!bytesToCalProgressPacket(buf, n, rx) || !rx.has_targets) {
        fprintf(stderr, "decode failed\n");
        return 2;
    }

    double mx, my, mz, ax, ay, az;
    while (scanf(" %lf,%lf,%lf,%lf,%lf,%lf", &mx, &my, &mz, &ax, &ay, &az) == 6) {
        int bin = mag_orient::binIndexFor((float)mx, (float)my, (float)mz,
                                          (float)ax, (float)ay, (float)az);
        printf("%d,%u\n", bin, (unsigned)rx.targets[bin]);
    }
    return 0;
}
