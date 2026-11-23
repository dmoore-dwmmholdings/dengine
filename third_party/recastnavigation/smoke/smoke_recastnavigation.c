/* Smoke test for de_tp_recastnavigation (DENG-76): a C11 executable under the engine's strict
 * flags links the C++ library, and Recast builds a walkable tile of a flat quad that Detour loads.
 * The nav module tests are in tests/unit/nav/. */
#include "de_test.h"

int de_smoke_recast_build_quad(int *out_poly_count);

TEST(de_tp_smoke_recastnavigation, build_quad_tile) {
    int polys = 0;
    EXPECT_EQ(de_smoke_recast_build_quad(&polys), 1);
    EXPECT_TRUE(polys > 0);
}
