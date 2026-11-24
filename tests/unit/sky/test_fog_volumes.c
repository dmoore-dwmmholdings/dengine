/*
 * test_fog_volumes.c: the CPU side of the froxel volumetric fog (DENG-207). The camera-relative
 * packing of DeFogVolume snapshots, the exponential slice distribution and its inverse, and the
 * pass list on a null-backend device: volumetrics OFF adds no fog pass at all, High adds the four,
 * and a second frame allocates nothing.
 */
#include "de_test.h"

#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_render.h>

#include "render/graph/rg_internal.h"
#include "render/world/render_world.h"
#include "world/sky/fog_internal.h"

#include <math.h>
#include <string.h>

static de__fog_snapshot snap(de_vec3 pos, de_vec3 extent, f32 density) {
    de__fog_snapshot s;
    memset(&s, 0, sizeof s);
    s.entity = 1u;
    s.transform = de_affine_identity();
    s.transform.m[9] = pos.x;
    s.transform.m[10] = pos.y;
    s.transform.m[11] = pos.z;
    s.volume.extent = extent;
    s.volume.density = density;
    s.volume.albedo = (de_vec3){0.5f, 0.75f, 2.0f};
    s.volume.anisotropy = 0.99f;
    s.volume.height_falloff = 0.2f;
    s.volume.noise = 1.5f;
    s.volume.edge_fade = 0.5f;
    return s;
}

TEST(fog, test_fog_volume_extract) {
    de__fog_snapshot src[4];
    src[0] = snap((de_vec3){10.0f, 20.0f, 3.0f}, (de_vec3){4.0f, 6.0f, 8.0f}, 0.1f);
    src[1] = snap((de_vec3){0.0f, 0.0f, 0.0f}, (de_vec3){2.0f, 2.0f, 2.0f}, 0.0f); /* skipped */
    src[2] = snap((de_vec3){0.0f, 0.0f, 5.0f}, (de_vec3){0.0f, 0.0f, 0.0f}, 0.02f); /* global */
    src[3] = snap((de_vec3){1.0f, 1.0f, 1.0f}, (de_vec3){1.0f, 1.0f, 1.0f}, 0.3f);
    de__fog_volume_gpu out[DE__FOG_MAX_VOLUMES];
    const de_vec3 cam = {8.0f, 18.0f, 1.0f};

    ASSERT_EQ(de__fog_pack_volumes(src, 4u, cam, out, DE__FOG_MAX_VOLUMES), 3u);
    /* Box: camera-relative inverse maps the box centre (pos - cam) to the origin. */
    const f32 rel[3] = {2.0f, 2.0f, 2.0f};
    for (u32 r = 0; r < 3u; ++r) {
        const f32 local = out[0].inv[r][0] * rel[0] + out[0].inv[r][1] * rel[1] +
                          out[0].inv[r][2] * rel[2] + out[0].inv[r][3];
        EXPECT_FLOAT_EQ(local, 0.0f, 1e-5f);
    }
    EXPECT_FLOAT_EQ(out[0].extent_density[0], 2.0f, 1e-6f); /* half extents */
    EXPECT_FLOAT_EQ(out[0].extent_density[1], 3.0f, 1e-6f);
    EXPECT_FLOAT_EQ(out[0].extent_density[2], 4.0f, 1e-6f);
    EXPECT_FLOAT_EQ(out[0].extent_density[3], 0.1f, 1e-6f);
    EXPECT_FLOAT_EQ(out[0].albedo_g[3], 0.95f, 1e-6f);     /* anisotropy clamped */
    EXPECT_FLOAT_EQ(out[0].params[0], 0.0f, 1e-6f);        /* no height falloff on a box */
    EXPECT_FLOAT_EQ(out[0].params[1], 1.0f, 1e-6f);        /* noise clamped to 1 */
    EXPECT_FLOAT_EQ(out[0].params[2], 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(out[0].params[3], 2.0f, 1e-5f);        /* base z, camera-relative */
    /* Global: a zero extent, flagged by x < 0, with its height falloff and base. */
    EXPECT_TRUE(out[1].extent_density[0] < 0.0f);
    EXPECT_FLOAT_EQ(out[1].params[0], 0.2f, 1e-6f);
    EXPECT_FLOAT_EQ(out[1].params[3], 4.0f, 1e-5f);
    /* The cap holds; NULL inputs pack nothing. */
    EXPECT_EQ(de__fog_pack_volumes(src, 4u, cam, out, 1u), 1u);
    EXPECT_EQ(de__fog_pack_volumes(NULL, 4u, cam, out, 4u), 0u);
    EXPECT_EQ(de__fog_pack_volumes(src, 4u, cam, NULL, 4u), 0u);
}

TEST(fog, test_fog_slice_distribution) {
    const u32 d = 64u;
    const f32 n = DE__FOG_NEAR, f = DE__FOG_MAX_DISTANCE;
    EXPECT_FLOAT_EQ(de__fog_slice_depth(0.0f, d, n, f), n, 1e-5f);
    EXPECT_FLOAT_EQ(de__fog_slice_depth((f32)d, d, n, f), f, 1e-3f);
    f32 prev = 0.0f;
    for (u32 s = 0; s <= d; ++s) {
        const f32 z = de__fog_slice_depth((f32)s, d, n, f);
        EXPECT_TRUE(z > prev);
        if (s > 1u) /* exponential: each slice deeper than the last */
            EXPECT_TRUE(z - prev > prev - de__fog_slice_depth((f32)s - 2.0f, d, n, f));
        EXPECT_FLOAT_EQ(de__fog_slice_of(z, d, n, f), (f32)s, 1e-3f);
        prev = z;
    }
    EXPECT_TRUE(de__fog_slice_of(0.1f, d, n, f) < 0.0f);
    EXPECT_TRUE(de__fog_slice_of(0.0f, d, n, f) < 0.0f);
    /* Degenerate ranges do not divide by zero. */
    EXPECT_FLOAT_EQ(de__fog_slice_of(3.0f, 0u, n, f), 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(de__fog_slice_depth(3.0f, d, n, n), n, 0.0f);
    /* The grids. */
    const de__fog_grid high = de__fog_grid_for(DE_QUALITY_HIGH);
    EXPECT_EQ(high.w, 160u);
    EXPECT_EQ(high.h, 90u);
    EXPECT_EQ(high.d, 64u);
    EXPECT_EQ(de__fog_grid_for(DE_QUALITY_OFF).d, 0u);
    u32 aw = 0, ah = 0;
    de__fog_atlas_size(high, &aw, &ah);
    EXPECT_EQ(aw, 160u * DE__FOG_ATLAS_COLUMNS);
    EXPECT_EQ(ah, 90u * (64u / DE__FOG_ATLAS_COLUMNS));
}

static u32 fog_passes(const de_rg *g) {
    u32 n = 0;
    for (u32 i = 0; i < g->pass_count; ++i)
        if (strncmp(g->passes[i]->name, "fog.", 4) == 0)
            ++n;
    return n;
}

TEST(fog, test_fog_off_culls_passes) {
    de_tracking_allocator track;
    const de_allocator alloc = de_tracking_allocator_init(&track, NULL, DE_MEM_RENDER);
    de_gpu_device *dev = NULL;
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL, .allocator = &alloc},
                                   &dev),
              DE_OK);
    de_arena *arena = NULL;
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "fog_test"}, &arena), DE_OK);
    de_rg *g = NULL;
    ASSERT_EQ(de__rg_create(&(de__rg_desc){.allocator = &alloc, .name = "fog"}, &g), DE_OK);
    de__fog *fog = NULL;
    ASSERT_EQ(de__fog_create(dev, &alloc, &fog), DE_OK);

    const u32 w = 64u, h = 36u;
    de_gpu_texture depth = de_gpu_texture_create(
        dev, &(de_gpu_texture_desc){.format = DE_FMT_D32_FLOAT, .width = w, .height = h,
                                    .usage = DE_USAGE_DEPTH | DE_USAGE_SAMPLED,
                                    .name = "depth"});
    de_gpu_texture hdr = de_gpu_texture_create(
        dev, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT, .width = w, .height = h,
                                    .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED,
                                    .name = "hdr"});
    ASSERT_TRUE(DE_HANDLE_VALID(depth) && DE_HANDLE_VALID(hdr));

    de__fog_snapshot vol = snap((de_vec3){0.0f, 0.0f, 0.0f}, (de_vec3){0.0f, 0.0f, 0.0f}, 0.05f);
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_HIGH);
    de__fog_inputs in = {.settings = &settings,
                         .volumes = &vol,
                         .volume_count = 1u,
                         .width = w,
                         .height = h,
                         .view_matrix = de_m4_identity(),
                         .proj = de_m4_identity(),
                         .prev_view_proj = de_m4_identity(),
                         .history_ok = true,
                         .pre_exposure = 1.0f};
    de_tracking_stats warm = {0};
    for (u32 frame = 0; frame < 4u; ++frame) {
        settings.volumetrics = frame == 0u ? DE_QUALITY_OFF : DE_QUALITY_HIGH;
        ASSERT_EQ(de__rg_begin(g, arena), DE_OK);
        in.frame_index = frame;
        in.depth = de_rg_import_texture(g, depth, DE_ACCESS_DEPTH_READ);
        in.target = de_rg_import_texture(g, hdr, DE_ACCESS_SHADER_READ);
        de__fog_output out;
        const u32 added = de__fog_add_passes(fog, g, arena, &in, &out);
        if (frame == 0u) {
            EXPECT_EQ(added, 0u);
            EXPECT_EQ(fog_passes(g), 0u);
            EXPECT_FALSE(DE_HANDLE_VALID(out.integrated));
        } else {
            EXPECT_EQ(added, 4u);
            EXPECT_EQ(fog_passes(g), 4u);
            EXPECT_TRUE(DE_HANDLE_VALID(out.integrated));
            EXPECT_EQ(g->error_count, 0u);
        }
        de__rg_reset(g);
        de_arena_reset(arena);
        if (frame == 2u)
            warm = de_tracking_allocator_stats(&track);
        if (frame == 3u) /* no heap allocation once the view's volumes exist */
            EXPECT_EQ(de_tracking_allocator_stats(&track).alloc_count, warm.alloc_count);
    }
    /* No volume with a density: no pass either. */
    vol.volume.density = 0.0f;
    ASSERT_EQ(de__rg_begin(g, arena), DE_OK);
    in.depth = de_rg_import_texture(g, depth, DE_ACCESS_DEPTH_READ);
    in.target = de_rg_import_texture(g, hdr, DE_ACCESS_SHADER_READ);
    EXPECT_EQ(de__fog_add_passes(fog, g, arena, &in, NULL), 0u);
    de__rg_reset(g);
    de_arena_reset(arena);

    de__fog_destroy(fog);
    de__rg_destroy(g);
    de_gpu_texture_destroy(dev, depth);
    de_gpu_texture_destroy(dev, hdr);
    de_gpu_device_destroy(dev);
    de_arena_destroy(arena);
    EXPECT_EQ(de_tracking_allocator_stats(&track).blocks_live, 0u);
}
