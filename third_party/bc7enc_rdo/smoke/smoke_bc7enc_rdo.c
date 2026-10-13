/* Smoke test for de_tp_bc7enc_rdo: the encoders link, and a smooth 4x4 block survives BC7, BC1 and
 * BC4 with a small error. Quality on real images is tested by the texture importer
 * (tests/unit/import/test_texture_bc.c, import.texture_bc.*). */
#include <stdint.h>

#include "de_test.h"

int de_smoke_bc7enc_rdo_bc7(const uint8_t *rgba);
int de_smoke_bc7enc_rdo_bc1(const uint8_t *rgba);
int de_smoke_bc7enc_rdo_bc4(const uint8_t *rgba);

/* A horizontal gradient: red 0 to 255, green and blue fixed, alpha 255. */
static void gradient(uint8_t *rgba) {
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            uint8_t *p = rgba + (y * 4 + x) * 4;
            p[0] = (uint8_t)(x * 85);
            p[1] = 128;
            p[2] = 64;
            p[3] = 255;
        }
}

TEST(de_tp_smoke_bc7enc_rdo, bc7_block) {
    uint8_t rgba[64];
    gradient(rgba);
    EXPECT_LE(de_smoke_bc7enc_rdo_bc7(rgba), 4);
}

TEST(de_tp_smoke_bc7enc_rdo, bc1_block) {
    uint8_t rgba[64];
    gradient(rgba);
    EXPECT_LE(de_smoke_bc7enc_rdo_bc1(rgba), 12);
}

TEST(de_tp_smoke_bc7enc_rdo, bc4_block) {
    uint8_t rgba[64];
    gradient(rgba);
    EXPECT_LE(de_smoke_bc7enc_rdo_bc4(rgba), 19); /* 8 levels from 0 to 255: steps of 36 */
}
