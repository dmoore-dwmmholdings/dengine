/* Smoke test for de_tp_bcdec (test-only reference decoder, DENG-99): <bcdec.h> compiles with the
 * engine's strict flags, and hand-made BC1 and BC4 blocks decode to the expected texels. The BC
 * tests of the texture importer (tests/unit/import/test_texture_bc.c) decode every format. */
#include <bcdec.h>

#include <string.h>

#include "de_test.h"

TEST(de_tp_smoke_bcdec, bc1_solid) {
    /* color0 = 0xF800 (red), color1 = 0x001F (blue), every index 0: all red, opaque. */
    static const unsigned char block[8] = {0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0};
    unsigned char rgba[4 * 4 * 4];
    memset(rgba, 0x55, sizeof rgba);
    bcdec_bc1(block, rgba, 4 * 4);
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(rgba[i * 4 + 0], 255);
        EXPECT_EQ(rgba[i * 4 + 1], 0);
        EXPECT_EQ(rgba[i * 4 + 2], 0);
        EXPECT_EQ(rgba[i * 4 + 3], 255);
    }
}

TEST(de_tp_smoke_bcdec, bc4_endpoints) {
    /* red0 = 200, red1 = 100; index 0 everywhere but texel 0, which has index 1 (red1). */
    static const unsigned char block[8] = {200, 100, 0x01, 0, 0, 0, 0, 0};
    unsigned char r[16];
    bcdec_bc4(block, r, 4);
    EXPECT_EQ(r[0], 100);
    for (int i = 1; i < 16; ++i)
        EXPECT_EQ(r[i], 200);
}
