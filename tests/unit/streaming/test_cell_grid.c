/*
 * test_cell_grid.c: the world partition cell math (DENG-271, src/world/streaming/cell_grid.c,
 * far_proxy.c and streaming_internal.h): cell coordinates at +-100 km, cell names, the
 * nearest-first neighbour order, camera-relative cell origins and the far-proxy fade.
 */
#include "de_test.h"
#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_math.h>
#include "world/streaming/streaming_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- test_cell_grid_coords_large ------------------------------------------------------------ */

TEST(streaming, cell_grid_coords_large) {
    i32 cx = 7, cy = 7;
    /* The boundaries: a cell is [k*256, k*256+256). */
    de_world_cell_of(0.0, 0.0, &cx, &cy);
    EXPECT_EQ(cx, 0);
    EXPECT_EQ(cy, 0);
    de_world_cell_of(255.999, 256.0, &cx, &cy);
    EXPECT_EQ(cx, 0);
    EXPECT_EQ(cy, 1);
    de_world_cell_of(-0.001, -256.0, &cx, &cy);
    EXPECT_EQ(cx, -1);
    EXPECT_EQ(cy, -1);
    de_world_cell_of(-256.001, -512.0, &cx, &cy);
    EXPECT_EQ(cx, -2);
    EXPECT_EQ(cy, -2);

    /* +-100 km: 100000 / 256 = 390.625. */
    de_world_cell_of(100000.0, -100000.0, &cx, &cy);
    EXPECT_EQ(cx, 390);
    EXPECT_EQ(cy, -391);
    de_world_cell_of(99840.0, 99839.999999, &cx, &cy);
    EXPECT_EQ(cx, 390);
    EXPECT_EQ(cy, 389);

    /* The origin round-trips exactly in f64, far out too. */
    for (i32 k = -400; k <= 400; k += 37) {
        de_dvec3 o = de__cell_origin(k, -k);
        de_world_cell_of(o.x, o.y, &cx, &cy);
        EXPECT_EQ(cx, k);
        EXPECT_EQ(cy, -k);
        EXPECT_FLOAT_EQ(o.z, 0.0, 0.0);
    }

    /* Clamps instead of overflowing; NaN goes to the low clamp. */
    EXPECT_EQ(de__cell_coord(1e300), DE__CELL_COORD_MAX);
    EXPECT_EQ(de__cell_coord(-1e300), -DE__CELL_COORD_MAX);
    EXPECT_EQ(de__cell_coord(NAN), -DE__CELL_COORD_MAX);
    de_world_cell_of(1.0, 1.0, NULL, NULL); /* NULL outputs are allowed */

    /* Names and their parse. */
    char buf[64];
    ASSERT_TRUE(de__cell_name(buf, sizeof buf, "small", 390, -391));
    EXPECT_STREQ(buf, "world/small/cell_390_-391");
    ASSERT_TRUE(de__cell_leaf(buf, sizeof buf, -1, 0));
    EXPECT_STREQ(buf, "cell_-1_0");
    EXPECT_FALSE(de__cell_leaf(buf, 6, 1, 1));
    i32 px = 0, py = 0;
    EXPECT_TRUE(de__cell_name_parse("world/small/cell_390_-391", "small", &px, &py));
    EXPECT_EQ(px, 390);
    EXPECT_EQ(py, -391);
    EXPECT_TRUE(de__cell_name_parse("world/small/cell_0_0", "small", &px, &py));
    EXPECT_EQ(px, 0);
    EXPECT_EQ(py, 0);
    /* One spelling per cell: no '+', no leading zeros, no "-0", no other world, no junk. */
    EXPECT_FALSE(de__cell_name_parse("world/small/cell_+1_0", "small", &px, &py));
    EXPECT_FALSE(de__cell_name_parse("world/small/cell_01_0", "small", &px, &py));
    EXPECT_FALSE(de__cell_name_parse("world/small/cell_-0_0", "small", &px, &py));
    EXPECT_FALSE(de__cell_name_parse("world/big/cell_1_0", "small", &px, &py));
    EXPECT_FALSE(de__cell_name_parse("world/small/cell_1_0x", "small", &px, &py));
    EXPECT_FALSE(de__cell_name_parse("world/small/cell_1", "small", &px, &py));
    EXPECT_FALSE(de__cell_name_parse("world/small/cell_99999999999_0", "small", &px, &py));
    /* DENG-830: the far proxy bundles use the same rules under "proxy_". */
    EXPECT_TRUE(de__cell_proxy_parse("world/small/proxy_-3_12", "small", &px, &py));
    EXPECT_EQ(px, -3);
    EXPECT_EQ(py, 12);
    EXPECT_FALSE(de__cell_proxy_parse("world/small/cell_1_0", "small", &px, &py));
    EXPECT_FALSE(de__cell_name_parse("world/small/proxy_1_0", "small", &px, &py));
    EXPECT_FALSE(de__cell_proxy_parse("world/small/proxy_01_0", "small", &px, &py));
    EXPECT_FALSE(de__cell_proxy_parse("world/smal/proxy_1_0", "small", &px, &py));

    /* Distance to a cell rectangle: 0 inside, edge and corner distances outside. */
    EXPECT_FLOAT_EQ(de__cell_dist2(0, 0, 10.0, 10.0), 0.0, 0.0);
    EXPECT_FLOAT_EQ(de__cell_dist2(1, 0, 200.0, 10.0), 56.0 * 56.0, 0.0);
    EXPECT_FLOAT_EQ(de__cell_dist2(1, 1, 253.0, 252.0), 3.0 * 3.0 + 4.0 * 4.0, 0.0);
}

/* ---- test_cell_grid_neighbour_order --------------------------------------------------------- */

TEST(streaming, cell_grid_neighbour_order) {
    de__cell_key keys[64];
    /* From the middle of cell (0, 0) with a 200 m radius: the cell, then its 4 edge neighbours
     * (72 m), then the 4 corners (72*sqrt2 m). */
    u32 n = de__cells_in_radius(128.0, 128.0, 200.0, keys, 64);
    ASSERT_EQ(n, 9u);
    EXPECT_EQ(keys[0].cx, 0);
    EXPECT_EQ(keys[0].cy, 0);
    EXPECT_FLOAT_EQ(keys[0].dist2, 0.0, 0.0);
    for (u32 i = 1; i < 5; ++i) {
        EXPECT_FLOAT_EQ(keys[i].dist2, 128.0 * 128.0, 0.0);
        EXPECT_EQ(abs(keys[i].cx) + abs(keys[i].cy), 1);
    }
    for (u32 i = 5; i < 9; ++i) {
        EXPECT_FLOAT_EQ(keys[i].dist2, 2.0 * 128.0 * 128.0, 0.0);
        EXPECT_EQ(abs(keys[i].cx), 1);
        EXPECT_EQ(abs(keys[i].cy), 1);
    }
    /* Ties break on cx, then cy. */
    EXPECT_EQ(keys[1].cx, -1);
    EXPECT_EQ(keys[2].cx, 0);
    EXPECT_EQ(keys[2].cy, -1);
    EXPECT_EQ(keys[3].cx, 0);
    EXPECT_EQ(keys[3].cy, 1);
    EXPECT_EQ(keys[4].cx, 1);
    for (u32 i = 1; i < n; ++i)
        EXPECT_TRUE(de__cell_key_cmp(&keys[i - 1], &keys[i]) < 0);

    /* The total is returned even when the output is short; the kept ones are the nearest. */
    de__cell_key few[3];
    EXPECT_EQ(de__cells_in_radius(128.0, 128.0, 200.0, few, 3), 9u);
    EXPECT_EQ(few[0].cx, 0);
    EXPECT_EQ(few[0].cy, 0);
    EXPECT_FLOAT_EQ(few[1].dist2, 128.0 * 128.0, 0.0);

    /* Far out (100 km) the order holds the same. */
    n = de__cells_in_radius(100000.0, 100000.0, 600.0, keys, 64);
    ASSERT_TRUE(n > 9u && n <= 64u);
    EXPECT_EQ(keys[0].cx, 390);
    EXPECT_EQ(keys[0].cy, 390);
    for (u32 i = 1; i < n; ++i) {
        EXPECT_TRUE(de__cell_key_cmp(&keys[i - 1], &keys[i]) < 0);
        EXPECT_TRUE(keys[i].dist2 <= 600.0 * 600.0);
    }

    /* The sort is the same order whatever the input order. */
    de__cell_key mixed[5] = {{4.0, 2, 0}, {1.0, 0, 0}, {4.0, -1, 3}, {0.0, 5, 5}, {1.0, 0, -1}};
    de__cell_keys_sort(mixed, 5);
    EXPECT_EQ(mixed[0].cx, 5);
    EXPECT_EQ(mixed[1].cy, -1);
    EXPECT_EQ(mixed[2].cy, 0);
    EXPECT_EQ(mixed[3].cx, -1);
    EXPECT_EQ(mixed[4].cx, 2);
}

/* Camera-relative cell origins stay exact to well under 1 mm at 100 km. */
TEST(streaming, cell_origins_relative) {
    const i32 cells[6] = {390, 390, 391, 390, -391, -391};
    de_vec3 out[3];
    de_dvec3 cam = {100000.0, 100000.125, 3.0};
    de_world_cell_origins_relative(cells, 3, cam, out);
    EXPECT_FLOAT_EQ(out[0].x, -160.0f, 1e-4f);
    EXPECT_FLOAT_EQ(out[0].y, -160.125f, 1e-4f);
    EXPECT_FLOAT_EQ(out[0].z, -3.0f, 1e-4f);
    EXPECT_FLOAT_EQ(out[1].x, 96.0f, 1e-4f);
    /* A point 0.4 mm off the camera, through the cell origin: f32 keeps it. */
    de_dvec3 p = {100000.0004, 100000.125, 3.0};
    de_world_cell_origins_relative(cells, 1, p, out);
    f64 local_x = 160.0; /* the entity's cell-local x */
    EXPECT_TRUE(fabs(((f64)out[0].x + local_x) - (-0.0004)) < 1e-5);
    de_world_cell_origins_relative(NULL, 1, cam, out); /* no-op */
}

/* ---- far proxy fade -------------------------------------------------------------------------- */

TEST(streaming, far_proxy_fade) {
    de__far_proxy fp;
    de__far_proxy_reset(&fp);
    EXPECT_TRUE(fp.active);
    EXPECT_FLOAT_EQ(fp.alpha, 1.0f, 0.0);
    /* Unloaded: stays a full proxy. */
    de__far_proxy_step(&fp, false, 0.1f, 0.25f);
    EXPECT_TRUE(fp.active);
    EXPECT_FLOAT_EQ(fp.alpha, 1.0f, 0.0);
    /* Loaded: fades out over 0.25 s. */
    de__far_proxy_step(&fp, true, 0.1f, 0.25f);
    EXPECT_TRUE(fp.active);
    EXPECT_FLOAT_EQ(fp.alpha, 0.6f, 1e-5f);
    de__far_proxy_step(&fp, true, 0.1f, 0.25f);
    EXPECT_FLOAT_EQ(fp.alpha, 0.2f, 1e-5f);
    de__far_proxy_step(&fp, true, 0.1f, 0.25f);
    EXPECT_FALSE(fp.active);
    EXPECT_FLOAT_EQ(fp.alpha, 0.0f, 0.0);
    de__far_proxy_step(&fp, true, 0.1f, 0.25f);
    EXPECT_FALSE(fp.active);
    /* Unloaded again: back to a full proxy at once. */
    de__far_proxy_step(&fp, false, 0.1f, 0.25f);
    EXPECT_TRUE(fp.active);
    EXPECT_FLOAT_EQ(fp.alpha, 1.0f, 0.0);
    /* No fade time: off at the first loaded frame. */
    de__far_proxy_step(&fp, true, 0.016f, 0.0f);
    EXPECT_FALSE(fp.active);
}
