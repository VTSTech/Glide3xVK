// R114 host check: g3x_resolution_size must map the Vanilla Plus stops and
// keep every stock enum byte-identical. Run: g++ -o /tmp/r114test scripts/test_res18.cpp
#include "../src/g3x_types.h"
#include <cstdio>

static int fails = 0;
static void expect(GrScreenResolution_t res, unsigned ew, unsigned eh) {
    unsigned w = 0, h = 0;
    g3x_resolution_size(res, &w, &h);
    bool ok = (w == ew && h == eh);
    if (!ok) ++fails;
    std::printf("%s res=%2u -> %4ux%-4u (want %4ux%-4u)\n",
                ok ? "PASS" : "FAIL", (unsigned)res, w, h, ew, eh);
}

int main() {
    // stock enums the game actually sends (D2Glide: 7 = 640x480, 8 = 800x600)
    expect(7, 640, 480);
    expect(8, 800, 600);
    // Vanilla Plus stops
    expect(16, 1280, 720);  // mode 4, live since R108
    expect(17, 1344, 700);  // mode 3, dormant (R109)
    expect(18, 1366, 768);  // mode 5, R112 fifth stop — the R114 fix
    // genuinely unknown: soft-fail must survive for future values
    expect(19, 640, 480);
    expect(255, 640, 480);
    std::printf(fails ? "FAILED (%d)\n" : "ALL GREEN\n", fails);
    return fails != 0;
}
