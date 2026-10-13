/* Smoke test for de_tp_meshoptimizer: the C API compiles as C11 under the engine's strict flags,
 * and a C executable links the C++ library. Optimizes, simplifies and clusters a small grid.
 * The importer tests are in tests/unit/import/ (test_meshlets.c, test_mesh_lod.c). */
#include <string.h>

#include <meshoptimizer.h>

#include "de_test.h"

/* A 9 x 9 vertex grid in the XY plane: 8 x 8 quads, 128 triangles. */
#define GRID      9u
#define GRID_TRIS (2u * (GRID - 1u) * (GRID - 1u))

static void make_grid(float *pos, unsigned int *idx) {
    for (unsigned int y = 0; y < GRID; ++y)
        for (unsigned int x = 0; x < GRID; ++x) {
            float *p = pos + 3u * (y * GRID + x);
            p[0] = (float)x;
            p[1] = (float)y;
            p[2] = 0.0f;
        }
    unsigned int k = 0;
    for (unsigned int y = 0; y + 1u < GRID; ++y)
        for (unsigned int x = 0; x + 1u < GRID; ++x) {
            unsigned int a = y * GRID + x, b = a + 1u, c = a + GRID, d = c + 1u;
            idx[k++] = a;
            idx[k++] = b;
            idx[k++] = d;
            idx[k++] = a;
            idx[k++] = d;
            idx[k++] = c;
        }
}

TEST(de_tp_smoke_meshoptimizer, optimize_simplify) {
    static float pos[3u * GRID * GRID];
    static unsigned int idx[3u * GRID_TRIS], opt[3u * GRID_TRIS], lod[3u * GRID_TRIS];
    make_grid(pos, idx);
    meshopt_optimizeVertexCache(opt, idx, 3u * GRID_TRIS, GRID * GRID);
    /* A permutation of the triangles: same count, every index in range. */
    for (unsigned int i = 0; i < 3u * GRID_TRIS; ++i)
        ASSERT_TRUE(opt[i] < GRID * GRID);
    float err = -1.0f;
    size_t n =
        meshopt_simplify(lod, opt, 3u * GRID_TRIS, pos, GRID * GRID, 12u, 6u, 1.0f, 0u, &err);
    /* Fewer triangles, with a relative error inside the limit. */
    EXPECT_TRUE(n < 3u * GRID_TRIS);
    EXPECT_TRUE(n % 3u == 0u);
    EXPECT_TRUE(err >= 0.0f && err <= 1.0f);
    EXPECT_TRUE(meshopt_simplifyScale(pos, GRID * GRID, 12u) > 7.9f);
}

TEST(de_tp_smoke_meshoptimizer, meshlets) {
    static float pos[3u * GRID * GRID];
    static unsigned int idx[3u * GRID_TRIS];
    make_grid(pos, idx);
    size_t cap = meshopt_buildMeshletsBound(3u * GRID_TRIS, 64u, 124u);
    ASSERT_TRUE(cap >= 2u && cap < 64u);
    static struct meshopt_Meshlet ml[64];
    static unsigned int mv[3u * GRID_TRIS];
    static unsigned char mt[3u * GRID_TRIS];
    size_t count = meshopt_buildMeshlets(ml, mv, mt, idx, 3u * GRID_TRIS, pos, GRID * GRID, 12u,
                                         64u, 124u, 0.25f);
    ASSERT_TRUE(count >= 2u && count <= cap);
    unsigned int tris = 0;
    for (size_t i = 0; i < count; ++i) {
        EXPECT_TRUE(ml[i].vertex_count <= 64u);
        EXPECT_TRUE(ml[i].triangle_count <= 124u);
        tris += ml[i].triangle_count;
        struct meshopt_Bounds b =
            meshopt_computeMeshletBounds(mv + ml[i].vertex_offset, mt + ml[i].triangle_offset,
                                         ml[i].triangle_count, pos, GRID * GRID, 12u);
        EXPECT_TRUE(b.radius > 0.0f);
        /* Every triangle of the flat grid faces +Z: the cone axis is +Z. */
        EXPECT_FLOAT_EQ(b.cone_axis[2], 1.0f, 1e-4f);
    }
    EXPECT_EQ(tris, GRID_TRIS);
}
