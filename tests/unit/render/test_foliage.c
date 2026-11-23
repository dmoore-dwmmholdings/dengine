/*
 * test_foliage.c (DENG-822, DENG-823): foliage instances in the render world. Each instance on a
 * DeFoliagePage of a DeFoliage layer becomes a mesh instance slot (the instanced GPU scene path)
 * owned by the layer entity; a cell beyond its type's cullDistance from the main camera is not
 * drawn; a changed page rebuilds its slots and a removed layer frees them. DIDE-R167 AC1: a layer of
 * 100 m x 100 m at 4/m^2 (40000 instances over 49 cells) is rendered in full. Also the foliageType
 * parse and the alignToNormal rotation.
 */
#include "de_test.h"
#include "test_render_world.h"
#include "render/renderer_internal.h"
#include <string.h>

static void rwt_setup(rwt *f) {
    memset(f, 0, sizeof *f);
    rwt_start(f, 0, false);
}
static void rwt_teardown(rwt *f) {
    rwt_stop(f);
}

#define BLOB_GRASS 77u
#define BLOB_PINE  78u

/* A page of layer `layer`: `n` instances of cell (x, z). */
static de_entity spawn_page(rwt *f, de_entity layer, i32 x, i32 z, u32 page,
                            const de_foliage_instance *in, u32 n, u64 revision) {
    de_entity e = {0};
    ASSERT_EQ(de_entity_spawn_with(f->w, &f->ids.foliage_page, 1, &e), DE_OK);
    DeFoliagePage *p = (DeFoliagePage *)de_entity_get(f->w, e, f->ids.foliage_page);
    memset(p, 0, sizeof *p);
    p->layer = layer;
    p->x = x;
    p->z = z;
    p->page = page;
    p->count = n;
    p->revision = revision;
    memcpy(p->instances, in, sizeof *in * n);
    return e;
}

static de_entity g_grass_page;

/* A layer with `n` grass instances in cell (0, 0) and, when `pines`, 2 pines in cell (4, 0). */
static de_entity spawn_layer(rwt *f, u32 n, bool pines) {
    const de_component cs[2] = {f->bi.transform, f->ids.foliage};
    de_entity e = {0};
    ASSERT_EQ(de_entity_spawn_with(f->w, cs, 2, &e), DE_OK);
    *(DeTransform *)de_entity_get(f->w, e, f->bi.transform) = de_transform_identity();
    DeFoliage *fo = (DeFoliage *)de_entity_get(f->w, e, f->ids.foliage);
    memset(fo, 0, sizeof *fo);
    fo->types[0] = (de_blob){BLOB_GRASS};
    fo->types[1] = (de_blob){BLOB_PINE};
    de_foliage_instance in[16];
    ASSERT_TRUE(n <= 16u);
    for (u32 i = 0; i < n; ++i)
        in[i] = (de_foliage_instance){2.0f + (f32)i, 1.0f, 3.0f, 0.0f, 1.0f, 0u, 0u, 0, 0};
    g_grass_page = spawn_page(f, e, 0, 0, 0, in, n, 1);
    if (pines) {
        for (u32 i = 0; i < 2; ++i)
            in[i] = (de_foliage_instance){70.0f + (f32)i, 0.0f, 4.0f, 90.0f, 2.0f, 0u, 1u, 0, 0};
        (void)spawn_page(f, e, 4, 0, 0, in, 2, 1);
    }
    return e;
}

static u32 slots_of(const de__render_world_frame *fr, de_entity e, de_mesh mesh) {
    u32 n = 0;
    for (u32 s = 0; s < fr->instances.count; ++s)
        if (fr->instances.entity[s] == e.bits && fr->instances.mesh[s].id == mesh.id)
            n++;
    return n;
}

TEST_F(foliage, instances_become_mesh_slots_and_follow_changes, rwt) {
    de__render_world *rw = f->r->render_world;
    ASSERT_TRUE(de__rw_foliage_set_type(rw, (de_blob){BLOB_GRASS}, (de_mesh){5}, 0.0f));
    ASSERT_TRUE(de__rw_foliage_set_type(rw, (de_blob){BLOB_PINE}, (de_mesh){6}, 0.0f));
    const de_entity e = spawn_layer(f, 5, true);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    const de__render_world_frame *fr = rwt_last(f);
    EXPECT_EQ(slots_of(fr, e, (de_mesh){5}), 5u);
    EXPECT_EQ(slots_of(fr, e, (de_mesh){6}), 2u);
    /* The transform: the instance position (render origin 0), uniform scale. */
    u32 at = DE__RW_NONE;
    for (u32 s = 0; s < fr->instances.count; ++s)
        if (fr->instances.entity[s] == e.bits && fr->instances.mesh[s].id == 6u)
            at = s;
    ASSERT_NE(at, DE__RW_NONE);
    EXPECT_TRUE(fr->instances.flags[at] & DE__RW_INST_CAST_SHADOWS);
    EXPECT_FALSE(fr->instances.flags[at] & DE__RW_INST_SPRITE);
    EXPECT_FLOAT_EQ(fr->instances.transform[at].m[11], 4.0f, 1e-5f); /* column 3: z */
    /* Unchanged frames keep the slots and dirty nothing. */
    ASSERT_EQ(rwt_frame(f), DE_OK);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    fr = rwt_last(f);
    EXPECT_EQ(slots_of(fr, e, (de_mesh){5}), 5u);
    EXPECT_EQ(fr->dirty_instance_count, 0u);
    /* A changed page: two grass instances fewer. */
    DeFoliagePage *pg = (DeFoliagePage *)de_entity_get(f->w, g_grass_page, f->ids.foliage_page);
    pg->count = 3;
    pg->revision = 2;
    de_entity_mark_changed(f->w, g_grass_page, f->ids.foliage_page);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    fr = rwt_last(f);
    EXPECT_EQ(slots_of(fr, e, (de_mesh){5}), 3u);
    EXPECT_EQ(slots_of(fr, e, (de_mesh){6}), 2u);
    /* A despawned layer frees every slot. */
    de_entity_despawn(f->w, e);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    fr = rwt_last(f);
    EXPECT_EQ(slots_of(fr, e, (de_mesh){5}) + slots_of(fr, e, (de_mesh){6}), 0u);
    EXPECT_EQ(fr->instances.live, 0u);
}

TEST_F(foliage, cells_beyond_the_cull_distance_are_not_drawn, rwt) {
    de__render_world *rw = f->r->render_world;
    /* Grass culls at 30 m, pine never. */
    ASSERT_TRUE(de__rw_foliage_set_type(rw, (de_blob){BLOB_GRASS}, (de_mesh){5}, 30.0f));
    ASSERT_TRUE(de__rw_foliage_set_type(rw, (de_blob){BLOB_PINE}, (de_mesh){6}, 0.0f));
    de_transform cam = de_transform_identity();
    const de_entity c = rwt_spawn_camera(f, cam, 1);
    const de_entity e = spawn_layer(f, 4, true);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    EXPECT_EQ(slots_of(rwt_last(f), e, (de_mesh){5}), 4u);
    rwt_move(f, c, (de_dvec3){100.0, 0.0, 0.0});
    ASSERT_EQ(rwt_frame(f), DE_OK);
    EXPECT_EQ(slots_of(rwt_last(f), e, (de_mesh){5}), 0u);
    EXPECT_EQ(slots_of(rwt_last(f), e, (de_mesh){6}), 2u);
    rwt_move(f, c, (de_dvec3){20.0, 0.0, 3.0}); /* 4 m past the cell's edge */
    ASSERT_EQ(rwt_frame(f), DE_OK);
    EXPECT_EQ(slots_of(rwt_last(f), e, (de_mesh){5}), 4u);
}

/* DIDE-R167 AC1: 100 m x 100 m at 4/m^2, in 16 m cells of up to 1024 instances (two pages each):
 * every instance gets a slot, and a despawned layer frees them all. */
TEST_F(foliage, a_100m_layer_at_4_per_m2_is_rendered_in_full, rwt) {
    de__render_world *rw = f->r->render_world;
    ASSERT_TRUE(de__rw_foliage_set_type(rw, (de_blob){BLOB_GRASS}, (de_mesh){5}, 0.0f));
    const de_component cs[2] = {f->bi.transform, f->ids.foliage};
    de_entity e = {0};
    ASSERT_EQ(de_entity_spawn_with(f->w, cs, 2, &e), DE_OK);
    *(DeTransform *)de_entity_get(f->w, e, f->bi.transform) = de_transform_identity();
    DeFoliage *fo = (DeFoliage *)de_entity_get(f->w, e, f->ids.foliage);
    memset(fo, 0, sizeof *fo);
    fo->types[0] = (de_blob){BLOB_GRASS};
    /* A 200 x 200 grid at 0.5 m: 40000 instances over cells 0..6 on each axis. */
    static de_foliage_instance cell[7][7][1024];
    static u32 count[7][7];
    memset(count, 0, sizeof count);
    u32 total = 0;
    for (u32 j = 0; j < 200; ++j)
        for (u32 i = 0; i < 200; ++i) {
            const f32 x = 0.25f + 0.5f * (f32)i, z = 0.25f + 0.5f * (f32)j;
            const u32 cx = (u32)(x / 16.0f), cz = (u32)(z / 16.0f);
            ASSERT_TRUE(count[cz][cx] < 1024u);
            cell[cz][cx][count[cz][cx]++] =
                (de_foliage_instance){x, 0.0f, z, 0.0f, 1.0f, 0u, 0u, 0, 0};
            ++total;
        }
    u32 pages = 0;
    for (u32 cz = 0; cz < 7; ++cz)
        for (u32 cx = 0; cx < 7; ++cx)
            for (u32 p = 0; p * DE_FOLIAGE_PAGE_INSTANCES < count[cz][cx]; ++p) {
                const u32 left = count[cz][cx] - p * DE_FOLIAGE_PAGE_INSTANCES;
                (void)spawn_page(f, e, (i32)cx, (i32)cz, p,
                                 &cell[cz][cx][p * DE_FOLIAGE_PAGE_INSTANCES],
                                 left < DE_FOLIAGE_PAGE_INSTANCES ? left : DE_FOLIAGE_PAGE_INSTANCES,
                                 1);
                ++pages;
            }
    EXPECT_EQ(total, 40000u);
    EXPECT_GT(pages, 49u);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    EXPECT_EQ(slots_of(rwt_last(f), e, (de_mesh){5}), total);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    EXPECT_EQ(rwt_last(f)->dirty_instance_count, 0u);
    de_entity_despawn(f->w, e);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    EXPECT_EQ(rwt_last(f)->instances.live, 0u);
}

TEST(foliage, parse_reads_mesh_and_cull_distance) {
    char mesh[64];
    f32 cull = -1.0f;
    static const char a[] = "mesh = \"mesh:grass\"\ndensity = 4.0\ncullDistance = 60.0\n";
    ASSERT_TRUE(de__rw_foliage_parse(a, sizeof a - 1, mesh, sizeof mesh, &cull));
    EXPECT_STREQ(mesh, "mesh:grass");
    EXPECT_FLOAT_EQ(cull, 60.0f, 0.0f);
    static const char b[] = "mesh = \"m\"\ncullDistance = 12\n";
    ASSERT_TRUE(de__rw_foliage_parse(b, sizeof b - 1, mesh, sizeof mesh, &cull));
    EXPECT_FLOAT_EQ(cull, 12.0f, 0.0f);
    static const char c[] = "mesh = \"m\"\n";
    ASSERT_TRUE(de__rw_foliage_parse(c, sizeof c - 1, mesh, sizeof mesh, &cull));
    EXPECT_FLOAT_EQ(cull, 0.0f, 0.0f);
    static const char d[] = "density = 1.0\n";
    EXPECT_FALSE(de__rw_foliage_parse(d, sizeof d - 1, mesh, sizeof mesh, &cull));
    static const char e[] = "mesh = [\n";
    EXPECT_FALSE(de__rw_foliage_parse(e, sizeof e - 1, mesh, sizeof mesh, &cull));
}

TEST(foliage, rotation_is_yaw_then_tilt_onto_the_normal) {
    /* Upright: the yaw alone. */
    de_quat q = de__rw_foliage_rotation(90.0f, 0, 0);
    de_vec3 v = de_quat_rotate(q, (de_vec3){1, 0, 0});
    EXPECT_FLOAT_EQ(v.x, 0.0f, 1e-5f);
    EXPECT_FLOAT_EQ(v.z, -1.0f, 1e-5f);
    /* Tilted: +y goes to the normal (-0.6, 0.8, 0) whatever the yaw. */
    for (u32 k = 0; k < 4; ++k) {
        q = de__rw_foliage_rotation(37.0f * (f32)k, (i8)-76, 0); /* -76/127 ~ -0.598 */
        v = de_quat_rotate(q, (de_vec3){0, 1, 0});
        EXPECT_FLOAT_EQ(v.x, -0.598f, 2e-3f);
        EXPECT_FLOAT_EQ(v.y, 0.801f, 2e-3f);
        EXPECT_FLOAT_EQ(v.z, 0.0f, 1e-5f);
    }
}
