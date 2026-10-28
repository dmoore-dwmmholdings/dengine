#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_ecs.h>
#include <dengine/de_reflect.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_render.h>

#include "gpu/gpu_backend.h"
#include "render/components_internal.h"
#include "render/formats/de_cooked_material.h"
#include "render/formats/de_cooked_mesh.h"
#include "render/graph/rg_internal.h"
#include "render/lighting/lighting.h"
#include "render/loaders/render_assets.h"
#include "render/materials/brdf_luts.h"
#include "render/materials/material_models.h"
#include "render/passes/sss/sss.h"
#include "render/passes/vis/gbuffer.h"
#include "render/passes/vis/vis_stages.h"
#include "render/renderer_internal.h"
#include "render/views_internal.h"
#include "render/probes/probes_internal.h"
#include "render/scene/gpu_scene.h"
#include "render/world/render_world.h"

#if DE_HAS_GPU_VULKAN
#include "gpu/vulkan/vk_internal.h"
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
#include "gpu/d3d12/d3d12_internal.h"
#endif
#include "render_d3d12_test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lighting_fixture.h"
#include "render/shadows/shadows_internal.h"

static u32 random_u32(u32 *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}
TEST(shadow_atlas, atlas_alloc_no_overlap_property) {
    de__atlas_alloc a;
    ASSERT_EQ(de__atlas_alloc_init(&a, NULL, 4096), DE_OK);
    EXPECT_EQ(a.node_count, 87381);
    de__atlas_tile tiles[128] = {0};
    u32 seed = 0x336u;
    for (u32 iteration = 0; iteration < 20000; ++iteration) {
        const u32 slot = random_u32(&seed) % 128u;
        if (tiles[slot].size) {
            ASSERT_TRUE(de__atlas_alloc_free(&a, tiles[slot]));
            EXPECT_FALSE(de__atlas_alloc_free(&a, tiles[slot]));
            tiles[slot] = (de__atlas_tile){0};
        } else {
            const u32 size = 16u << (random_u32(&seed) % 8u);
            if (de__atlas_alloc_take(&a, size, &tiles[slot])) {
                const de__atlas_tile t = tiles[slot];
                EXPECT_EQ(t.size, size);
                EXPECT_EQ(t.x % size, 0);
                EXPECT_EQ(t.y % size, 0);
                EXPECT_TRUE(t.x + size <= a.size && t.y + size <= a.size);
                for (u32 j = 0; j < 128; ++j) {
                    const de__atlas_tile b = tiles[j];
                    if (j != slot && b.size)
                        ASSERT_TRUE(t.x + t.size <= b.x || b.x + b.size <= t.x ||
                                    t.y + t.size <= b.y || b.y + b.size <= t.y);
                }
            }
        }
        u32 area = 0;
        for (u32 i = 0; i < 128; ++i)
            area += tiles[i].size * tiles[i].size;
        ASSERT_EQ(a.occupied, area);
    }
    for (u32 i = 0; i < 128; ++i)
        if (tiles[i].size)
            ASSERT_TRUE(de__atlas_alloc_free(&a, tiles[i]));
    EXPECT_EQ(a.occupied, 0);
    EXPECT_EQ(a.nodes[0], 0);
    de__atlas_tile t[4];
    for (u32 i = 0; i < 4; ++i)
        ASSERT_TRUE(de__atlas_alloc_take(&a, 2048, &t[i]));
    de__atlas_tile extra;
    EXPECT_FALSE(de__atlas_alloc_take(&a, 16, &extra));
    for (u32 i = 0; i < 4; ++i)
        ASSERT_TRUE(de__atlas_alloc_free(&a, t[i]));
    ASSERT_TRUE(de__atlas_alloc_take(&a, 2048, &extra));
    EXPECT_EQ(memcmp(&extra, &t[0], sizeof extra), 0);
    de__atlas_alloc_destroy(&a);
}
TEST(shadow_atlas, atlas_alloc_bounds_and_handles) {
    de__atlas_alloc a;
    EXPECT_NE(de__atlas_alloc_init(&a, NULL, 4095), DE_OK);
    ASSERT_EQ(de__atlas_alloc_init(&a, NULL, 8192), DE_OK);
    EXPECT_EQ(a.node_count, 349525);
    de__atlas_tile t;
    EXPECT_FALSE(de__atlas_alloc_take(&a, 8, &t));
    EXPECT_FALSE(de__atlas_alloc_take(&a, 17, &t));
    EXPECT_FALSE(de__atlas_alloc_take(&a, 4096, &t));
    ASSERT_TRUE(de__atlas_alloc_take(&a, 16, &t));
    de__atlas_tile bad = t;
    ++bad.x;
    EXPECT_FALSE(de__atlas_alloc_free(&a, bad));
    EXPECT_EQ(a.occupied, 256);
    ASSERT_TRUE(de__atlas_alloc_free(&a, t));
    de__atlas_alloc_destroy(&a);
}
TEST(shadow_atlas, derived_range_matches_physical_light_units) {
    DeLight l = {.color = {.25f, .5f, 1}, .intensity = 100, .spot_outer = .6f, .area_size = {4, 7}};
    for (u32 kind = DE_LIGHT_POINT; kind <= DE_LIGHT_DISK; ++kind) {
        l.kind = (de_light_kind)kind;
        double intensity = kind == DE_LIGHT_POINT  ? 100 / (4 * PI_D)
                           : kind == DE_LIGHT_SPOT ? 100 / (2 * PI_D * (1 - cos(.6)))
                           : kind == DE_LIGHT_RECT ? 100 * 4 * 7
                                                   : 100 * PI_D * 4;
        EXPECT_FLOAT_EQ(de__atlas_light_range(&l, .05f), sqrt(intensity / .05), 2e-4);
        EXPECT_FLOAT_EQ(de__atlas_light_range(&l, .2f), sqrt(intensity / .2), 2e-4);
        l.range = 9;
        EXPECT_TRUE(de__atlas_light_range(&l, .0001f) == 9);
        l.range = 0;
    }
    l.kind = DE_LIGHT_DISK;
    f32 range = de__atlas_light_range(&l, .05f);
    l.area_size.y = 100;
    EXPECT_TRUE(de__atlas_light_range(&l, .05f) == range);
}

typedef struct atlas_read {
    de_rg_resource depth, mirror, readback;
    u32 size;
} atlas_read;
static void atlas_read_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const atlas_read *p = user;
    de_gpu_buffer rb = de_rg_buffer(pass, p->readback);
    de_gpu_cmd_copy_texture_to_buffer(cmd, rb, 0, p->size * 4u, de_rg_texture(pass, p->depth), 0,
                                      0);
    de_gpu_cmd_copy_texture_to_buffer(cmd, rb, (u64)p->size * p->size * 4u, p->size * 4u,
                                      de_rg_texture(pass, p->mirror), 0, 0);
}
static de_result atlas_try_begin(lx *f, de_quality quality, de__atlas_info *info) {
    const u64 frame = f->frames;
    world_begin(&f->w);
    ASSERT_EQ(de__gpu_scene_update(f->gs, &f->w.f), DE_OK);
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
    ASSERT_EQ(de__gpu_scene_add_passes(f->gs, f->g), DE_OK);
    de__rw_view camera = {.offset = f->eye, .camera = {.near_z = .1f}};
    de_render_settings settings = {.shadows = quality};
    de__view_frame vf = {.view = {.id = 1},
                         .graph = f->g,
                         .world = &f->w.f,
                         .camera = &camera,
                         .settings = &settings,
                         .frame_index = frame,
                         .width = f->width,
                         .height = f->height};
    de__vis_context vc = {.gpu_scene = f->gs, .frames_completed = frame, .layer_mask = 1};
    de_result result = de__shadow_atlas_build(f->atlas, &vf, &vc);
    if (result == DE_OK)
        ASSERT_TRUE(de__shadow_atlas_get(f->atlas, vf.view, info));
    return result;
}
static de__atlas_info atlas_begin(lx *f, de_quality quality) {
    de__atlas_info info;
    ASSERT_EQ(atlas_try_begin(f, quality, &info), DE_OK);
    return info;
}
static const f32 *atlas_end_read_result(lx *f, const de__atlas_info *info, de_view view, u64 frame,
                                        bool reported_success) {
    const u64 bytes = (u64)info->size * info->size * 8u;
    if (!f->rb.id || f->rb_size < bytes) {
        if (f->rb.id)
            de_gpu_buffer_destroy(f->dev, f->rb);
        f->rb = de_gpu_buffer_create(
            f->dev, &(de_gpu_buffer_desc){.size = bytes,
                                          .usage = DE_USAGE_COPY_DST,
                                          .memory = DE_MEM_READBACK,
                                          .name = "atlas canonical/mirror readback"});
        ASSERT_TRUE(f->rb.id);
        f->rb_size = bytes;
    }
    atlas_read *read = DE_ARENA_PUSH(f->arena, atlas_read, 1);
    ASSERT_TRUE(read);
    *read = (atlas_read){.depth = de_rg_import_texture(f->g, info->depth, DE_ACCESS_COPY_SRC),
                         .mirror = de_rg_import_texture(f->g, info->mirror, DE_ACCESS_SHADER_READ),
                         .readback = de_rg_import_buffer(f->g, f->rb, DE_ACCESS_COPY_DST),
                         .size = info->size};
    de_rg_pass *pass =
        de_rg_add_pass(f->g, "atlas.acceptance.read", DE_QUEUE_GRAPHICS, atlas_read_execute, read);
    de_rg_pass_side_effect(pass);
    de_rg_read(pass, read->depth, DE_ACCESS_COPY_SRC);
    de_rg_read(pass, read->mirror, DE_ACCESS_COPY_SRC);
    de_rg_write(pass, read->readback, DE_ACCESS_COPY_DST);
    de_rg_export(f->g, read->mirror, DE_ACCESS_SHADER_READ);
    ASSERT_EQ(de__rg_finalize(f->g), DE_OK);
    ASSERT_EQ(de__rg_compile(f->g), DE_OK);
    const u64 sequence = f->frames++;
    ASSERT_EQ(de__rg_execute(f->g, &(de__rg_execute_desc){.frame_index = frame,
                                                          .frame_fence = f->fence,
                                                          .frame_value = sequence + 1}),
              DE_OK);
    de__shadow_atlas_complete(f->atlas, view, f->g, true, reported_success);
    de__gpu_scene_frame_end(f->gs, f->g, true);
    ASSERT_EQ(de_gpu_fence_wait(f->dev, f->fence, sequence + 1, FIRST_FRAME_WAIT_NS), DE_OK);
    const f32 *pixels = de_gpu_buffer_map(f->dev, f->rb);
    ASSERT_TRUE(pixels);
    return pixels;
}
static const f32 *atlas_end_read_view(lx *f, const de__atlas_info *info, de_view view, u64 frame) {
    return atlas_end_read_result(f, info, view, frame, true);
}
static const f32 *atlas_end_read(lx *f, const de__atlas_info *info) {
    return atlas_end_read_view(f, info, (de_view){.id = 1}, f->frames);
}
static void tile_rect(const de__atlas_info *info, u32 light, u32 face, u32 rect[4]) {
    const de__atlas_gpu *g = &info->data[1u + light * 6u + face];
    rect[0] = (u32)(g->tile[0] * info->size);
    rect[1] = (u32)(g->tile[1] * info->size);
    rect[2] = rect[3] = (u32)(g->tile[2] * info->size);
}
static u32 tile_nonzero(const de__atlas_info *info, const f32 *pixels, u32 light) {
    u32 r[4], covered = 0;
    tile_rect(info, light, 0, r);
    for (u32 y = r[1]; y < r[1] + r[3]; ++y)
        for (u32 x = r[0]; x < r[0] + r[2]; ++x) {
            u32 index = y * info->size + x;
            EXPECT_TRUE(isfinite(pixels[index]));
            EXPECT_EQ(memcmp(&pixels[index], &pixels[index + info->size * info->size], 4), 0);
            covered += pixels[index] > 0;
        }
    return covered;
}
typedef struct atlas_query {
    f32 position[4], normal[4];
    u32 index[4];
} atlas_query;
typedef struct atlas_query_run {
    de_gpu_device *dev;
    de__engine_shader_code code;
    de__vis_kernel kernel;
    de_gpu_buffer queries, readback;
    de_rg_resource metadata, input, output, rb;
    u32 count;
} atlas_query_run;
static void atlas_query_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const atlas_query_run *p = user;
    de_gpu_device *dev = p->dev;
    const u32 pc[4] = {de_gpu_buffer_bindless(dev, de_rg_buffer(pass, p->metadata)),
                       de_gpu_buffer_bindless(dev, de_rg_buffer(pass, p->input)),
                       de_gpu_buffer_bindless(dev, de_rg_buffer(pass, p->output)), p->count};
    de_gpu_cmd_set_pipeline(cmd, p->kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, (p->count + 63u) / 64u, 1, 1);
}
static void atlas_query_copy(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const atlas_query_run *p = user;
    de_gpu_cmd_copy_buffer(cmd, de_rg_buffer(pass, p->rb), 0, de_rg_buffer(pass, p->output), 0,
                           (u64)p->count * 16u);
}
static void atlas_query_add(lx *f, atlas_query_run *p, const atlas_query *queries, u32 count) {
    memset(p, 0, sizeof *p);
    p->count = count;
    p->dev = f->dev;
    const char *entries[] = {"cs_atlas_query"};
    char why[768];
    ASSERT_EQ(de__engine_shader_load(f->dev, &f->alloc, "shadow_atlas", 0, entries, 1, &p->code,
                                     why, sizeof why),
              DE_OK);
    const u32 threads[3] = {64, 1, 1};
    ASSERT_EQ(de__vis_kernel_create(f->vis, &p->code, 0, "atlas production query", threads, 16,
                                    &p->kernel),
              DE_OK);
    p->queries = upload_buf(f->dev, queries, (u64)count * sizeof *queries);
    p->readback =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)count * 16u,
                                                           .usage = DE_USAGE_COPY_DST,
                                                           .memory = DE_MEM_READBACK,
                                                           .name = "atlas visibility readback"});
    ASSERT_TRUE(p->readback.id);
    de__lighting_inputs in = {0};
    de__shadow_atlas_publish(f->atlas, (de_view){.id = 1}, f->g, f->frames, &in);
    p->metadata = in.atlas_records;
    p->input = de_rg_import_buffer(f->g, p->queries, DE_ACCESS_SHADER_READ);
    p->output = de_rg_create_buffer(
        f->g, &(de_gpu_buffer_desc){.size = (u64)count * 16u,
                                    .stride = 16,
                                    .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                    .name = "atlas visibility results"});
    p->rb = de_rg_import_buffer(f->g, p->readback, DE_ACCESS_COPY_DST);
    de_rg_pass *pass =
        de_rg_add_pass(f->g, "atlas.acceptance.query", DE_QUEUE_GRAPHICS, atlas_query_execute, p);
    de_rg_read(pass, p->metadata, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, p->input, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, in.atlas_texture, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, p->output, DE_ACCESS_SHADER_WRITE);
    pass = de_rg_add_pass(f->g, "atlas.acceptance.query_readback", DE_QUEUE_GRAPHICS,
                          atlas_query_copy, p);
    de_rg_pass_side_effect(pass);
    de_rg_read(pass, p->output, DE_ACCESS_COPY_SRC);
    de_rg_write(pass, p->rb, DE_ACCESS_COPY_DST);
}
static void atlas_query_destroy(lx *f, atlas_query_run *p) {
    de_gpu_pipeline_destroy(f->dev, p->kernel.pipeline);
    de__engine_shader_code_free(&p->code);
    de_gpu_buffer_destroy(f->dev, p->queries);
    de_gpu_buffer_destroy(f->dev, p->readback);
}
static void cached_removal(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    u32 mat = material_add(.5f, .6f, 0, .5f), mesh = lx_mesh_plane(f, 1, 1, 2);
    world_set(&f->w, 0, mesh, at(-4, 0, 0), mat);
    world_set(&f->w, 1, mesh, at(4, 0, 0), mat);
    f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
    f->w.f.instances.flags[1] |= DE__RW_INST_CAST_SHADOWS;
    const DeLight light = {.kind = DE_LIGHT_SPOT,
                           .color = {1, 1, 1},
                           .intensity = 100,
                           .range = 6,
                           .spot_inner = .3f,
                           .spot_outer = .5f,
                           .cast_shadows = true};
    world_light(&f->w, 0, &light, at(-4, 0, 4));
    world_light(&f->w, 1, &light, at(4, 0, 4));
    /* A non-casting receiver survives removal: preserve stale-depth erasure coverage. */
    world_set(&f->w, 2, mesh, at(-4, 0, 0), mat);
    de__atlas_info info = atlas_begin(f, DE_QUALITY_LOW);
    ASSERT_EQ(info.rendered, 2);
    const f32 *pixels = atlas_end_read(f, &info);
    ASSERT_GT(tile_nonzero(&info, pixels, 0), 100u);
    ASSERT_GT(tile_nonzero(&info, pixels, 1), 100u);
    u32 r[4];
    tile_rect(&info, 1, 0, r);
    f32 *neighbor = zalloc((size_t)r[2] * r[3] * 4);
    for (u32 y = 0; y < r[3]; ++y)
        memcpy(neighbor + y * r[2], pixels + (r[1] + y) * info.size + r[0], r[2] * 4u);
    info = atlas_begin(f, DE_QUALITY_LOW);
    EXPECT_EQ(info.rendered, 0);
    pixels = atlas_end_read(f, &info);
    // Removal must erase previous nonzero reverse-Z depth, not merely omit caster draws.
    f->w.f.instances.entity[0] = 0;
    f->w.f.removed_instance_count = 1;
    f->w.f.removed_instances[0] = 0;
    info = atlas_begin(f, DE_QUALITY_LOW);
    EXPECT_EQ(info.rendered, 1);
    pixels = atlas_end_read(f, &info);
    EXPECT_EQ(tile_nonzero(&info, pixels, 0), 0);
    ASSERT_GT(tile_nonzero(&info, pixels, 1), 100u);
    for (u32 y = 0; y < r[3]; ++y)
        EXPECT_EQ(memcmp(neighbor + y * r[2], pixels + (r[1] + y) * info.size + r[0], r[2] * 4u),
                  0);
    free(neighbor);
    EXPECT_EQ(validation_errors(f->dev), 0);
}
static void static_cache(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    const DeLight light = {.kind = DE_LIGHT_SPOT,
                           .color = {1, 1, 1},
                           .intensity = 100,
                           .range = 6,
                           .spot_outer = .5f,
                           .cast_shadows = true};
    u32 mat = material_add(.5f, .6f, 0, .5f), mesh = lx_mesh_plane(f, 1, 1, 2);
    world_set(&f->w, 0, mesh, at(0, 0, 0), mat);
    f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
    for (u32 i = 0; i < 16; ++i)
        world_light(&f->w, i, &light, at(0, 0, 4));
    for (u32 frame = 0; frame < 4; ++frame) {
        de__atlas_info info = atlas_begin(f, DE_QUALITY_LOW);
        EXPECT_EQ(info.lights, 16);
        EXPECT_EQ(info.rendered, frame < 2 ? 8 : 0);
        for (u32 i = 0; i < 16; ++i) {
            EXPECT_EQ(info.data[1 + i * 6].identity[0], 5000u + i);
            EXPECT_EQ(info.data[1 + i * 6].identity[3], frame == 0 && i >= 8 ? 0 : 1);
        }
        const f32 *pixels = atlas_end_read(f, &info);
        for (u32 i = 0; i < 16; ++i)
            if (info.data[1 + i * 6].identity[3])
                EXPECT_GT(tile_nonzero(&info, pixels, i), 100u);
    }
    EXPECT_EQ(validation_errors(f->dev), 0);
}
static void local_limit(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    ASSERT_TRUE(cvar_set("r.shadow_local_max", "2"));
    f->width = 65;
    f->height = 43;
    f->shadow_quality = DE_QUALITY_LOW;
    u32 mat = material_add(.5f, .6f, 0, .5f), mesh = lx_mesh_plane(f, 1, 1, 2);
    world_set(&f->w, 0, mesh, at(0, 0, 0), mat);
    f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
    const u64 ids[3] = {5012, 5003, 5008};
    for (u32 i = 0; i < 3; ++i) {
        DeLight l = {.kind = DE_LIGHT_SPOT,
                     .color = {.5f + .1f * i, 1, 1},
                     .intensity = 100,
                     .range = 6,
                     .spot_inner = .3f,
                     .spot_outer = .5f,
                     .cast_shadows = true};
        world_light(&f->w, i, &l, at(0, 0, 4));
        f->w.f.lights.entity[i] = ids[i];
    }
    for (u32 frame = 0; frame < 3; ++frame) {
        ASSERT_TRUE(lx_frame(f));
        de__atlas_info info;
        ASSERT_TRUE(de__shadow_atlas_get(f->atlas, (de_view){.id = 1}, &info));
        EXPECT_EQ(info.lights, 2);
        EXPECT_EQ(info.data[1].identity[0], 5003);
        EXPECT_EQ(info.data[7].identity[0], 5008);
        EXPECT_EQ(info.rendered, frame == 0 ? 2 : 0);
        EXPECT_EQ(f->res.counters[DE__LIGHTING_COUNTER_LOCAL / 4], 3);
        u32 indices[3] = {0};
        for (u32 j = 0; j < 3; ++j) {
            u32 flags;
            memcpy(&flags, &f->res.lights[j].axis[3], 4);
            const f32 candela = 100.f / (2.f * (f32)PI_D * (1 - cosf(.5f)));
            i32 source = (i32)lroundf((f->res.lights[j].color[0] / candela - .5f) * 10.f);
            ASSERT_TRUE(source >= 0 && source < 3);
            indices[source] = (flags >> 8) & 255u;
        }
        EXPECT_EQ(indices[0], 0);
        EXPECT_EQ(indices[1], 1);
        EXPECT_EQ(indices[2], 7);
    }
    // The actual producer's excluded index must stay unshadowed even at an occluded receiver.
    de_gpu_buffer_destroy(f->dev, f->rb);
    f->rb = (de_gpu_buffer){0};
    de__atlas_info info = atlas_begin(f, DE_QUALITY_LOW);
    atlas_query queries[3] = {0};
    for (u32 i = 0; i < 3; ++i) {
        queries[i].position[2] = -1;
        queries[i].normal[2] = 1;
        queries[i].index[0] = i == 0 ? 0 : i == 1 ? 1 : 7;
    }
    atlas_query_run run;
    atlas_query_add(f, &run, queries, 3);
    atlas_end_read(f, &info);
    const f32 *values = de_gpu_buffer_map(f->dev, run.readback);
    ASSERT_TRUE(values);
    EXPECT_TRUE(values[0] == 1);
    EXPECT_TRUE(values[4] == 0);
    EXPECT_TRUE(values[8] == 0);
    atlas_query_destroy(f, &run);
    ASSERT_TRUE(cvar_set("r.shadow_local_max", "32"));
    EXPECT_EQ(validation_errors(f->dev), 0);
}
static void invalidation(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    u32 materials[2] = {material_add(.5f, .6f, 0, .5f), material_add(.5f, .6f, 0, .5f)},
        mesh = lx_mesh_plane(f, 1, 1, 2), small_mesh = lx_mesh_plane(f, 1, 1, 1);
    DeLight light = {.kind = DE_LIGHT_SPOT,
                     .color = {1, 1, 1},
                     .intensity = 100,
                     .range = 6,
                     .spot_inner = .3f,
                     .spot_outer = .5f,
                     .cast_shadows = true};
    for (u32 i = 0; i < 2; ++i) {
        world_set(&f->w, i, mesh, at(i ? 4.f : -4.f, 0, 0), materials[i]);
        f->w.f.instances.flags[i] |= DE__RW_INST_CAST_SHADOWS;
        world_light(&f->w, i, &light, at(i ? 4.f : -4.f, 0, 4));
    }
    de__atlas_info info = atlas_begin(f, DE_QUALITY_LOW);
    const f32 *pixels = atlas_end_read(f, &info);
    u32 rect[4];
    tile_rect(&info, 1, 0, rect);
    f32 *neighbor = zalloc((size_t)rect[2] * rect[3] * 4);
    for (u32 y = 0; y < rect[3]; ++y)
        memcpy(neighbor + y * rect[2], pixels + (rect[1] + y) * info.size + rect[0], rect[2] * 4u);
    for (u32 phase = 0; phase < 16; ++phase) {
        if (phase == 0)
            world_set(&f->w, 0, mesh, at(-3.5f, 0, 0), materials[0]);
        if (phase == 1)
            f->w.f.instances.flags[0] &= (u8)~DE__RW_INST_CAST_SHADOWS;
        if (phase == 2)
            f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
        if (phase == 3) {
            g_mat[0].alpha_mode = DE_COOKED_ALPHA_MASK;
            g_mat[0].standard.alpha_mode = DE_COOKED_ALPHA_MASK;
            g_mat[0].standard.base_color.w = 0;
            g_mat[0].standard.alpha_cutoff = .5f;
        }
        if (phase == 4) {
            g_mat[0].alpha_mode = DE_COOKED_ALPHA_OPAQUE;
            g_mat[0].standard.alpha_mode = DE_COOKED_ALPHA_OPAQUE;
            g_mat[0].standard.base_color.w = 1;
        }
        if (phase == 5)
            world_set(&f->w, 0, small_mesh, at(-3.5f, 0, 0), materials[0]);
        if (phase == 0 || phase == 5)
            f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
        if (phase <= 2)
            f->w.touch_all = true;
        if (phase == 6) {
            light.range = 5;
            world_light(&f->w, 0, &light, at(-4, 0, 4));
        }
        if (phase == 7) {
            light.spot_outer = .4f;
            world_light(&f->w, 0, &light, at(-4, 0, 4));
        }
        if (phase == 8) {
            light.kind = DE_LIGHT_RECT;
            light.area_size = (de_vec2){1, 1};
            world_light(&f->w, 0, &light, at(-4, 0, 4));
        }
        if (phase == 9) {
            light.kind = DE_LIGHT_DISK;
            world_light(&f->w, 0, &light, at(-4, 0, 4));
        }
        if (phase == 10) {
            light.source_radius = .2f;
            world_light(&f->w, 0, &light, at(-4, 0, 4));
        }
        if (phase == 11)
            f->w.f.origin = (de_dvec3){1000000, 1000000, 0};
        if (phase == 13)
            ASSERT_TRUE(de__shadow_atlas_dynamic(f->atlas, (de_view){.id = 1}, 5000, true));
        if (phase == 15)
            ASSERT_TRUE(de__shadow_atlas_dynamic(f->atlas, (de_view){.id = 1}, 5000, false));
        info = atlas_begin(f, phase >= 12 ? DE_QUALITY_MEDIUM : DE_QUALITY_LOW);
        EXPECT_EQ(info.rendered, phase == 10 || phase == 15   ? 0
                                 : phase == 11 || phase == 12 ? 2
                                                              : 1);
        pixels = atlas_end_read(f, &info);
        if (phase == 1 || phase == 3)
            EXPECT_EQ(tile_nonzero(&info, pixels, 0), 0);
        else
            EXPECT_GT(tile_nonzero(&info, pixels, 0), 4u);
        if (phase < 12)
            for (u32 y = 0; y < rect[3]; ++y)
                EXPECT_EQ(memcmp(neighbor + y * rect[2],
                                 pixels + (rect[1] + y) * info.size + rect[0], rect[2] * 4u),
                          0);
    }
    free(neighbor);
    EXPECT_EQ(validation_errors(f->dev), 0);
}
static void inactive_gap(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    const u32 mat = material_add(.5f, .6f, 0, .5f), mesh = lx_mesh_plane(f, 1, 1, 2);
    const DeLight light = {.kind = DE_LIGHT_SPOT,
                           .color = {1, 1, 1},
                           .intensity = 100,
                           .range = 6,
                           .spot_outer = .5f,
                           .cast_shadows = true};
    for (u32 i = 0; i < 2; ++i) {
        world_set(&f->w, i, mesh, at(i ? 4.f : -4.f, 0, 0), mat);
        f->w.f.instances.flags[i] |= DE__RW_INST_CAST_SHADOWS;
        world_light(&f->w, i, &light, at(i ? 4.f : -4.f, 0, 4));
    }
    /* This receiver survives caster removal, requiring the stale tile to be erased. */
    world_set(&f->w, 2, mesh, at(-4, 0, 0), mat);
    de__atlas_info info = atlas_begin(f, DE_QUALITY_LOW);
    const f32 *pixels = atlas_end_read(f, &info);
    EXPECT_GT(tile_nonzero(&info, pixels, 0), 100u);
    u32 rect[4];
    tile_rect(&info, 1, 0, rect);
    f32 *neighbor = zalloc((size_t)rect[2] * rect[3] * 4);
    for (u32 y = 0; y < rect[3]; ++y)
        memcpy(neighbor + y * rect[2], pixels + (rect[1] + y) * info.size + rect[0], rect[2] * 4u);
    f->w.f.instances.entity[0] = 0;
    f->w.f.removed_instance_count = 1;
    f->w.f.removed_instances[0] = 0;
    info = atlas_begin(f, DE_QUALITY_OFF);
    EXPECT_EQ(info.rendered, 0);
    atlas_end_read(f, &info);
    f->w.f.removed_instance_count = 0;
    info = atlas_begin(f, DE_QUALITY_OFF);
    EXPECT_EQ(info.rendered, 0);
    atlas_end_read(f, &info);
    info = atlas_begin(f, DE_QUALITY_LOW);
    EXPECT_EQ(info.rendered, 1);
    pixels = atlas_end_read(f, &info);
    EXPECT_EQ(tile_nonzero(&info, pixels, 0), 0);
    EXPECT_GT(tile_nonzero(&info, pixels, 1), 100u);
    for (u32 y = 0; y < rect[3]; ++y)
        EXPECT_EQ(memcmp(neighbor + y * rect[2], pixels + (rect[1] + y) * info.size + rect[0],
                         rect[2] * 4u),
                  0);
    free(neighbor);
    EXPECT_EQ(validation_errors(f->dev), 0);
}
static const de__gpu_backend_vtable *atlas_original_vt;
static bool atlas_fail_map;
static de_result atlas_map_failure(de_gpu_device *dev, de__gpu_buffer *buffer,
                                   const de_gpu_buffer_desc *desc) {
    de_result result = atlas_original_vt->buffer_create(dev, buffer, desc);
    if (result == DE_OK && atlas_fail_map && desc->name &&
        strcmp(desc->name, "shadow.atlas_metadata") == 0) {
        atlas_fail_map = false;
        buffer->mapped = NULL;
    }
    return result;
}
static void lifecycle(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    u32 mat = material_add(.5f, .6f, 0, .5f), mesh = lx_mesh_plane(f, 1, 1, 2);
    world_set(&f->w, 0, mesh, at(0, 0, 0), mat);
    f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
    DeLight light = {.kind = DE_LIGHT_SPOT,
                     .color = {1, 1, 1},
                     .intensity = 100,
                     .range = 6,
                     .spot_outer = .5f,
                     .cast_shadows = true};
    world_light(&f->w, 0, &light, at(0, 0, 4));
    atlas_original_vt = f->dev->vt;
    de__gpu_backend_vtable failing = *atlas_original_vt;
    failing.buffer_create = atlas_map_failure;
    atlas_fail_map = true;
    f->dev->vt = &failing;
    de__atlas_info info;
    de_result result = atlas_try_begin(f, DE_QUALITY_LOW, &info);
    f->dev->vt = atlas_original_vt;
    EXPECT_EQ(result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_FALSE(atlas_fail_map);
    de_error_clear();
    de__gpu_scene_frame_end(f->gs, f->g, false);
    info = atlas_begin(f, DE_QUALITY_LOW);
    EXPECT_EQ(info.rendered, 1);
    // An unexecuted producer abort releases all reservations and can be retried in this frame.
    de__shadow_atlas_complete(f->atlas, (de_view){.id = 1}, f->g, false, false);
    de__gpu_scene_frame_end(f->gs, f->g, false);
    de__atlas_info aborted;
    ASSERT_TRUE(de__shadow_atlas_get(f->atlas, (de_view){.id = 1}, &aborted));
    EXPECT_FALSE(aborted.ready);
    EXPECT_EQ(aborted.occupied, 0);
    info = atlas_begin(f, DE_QUALITY_LOW);
    EXPECT_EQ(info.rendered, 1);
    const f32 *pixels = atlas_end_read(f, &info);
    EXPECT_GT(tile_nonzero(&info, pixels, 0), 100u);
    // Ring0 still belongs to attempted frame0 when completed-frame is artificially held at0.
    de__rw_view camera = {.offset = f->eye};
    de_render_settings settings = {.shadows = DE_QUALITY_LOW};
    de__view_frame vf = {.view = {.id = 1},
                         .graph = f->g,
                         .world = &f->w.f,
                         .camera = &camera,
                         .settings = &settings,
                         .frame_index = 4};
    de__vis_context vc = {.gpu_scene = f->gs, .frames_completed = 0, .layer_mask = 1};
    EXPECT_EQ(de__shadow_atlas_build(f->atlas, &vf, &vc), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    de__atlas_info preserved;
    ASSERT_TRUE(de__shadow_atlas_get(f->atlas, (de_view){.id = 1}, &preserved));
    EXPECT_EQ(memcmp(info.data, preserved.data, sizeof info.data), 0);
    info = atlas_begin(f, DE_QUALITY_LOW);
    EXPECT_EQ(info.rendered, 0);
    atlas_end_read(f, &info);
    // Report failure after actual GPU submission: no partial candidate may remain cache-valid.
    light.range = 5;
    world_light(&f->w, 0, &light, at(0, 0, 4));
    info = atlas_begin(f, DE_QUALITY_LOW);
    EXPECT_EQ(info.rendered, 1);
    atlas_end_read_result(f, &info, (de_view){.id = 1}, f->frames, false);
    ASSERT_TRUE(de__shadow_atlas_get(f->atlas, (de_view){.id = 1}, &preserved));
    EXPECT_FALSE(preserved.ready);
    EXPECT_EQ(preserved.occupied, 0);
    EXPECT_EQ(preserved.rendered, 0);
    info = atlas_begin(f, DE_QUALITY_LOW);
    EXPECT_EQ(info.rendered, 1);
    pixels = atlas_end_read(f, &info);
    EXPECT_GT(tile_nonzero(&info, pixels, 0), 100u);
    EXPECT_EQ(validation_errors(f->dev), 0);
}
static void budget_movers(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    /* Keep scheduler/coherence coverage meaningful after receiver-aware admission. */
    const u32 receiver_mat = material_add(.5f, .6f, 0, .5f);
    const u32 receiver_mesh = lx_mesh_plane(f, 1, 1, 2);
    world_set(&f->w, 0, receiver_mesh, at(0, 0, -1), receiver_mat);
    const DeLight light = {.kind = DE_LIGHT_SPOT,
                           .color = {1, 1, 1},
                           .intensity = 100,
                           .range = 6,
                           .spot_outer = .5f,
                           .cast_shadows = true};
    u32 seen = 0;
    for (u32 frame = 0; frame < 6; ++frame) {
        // All16 continuously move together; equal importance must retain stable entity order.
        for (u32 i = 0; i < 16; ++i)
            world_light(&f->w, i, &light, at(.1f * frame, 0, 4));
        de__atlas_info info = atlas_begin(f, DE_QUALITY_LOW);
        EXPECT_EQ(info.rendered, 8);
        u32 mask = 0;
        for (u32 i = 0; i < 16; ++i) {
            EXPECT_EQ(info.data[1 + i * 6].identity[0], 5000u + i);
            if (info.data[1 + i * 6].identity[3])
                mask |= 1u << i;
        }
        EXPECT_EQ(mask, (frame & 1u) ? 0xff00u : 0xffu);
        seen |= mask;
        atlas_end_read(f, &info);
    }
    EXPECT_EQ(seen, 0xffffu);
    EXPECT_EQ(validation_errors(f->dev), 0);
}
static void point_coherence(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    ASSERT_TRUE(cvar_set("r.shadow_atlas_updates_per_frame", "1"));
    /* Keep scheduler/coherence coverage meaningful after receiver-aware admission. */
    const u32 receiver_mat = material_add(.5f, .6f, 0, .5f);
    const u32 receiver_mesh = lx_mesh_plane(f, 1, 1, 2);
    world_set(&f->w, 0, receiver_mesh, at(0, 0, -2), receiver_mat);
    const DeLight light = {.kind = DE_LIGHT_POINT,
                           .color = {1, 1, 1},
                           .intensity = 100,
                           .range = 8,
                           .cast_shadows = true};
    world_light(&f->w, 0, &light, at(0, 0, 0));
    for (u32 frame = 0; frame < 8; ++frame) {
        de__atlas_info info = atlas_begin(f, DE_QUALITY_LOW);
        EXPECT_EQ(info.rendered, frame < 6 ? 1 : 0);
        for (u32 face = 0; face < 6; ++face) {
            EXPECT_EQ(info.data[1 + face].identity[2], 6);
            EXPECT_EQ(info.data[1 + face].identity[3], frame < 5 ? 0 : 1);
        }
        atlas_end_read(f, &info);
    }
    ASSERT_TRUE(cvar_set("r.shadow_atlas_updates_per_frame", "8"));
    EXPECT_EQ(validation_errors(f->dev), 0);
}
static void spot_reference(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    const u32 mat = material_add(.5f, .6f, 0, .5f), mesh = lx_mesh_plane(f, 1, 1, 2);
    world_set(&f->w, 0, mesh, at(0, 0, 0), mat);
    f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
    const DeLight light = {.kind = DE_LIGHT_SPOT,
                           .color = {1, 1, 1},
                           .intensity = 100,
                           .range = 6,
                           .spot_outer = .5f,
                           .spot_inner = .3f,
                           .cast_shadows = true};
    world_light(&f->w, 0, &light, at(0, 0, 4));
    de__atlas_info info = atlas_begin(f, DE_QUALITY_LOW);
    atlas_query queries[200] = {0};
    for (u32 i = 0; i < 200; ++i) {
        queries[i].position[0] = ((f32)(i % 20) - 9.5f) * .23f;
        queries[i].position[1] = ((f32)(i / 20) - 4.5f) * .29f;
        queries[i].position[2] = -1;
        queries[i].normal[2] = 1;
        queries[i].index[0] = 1;
    }
    atlas_query_run run;
    atlas_query_add(f, &run, queries, 200);
    const f32 *pixels = atlas_end_read(f, &info), *values = de_gpu_buffer_map(f->dev, run.readback);
    ASSERT_TRUE(values);
    u32 r[4];
    tile_rect(&info, 0, 0, r);
    const double tangent = tan(.5) * (double)r[2] / (r[2] - 4), near_z = .06, far_z = 6,
                 a = near_z / (far_z - near_z), b = near_z * far_z / (far_z - near_z);
    u32 checked = 0, occluded = 0;
    for (u32 y = 0; y < r[3]; ++y)
        for (u32 x = 0; x < r[2]; ++x) {
            const double wx = (2. * (x + .5) / r[2] - 1.) * 4 * tangent,
                         wy = (1. - 2. * (y + .5) / r[3]) * 4 * tangent;
            if (fabs(fabs(wx) - 1) < .03 || fabs(fabs(wy) - 1) < .03)
                continue;
            bool hit = fabs(wx) < 1 && fabs(wy) < 1;
            const u32 offset = (r[1] + y) * info.size + r[0] + x;
            EXPECT_FLOAT_EQ(pixels[offset], hit ? b / 4 - a : 0, 2e-6);
            EXPECT_EQ(memcmp(pixels + offset, pixels + offset + info.size * info.size, 4), 0);
            checked++;
            occluded += hit;
        }
    EXPECT_GT(checked, 14000u);
    EXPECT_GT(occluded, 1000u);
    u32 lit = 0, dark = 0, partial = 0;
    for (u32 i = 0; i < 200; ++i) {
        double x = queries[i].position[0], y = queries[i].position[1];
        const double tile_texel = 2 * 5 * tangent / r[2], d = 5 - tile_texel * .5,
                     ndl = 5 / sqrt(x * x + y * y + 25), slope = sqrt(1 - ndl * ndl) / ndl,
                     depth = b / d - a + b / (d * d) * tile_texel * (.5 + fmin(slope, 4));
        const double u = (.5 + x / (2 * d * tangent)) * r[2] + r[0],
                     v = (.5 - y / (2 * d * tangent)) * r[3] + r[1];
        double expected = 0;
        for (u32 tap = 0; tap < 4; ++tap) {
            double radius = 1.5 * sqrt((tap + .5) / 4.), angle = tap * 2.399963229728653;
            double tx = fmax(r[0] + .5, fmin(r[0] + r[2] - .5, u + radius * cos(angle))),
                   ty = fmax(r[1] + .5, fmin(r[1] + r[3] - .5, v + radius * sin(angle)));
            expected += depth >= pixels[(u32)floor(ty) * info.size + (u32)floor(tx)] ? .25 : 0;
        }
        EXPECT_TRUE(isfinite(values[i * 4]));
        EXPECT_FLOAT_EQ(values[i * 4], expected, 1e-6);
        lit += expected == 1;
        dark += expected == 0;
        partial += expected > 0 && expected < 1;
    }
    EXPECT_GT(lit, 50u);
    EXPECT_GT(dark, 30u);
    EXPECT_GT(partial, 0u);
    printf("independent spot oracle: %u depth pixels (%u covered), visibility %u lit/%u dark/%u "
           "partial\n",
           checked, occluded, lit, dark, partial);
    atlas_query_destroy(f, &run);
    EXPECT_EQ(validation_errors(f->dev), 0);
}
static void point_cage(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    const u32 mat = material_add(.5f, .6f, 0, .5f), mesh = lx_mesh_plane(f, 1, 1, 6);
    const de_vec3 positions[6] = {{2, 0, 0},  {-2, 0, 0}, {0, 2, 0},
                                  {0, -2, 0}, {0, 0, 2},  {0, 0, -2}},
                  axes[6] = {{0, 1, 0}, {0, 1, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}};
    const f32 angles[6] = {-(f32)PI_D / 2, (f32)PI_D / 2, (f32)PI_D / 2,
                           -(f32)PI_D / 2, (f32)PI_D,     0};
    for (u32 i = 0; i < 6; ++i) {
        world_set(&f->w, i, mesh, trs(positions[i], axes[i], angles[i], 1), mat);
        f->w.f.instances.flags[i] |= DE__RW_INST_CAST_SHADOWS;
    }
    const DeLight light = {.kind = DE_LIGHT_POINT,
                           .color = {1, 1, 1},
                           .intensity = 100,
                           .range = 10,
                           .cast_shadows = true};
    world_light(&f->w, 0, &light, at(0, 0, 0));
    de__atlas_info info = atlas_begin(f, DE_QUALITY_LOW);
    EXPECT_EQ(info.rendered, 6);
    atlas_query queries[216] = {0};
    // Both sides of every major-axis seam, all octants and all three axis pairs.
    u32 count = 0;
    for (u32 pair = 0; pair < 3; ++pair)
        for (u32 octant = 0; octant < 8; ++octant)
            for (u32 e = 0; e < 3; ++e) {
                de_vec3 direction = {.3f, .3f, .3f};
                f32 *components = &direction.x;
                components[pair] = 1;
                components[(pair + 1) % 3] = 1 + ((f32)e - 1) * .001f;
                for (u32 k = 0; k < 3; ++k)
                    if (octant & (1u << k))
                        components[k] = -components[k];
                direction = de_v3_normalize(direction);
                for (u32 distance = 0; distance < 3; ++distance) {
                    atlas_query *q = &queries[count++];
                    const f32 d = distance == 0 ? 1.f : distance == 1 ? 4.f : 5.f;
                    q->position[0] = direction.x * d;
                    q->position[1] = direction.y * d;
                    q->position[2] = direction.z * d;
                    q->normal[0] = -direction.x;
                    q->normal[1] = -direction.y;
                    q->normal[2] = -direction.z;
                    q->index[0] = 1;
                }
            }
    ASSERT_EQ(count, 216);
    atlas_query_run run;
    atlas_query_add(f, &run, queries, count);
    const f32 *pixels = atlas_end_read(f, &info), *values = de_gpu_buffer_map(f->dev, run.readback);
    ASSERT_TRUE(values);
    for (u32 i = 0; i < count; ++i) {
        EXPECT_TRUE(isfinite(values[i * 4]));
        EXPECT_FLOAT_EQ(values[i * 4], i % 3 == 0 ? 1 : 0, 1e-6);
    }
    for (u32 face = 0; face < 6; ++face) {
        u32 r[4];
        tile_rect(&info, 0, face, r);
        u32 covered = 0;
        for (u32 y = r[1]; y < r[1] + r[3]; ++y)
            for (u32 x = r[0]; x < r[0] + r[2]; ++x)
                covered += pixels[y * info.size + x] > 0;
        EXPECT_GT(covered, 10000u);
    }
    atlas_query_destroy(f, &run);
    EXPECT_EQ(validation_errors(f->dev), 0);
}
static void atlas_receiver_test(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    f->width = 129;
    f->height = 83;
    f->eye = (de_vec3){0, 0, 10};
    f->target = (de_vec3){0};
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    ASSERT_EQ(de__forward_create_ex(f->vis, f->lt, &f->forward), DE_OK);
    f->shadow_quality = DE_QUALITY_LOW;
    // Exactly representable deferred values (sRGB128, roughness153) isolate shadow sampling.
    u32 mat = material_add(.2158605f, .6f, 0, .5f), receiver = material_add(.2158605f, .6f, 0, .5f);
    g_mat[mat - 1].standard.ior = g_mat[receiver - 1].standard.ior = 1.5f;
    g_mat[mat - 1].standard.specular_color = g_mat[receiver - 1].standard.specular_color =
        (de_vec3){1, 1, 1};
    u32 floor = lx_mesh_plane(f, 1, 1, 30), caster = lx_mesh_plane(f, 1, 1, 4),
        surface = lx_mesh_plane(f, 1, 1, 8);
    world_set(&f->w, 0, floor, at(0, 0, 0), mat);
    world_set(&f->w, 1, caster, at(0, 0, 2), mat);
    f->w.f.instances.flags[1] |= DE__RW_INST_CAST_SHADOWS;
    DeLight sun = {.kind = DE_LIGHT_SPOT,
                   .color = {1, 1, 1},
                   .intensity = 10000,
                   .range = 20,
                   .spot_inner = .7f,
                   .spot_outer = .9f,
                   .cast_shadows = true};
    // Offset the source so the camera can see a full shadow region beyond the caster itself.
    world_light(&f->w, 0, &sun, at(3, 0, 8));
    u32 pixels = f->width * f->height;
    f32 *forward = zalloc((size_t)pixels * 16), *unshadowed = zalloc((size_t)pixels * 16);
    u32 dark = 0;
    for (u32 phase = 0; phase < 2; ++phase) {
        f32 z = phase ? .5f : 3;
        world_set(&f->w, 2, surface, at(0, 0, z), receiver);
        g_mat[receiver - 1].alpha_mode = DE_COOKED_ALPHA_BLEND;
        g_mat[receiver - 1].standard.alpha_mode = DE_COOKED_ALPHA_BLEND;
        f->shadow_quality = DE_QUALITY_OFF;
        ASSERT_TRUE(lx_frame(f));
        memcpy(unshadowed, f->res.lighting, (size_t)pixels * 16);
        f->shadow_quality = DE_QUALITY_LOW;
        ASSERT_TRUE(lx_frame(f));
        memcpy(forward, f->res.lighting, (size_t)pixels * 16);
        u32 flags = 0;
        memcpy(&flags, &f->res.lights[0].axis[3], 4);
        de__atlas_info diagnostic;
        ASSERT_TRUE(de__shadow_atlas_get(f->atlas, (de_view){.id = 1}, &diagnostic));
        printf("receiver phase%u: cluster flags%u atlas records%u valid%u renders%u\n", phase,
               flags, f->res.lc.atlas_records.id, diagnostic.data[1].identity[3],
               diagnostic.rendered);
        EXPECT_EQ((flags >> 8u) & 255u, 1);
        g_mat[receiver - 1].alpha_mode = DE_COOKED_ALPHA_OPAQUE;
        g_mat[receiver - 1].standard.alpha_mode = DE_COOKED_ALPHA_OPAQUE;
        f->w.touch_all = true;
        ASSERT_TRUE(lx_frame(f));
        u32 checked = 0;
        f32 max_unshadowed = 0, min_ratio = 1;
        for (u32 i = 0; i < pixels; ++i) {
            f32 *p = f->res.pos + i * 4;
            if (p[3] < .5f || fabsf(p[2] - z) > .001f || fabsf(p[0]) > 3.8f || fabsf(p[1]) > 3.8f)
                continue;
            ++checked;
            max_unshadowed = fmaxf(max_unshadowed, unshadowed[i * 4]);
            if (unshadowed[i * 4] > .001f)
                min_ratio = fminf(min_ratio, forward[i * 4] / unshadowed[i * 4]);
            for (u32 ch = 0; ch < 3; ++ch)
                EXPECT_FLOAT_EQ(forward[i * 4 + ch], f->res.lighting[i * 4 + ch],
                                .01f * fmaxf(f->res.lighting[i * 4 + ch], .01f));
            if (!phase)
                EXPECT_FLOAT_EQ(forward[i * 4], unshadowed[i * 4], .002f);
            else
                dark += forward[i * 4] < unshadowed[i * 4] * .1f;
        }
        EXPECT_GT(checked, 100u);
        printf("receiver phase%u: checked%u baselineMax%g minimumRatio%g\n", phase, checked,
               max_unshadowed, min_ratio);
    }
    EXPECT_GT(dark, 10u);
    printf("own-surface forward shadows: %u shadowed pixels, above-caster surface stays lit\n",
           dark);
    EXPECT_EQ(validation_errors(f->dev), 0);
    free(forward);
    free(unshadowed);
}

static void rotating_groups(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    ASSERT_TRUE(cvar_set("r.shadow_atlas_updates_per_frame", "1"));
    u32 mat = material_add(.5f, .6f, 0, .5f), mesh = lx_mesh_plane(f, 1, 1, 2);
    world_set(&f->w, 0, mesh, at(0, 0, 0), mat);
    f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
    world_set(&f->w, 1, mesh, at(0, 0, 0), mat);
    f->w.f.instances.layer_mask[1] = 2; /* non-casting receiver, zero-depth fallback still tested */
    DeLight light = {.kind = DE_LIGHT_SPOT,
                     .color = {1, 1, 1},
                     .intensity = 100,
                     .range = 6,
                     .spot_outer = .5f,
                     .cast_shadows = true};
    world_light(&f->w, 0, &light, at(0, 0, 4));
    de__rw_view camera = {.offset = f->eye};
    de_render_settings settings = {.shadows = DE_QUALITY_LOW};
    for (u32 frame = 0; frame < 4; ++frame) {
        world_begin(&f->w);
        ASSERT_EQ(de__gpu_scene_update(f->gs, &f->w.f), DE_OK);
        de__shadow_atlas_frame_begin(f->atlas, frame, &camera);
        for (u32 i = 0; i < 2; ++i)
            ASSERT_EQ(de__shadow_atlas_frame_view(f->atlas, (de_view){.id = i + 1}, 1u << i,
                                                  DE_QUALITY_LOW),
                      DE_OK);
        u32 rendered = 0;
        for (u32 i = 0; i < 2; ++i) {
            de__rg_reset(f->g);
            de_arena_reset(f->arena);
            ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
            ASSERT_EQ(de__gpu_scene_add_passes(f->gs, f->g), DE_OK);
            de__view_frame vf = {.view = {.id = i + 1},
                                 .graph = f->g,
                                 .world = &f->w.f,
                                 .camera = &camera,
                                 .settings = &settings,
                                 .frame_index = frame};
            de__vis_context vc = {
                .gpu_scene = f->gs, .frames_completed = frame + 1, .layer_mask = 1u << i};
            ASSERT_EQ(de__shadow_atlas_build(f->atlas, &vf, &vc), DE_OK);
            de__atlas_info info;
            ASSERT_TRUE(de__shadow_atlas_get(f->atlas, vf.view, &info));
            rendered += info.rendered;
            EXPECT_EQ(info.rendered, i == 0 ? (frame % 2 == 0 ? 1 : 0) : (frame == 1 ? 1 : 0));
            const f32 *pixels = atlas_end_read_view(f, &info, vf.view, frame);
            if (i == 1 && frame == 0) {
                /* Cold zero-quota storage is published for fallback reads, not a completed
                 * shadow generation. Both complete images must already be defined zero. */
                const u64 texels = (u64)info.size * info.size;
                u64 nonzero = 0;
                for (u64 p = 0; p < texels * 2u; ++p)
                    if (pixels[p] != 0)
                        ++nonzero;
                EXPECT_EQ(nonzero, 0u);
                EXPECT_EQ(memcmp(pixels, pixels + texels, (size_t)texels * sizeof *pixels), 0);
                EXPECT_FALSE(de__shadow_atlas_capture_ready(f->atlas, vf.view));
            }
            if (i == 0 && frame == 0)
                ASSERT_TRUE(de__shadow_atlas_dynamic(f->atlas, vf.view, 5000, true));
            if (i == 1)
                EXPECT_EQ(de__shadow_atlas_capture_ready(f->atlas, vf.view), frame > 0);
        }
        EXPECT_TRUE(rendered <= 1);
        EXPECT_EQ(rendered, frame < 3 ? 1 : 0);
    }
    ASSERT_TRUE(cvar_set("r.shadow_atlas_updates_per_frame", "8"));
    EXPECT_EQ(validation_errors(f->dev), 0);
}
static void point_capacity(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    ASSERT_TRUE(cvar_set("r.shadow_atlas_updates_per_frame", "192"));
    u32 mat = material_add(.5f, .6f, 0, .5f), mesh = lx_mesh_plane(f, 1, 1, 2);
    world_set(&f->w, 0, mesh, at(0, 0, -2), mat);
    f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
    const DeLight light = {.kind = DE_LIGHT_POINT,
                           .color = {1, 1, 1},
                           .intensity = 100,
                           .range = 8,
                           .cast_shadows = true};
    for (u32 i = 0; i < 32; ++i)
        world_light(&f->w, i, &light, at(0, 0, 0));
    de__atlas_info info = atlas_begin(f, DE_QUALITY_LOW);
    EXPECT_EQ(info.lights, 32);
    EXPECT_EQ(info.rendered, 192);
    EXPECT_TRUE(info.capacity >= 192);
    EXPECT_EQ(info.occupied, 192u * 128u * 128u);
    for (u32 i = 0; i < 32; ++i)
        for (u32 face = 0; face < 6; ++face) {
            const de__atlas_gpu *record = &info.data[1 + i * 6 + face];
            EXPECT_EQ(record->identity[0], 5000u + i);
            EXPECT_EQ(record->identity[2], 6);
            EXPECT_EQ(record->identity[3], 1);
        }
    const f32 *pixels = atlas_end_read(f, &info);
    for (u32 i = 0; i < 32; ++i) {
        u32 rect[4];
        tile_rect(&info, i, 5, rect);
        u32 center = (rect[1] + rect[3] / 2u) * info.size + rect[0] + rect[2] / 2u;
        EXPECT_FLOAT_EQ(pixels[center], .08 * 8 / (7.92 * 2) - .08 / 7.92, 2e-6);
        EXPECT_EQ(memcmp(pixels + center, pixels + center + info.size * info.size, 4), 0);
    }
    u32 chunks = 0, lists[DE_QUEUE_COUNT_] = {0};
    const de__rg_chunk *partition = de__rg_exec_chunks(f->g, &chunks);
    for (u32 i = 0; i < chunks; ++i)
        ++lists[partition[i].queue];
    if (backend == DE_GPU_METAL) {
        EXPECT_TRUE(lists[DE_QUEUE_GRAPHICS] + 2u <= 64u);
        EXPECT_TRUE(lists[DE_QUEUE_COMPUTE] + 1u <= 64u);
        EXPECT_TRUE(lists[DE_QUEUE_COPY] + 2u <= 64u);
    }
    printf("192 tile renders: %u recorded chunks (graphics %u, compute %u, copy %u)\n", chunks,
           lists[DE_QUEUE_GRAPHICS], lists[DE_QUEUE_COMPUTE], lists[DE_QUEUE_COPY]);
    ASSERT_TRUE(cvar_set("r.shadow_atlas_updates_per_frame", "8"));
    info = atlas_begin(f, DE_QUALITY_LOW);
    EXPECT_EQ(info.rendered, 0);
    atlas_end_read(f, &info);
    EXPECT_EQ(validation_errors(f->dev), 0);
}
static void point_default_budget(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    const u32 mat = material_add(.5f, .6f, 0, .5f), mesh = lx_mesh_plane(f, 1, 1, 2);
    world_set(&f->w, 0, mesh, at(0, 0, -2), mat);
    f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
    const DeLight light = {.kind = DE_LIGHT_POINT,
                           .color = {1, 1, 1},
                           .intensity = 100,
                           .range = 8,
                           .cast_shadows = true};
    for (u32 i = 0; i < 32; ++i)
        world_light(&f->w, i, &light, at(0, 0, 0));
    for (u32 frame = 0; frame < 25; ++frame) {
        de__atlas_info info = atlas_begin(f, DE_QUALITY_LOW);
        EXPECT_EQ(info.lights, 32);
        EXPECT_EQ(info.rendered, frame < 24 ? 8 : 0);
        EXPECT_EQ(info.occupied, 192u * 128u * 128u);
        u32 ready = 0;
        for (u32 i = 0; i < 32; ++i) {
            for (u32 face = 0; face < 6; ++face) {
                const de__atlas_gpu *record = &info.data[1 + i * 6 + face];
                EXPECT_EQ(record->identity[0], 5000u + i);
                EXPECT_EQ(record->identity[2], 6);
                EXPECT_EQ(record->identity[3], info.data[1 + i * 6].identity[3]);
            }
            ready += info.data[1 + i * 6].identity[3] != 0;
        }
        EXPECT_EQ(ready, frame < 20 ? 0 : frame < 24 ? (frame - 19u) * 8u : 32u);
        const f32 *pixels = atlas_end_read(f, &info);
        if (frame >= 23)
            for (u32 i = 0; i < 32; ++i) {
                u32 rect[4], covered = 0;
                tile_rect(&info, i, 5, rect);
                for (u32 y = rect[1]; y < rect[1] + rect[3]; ++y)
                    for (u32 x = rect[0]; x < rect[0] + rect[2]; ++x)
                        covered += pixels[y * info.size + x] > 0;
                EXPECT_GT(covered, 100u);
            }
    }
    EXPECT_EQ(validation_errors(f->dev), 0);
}
static void wide_spot(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    u32 mat = material_add(.5f, .6f, 0, .5f), mesh = lx_mesh_plane(f, 1, 1, 20);
    world_set(&f->w, 0, mesh, at(0, 0, -.5f), mat);
    f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
    DeLight light = {.kind = DE_LIGHT_SPOT,
                     .color = {1, 1, 1},
                     .intensity = 100,
                     .range = 8,
                     .spot_outer = 85.f * (f32)PI_D / 180.f,
                     .cast_shadows = true};
    world_light(&f->w, 0, &light, at(0, 0, 0));
    de__atlas_info info = atlas_begin(f, DE_QUALITY_LOW);
    atlas_query queries[3] = {0};
    const f32 x = sinf(80.f * (f32)PI_D / 180.f), z = cosf(80.f * (f32)PI_D / 180.f);
    queries[0].position[0] = 5 * x;
    queries[0].position[2] = -5 * z;
    queries[0].normal[0] = -x;
    queries[0].normal[2] = z;
    queries[1].position[2] = 1;
    queries[1].normal[2] = -1;
    for (u32 i = 0; i < 3; ++i)
        queries[i].index[0] = 1;
    atlas_query_run run;
    atlas_query_add(f, &run, queries, 3);
    atlas_end_read(f, &info);
    const f32 *result = de_gpu_buffer_map(f->dev, run.readback);
    ASSERT_TRUE(result);
    EXPECT_FLOAT_EQ(result[0], 0, 1e-6);
    EXPECT_FLOAT_EQ(result[4], 1, 1e-6);
    EXPECT_FLOAT_EQ(result[8], 1, 1e-6);
    atlas_query_destroy(f, &run);
    EXPECT_EQ(validation_errors(f->dev), 0);
}
static void ultra_storage(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    u32 mat = material_add(.5f, .6f, 0, .5f), mesh = lx_mesh_plane(f, 1, 1, 2);
    world_set(&f->w, 0, mesh, at(0, 0, 0), mat);
    f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
    const DeLight light = {.kind = DE_LIGHT_SPOT,
                           .color = {1, 1, 1},
                           .intensity = 100,
                           .range = 6,
                           .spot_outer = .5f,
                           .cast_shadows = true};
    world_light(&f->w, 0, &light, at(0, 0, 4));
    de__atlas_info info = atlas_begin(f, DE_QUALITY_LOW);
    EXPECT_EQ(info.size, 4096);
    atlas_end_read(f, &info);
    info = atlas_begin(f, DE_QUALITY_ULTRA);
    EXPECT_EQ(info.size, 8192);
    EXPECT_EQ(info.rendered, 1);
    EXPECT_EQ(info.data[1].map.resolution, 8192);
    EXPECT_EQ(info.data[1].map.taps, 32);
    EXPECT_EQ(info.occupied, 2048u * 2048u);
    const f32 *pixels = atlas_end_read(f, &info);
    EXPECT_GT(tile_nonzero(&info, pixels, 0), 100000u);
    const de_gpu_texture retained = info.depth;
    info = atlas_begin(f, DE_QUALITY_ULTRA);
    EXPECT_EQ(info.depth.id, retained.id);
    EXPECT_EQ(info.rendered, 0);
    atlas_end_read(f, &info);
    EXPECT_EQ(validation_errors(f->dev), 0);
}
static void capture_progress(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    de_type_registry *types = NULL;
    de_world *ecs = NULL;
    de_renderer *renderer = NULL;
    ASSERT_EQ(de_type_registry_create(&f->alloc, &types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &f->alloc, .types = types}, &ecs),
              DE_OK);
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->dev,
                                                     .types = types,
                                                     .allocator = &f->alloc,
                                                     .frames_in_flight = 2},
                                 &renderer),
              DE_OK);
    de_renderer_attach(renderer, ecs);
    ASSERT_TRUE(cvar_set("r.pipelined", "0"));
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW);
    settings.gi = settings.reflections = settings.ao = DE_QUALITY_OFF;
    settings.upscaler = DE_UPSCALE_NONE;
    settings.render_scale = .25f;
    de_renderer_set_settings(renderer, &settings);
    de__probe_capture capture = {0};
    ASSERT_EQ(de__probe_capture_init(&capture, renderer, 32), DE_OK);
    de_render_settings cs = settings;
    cs.shadows = DE_QUALITY_HIGH;
    cs.render_scale = 1;
    for (u32 face = 0; face < 6; ++face)
        de_view_set_settings(renderer, capture.views[face], &cs);
    u32 mat = material_add(.5f, .6f, 0, .5f), mesh = lx_mesh_plane(f, 1, 1, 2);
    world_set(&f->w, 0, mesh, at(0, 0, 0), mat);
    f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
    const DeLight spot = {.kind = DE_LIGHT_SPOT,
                          .color = {1, 1, 1},
                          .intensity = 100,
                          .range = 6,
                          .spot_outer = .5f,
                          .cast_shadows = true};
    for (u32 i = 0; i < 16; ++i)
        world_light(&f->w, i, &spot, at(0, 0, 4));
    const DeLight sun = {
        .kind = DE_LIGHT_DIRECTIONAL, .color = {1, 1, 1}, .intensity = 1, .cast_shadows = true};
    world_light(&f->w, 16, &sun, at(0, 0, 0));
    de__gpu_scene_destroy(renderer->gpu_scene);
    renderer->gpu_scene = NULL;
    ASSERT_EQ(
        de__gpu_scene_create(&(de__gpu_scene_desc){.device = f->dev,
                                                   .allocator = &f->alloc,
                                                   .resolver = {.mesh = fake_mesh,
                                                                .material = fake_material,
                                                                .texture_bindless = fake_texture},
                                                   .staging_bytes = 16u << 20,
                                                   .views_in_view_graphs = true,
                                                   .initial_capacity[DE__GS_VIEWS] = DE__VIEW_MAX},
                             &renderer->gpu_scene),
        DE_OK);
    de__rw_view camera = {
        .entity = 999,
        .camera = {.near_z = .1f, .fov_y = 1.04719755f, .view = de_renderer_main_view(renderer)},
        .offset = {0, 0, 10},
        .rotation = {0, 0, 0, 1},
        .transform = at(0, 0, 10)};
    f->w.f.views = &camera;
    f->w.f.view_count = 1;
    f->w.f.main_view = 0;
    renderer->render_world->last = renderer->render_world->current = &f->w.f;
    de_gpu_texture shared = {0};
    for (u32 frame = 0; frame < 3; ++frame) {
        de__probe_capture_at_position(&capture, (de_dvec3){0, 0, 8}, (de_dvec3){0}, 63);
        world_begin(&f->w);
        de__renderer_kick_prepare(renderer);
        ASSERT_EQ(de_renderer_render_frame(renderer), DE_OK);
        de_gpu_wait_idle(f->dev);
        de__atlas_info main;
        ASSERT_TRUE(
            de__shadow_atlas_get(renderer->shadow_atlas, de_renderer_main_view(renderer), &main));
        EXPECT_EQ(main.lights, 16);
        EXPECT_EQ(main.rendered, 0);
        EXPECT_EQ(renderer->stats.shadow_atlas_tiles_rendered, frame < 2 ? 8 : 0);
        EXPECT_GT(renderer->stats.shadow_atlas_occupied_texels, 0u);
        if (!frame)
            shared = main.depth;
        EXPECT_EQ(main.depth.id, shared.id);
        for (u32 face = 0; face < 6; ++face) {
            de__atlas_info ci;
            ASSERT_TRUE(de__shadow_atlas_get(renderer->shadow_atlas, capture.views[face], &ci));
            EXPECT_EQ(ci.depth.id, main.depth.id);
            EXPECT_EQ(ci.rendered, face == 0 && frame < 2 ? 8 : 0);
            EXPECT_EQ(ci.data[1].map.taps, 16);
            EXPECT_EQ(main.data[1].map.taps, 4);
            EXPECT_EQ(ci.data[1].map.resolution, 4096);
            const void *mapped = de_gpu_buffer_map(f->dev, ci.records);
            ASSERT_TRUE(mapped);
            EXPECT_EQ(memcmp(mapped, ci.data, sizeof ci.data), 0);
            EXPECT_EQ(de__shadow_atlas_capture_ready(renderer->shadow_atlas, capture.views[face]),
                      frame > 0);
        }
        de_result result = de__probe_capture_complete(&capture, NULL,
                                                      (de__probe_handle){DE__PROBE_NONE, 0}, false);
        EXPECT_EQ(result, frame == 0 ? DE_ERR_BUSY : DE_OK);
        de_error_clear();
        de__csm_info directional;
        ASSERT_TRUE(de__csm_get(renderer->csm, de_renderer_main_view(renderer), &directional));
        EXPECT_TRUE(directional.ready);
        EXPECT_EQ(directional.lights, 1);
    }
    /* The second mask must have a receiver to exercise a competing shadow cache. */
    world_set(&f->w, 1, mesh, at(0, 0, 0), mat);
    f->w.f.instances.layer_mask[1] = 2;
    de_view_set_layer_mask(renderer, de_renderer_main_view(renderer), 1);
    for (u32 face = 0; face < 6; ++face)
        de_view_set_layer_mask(renderer, capture.views[face], 2);
    for (u32 frame = 0; frame < 4; ++frame) {
        world_set(&f->w, 0, mesh, at(.1f * frame, 0, 0), mat);
        f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
        de__probe_capture_at_position(&capture, (de_dvec3){0, 0, 8}, (de_dvec3){0}, 63);
        world_begin(&f->w);
        de__renderer_kick_prepare(renderer);
        ASSERT_EQ(de_renderer_render_frame(renderer), DE_OK);
        de_gpu_wait_idle(f->dev);
        de__atlas_info main, ci;
        ASSERT_TRUE(
            de__shadow_atlas_get(renderer->shadow_atlas, de_renderer_main_view(renderer), &main));
        ASSERT_TRUE(de__shadow_atlas_get(renderer->shadow_atlas, capture.views[0], &ci));
        EXPECT_NE(main.depth.id, ci.depth.id);
        EXPECT_EQ(main.rendered, 4);
        EXPECT_EQ(ci.rendered, 4);
        EXPECT_EQ(renderer->stats.shadow_atlas_tiles_rendered, 8);
        de_result result = de__probe_capture_complete(&capture, NULL,
                                                      (de__probe_handle){DE__PROBE_NONE, 0}, false);
        EXPECT_EQ(result, frame < 3 ? DE_ERR_BUSY : DE_OK);
        de_error_clear();
    }
    de__probe_capture_destroy(&capture);
    renderer->render_world->current = renderer->render_world->last = NULL;
    de_renderer_destroy(renderer);
    de_world_destroy(ecs);
    de_type_registry_destroy(types);
    f->w.f.views = NULL;
    f->w.f.view_count = 0;
    EXPECT_EQ(validation_errors(f->dev), 0);
}
/* Two independent layer groups; one point-light slot per group. No windows/platform init. */
static void hidden_shadow_groups(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    lx_save_shadow_budget(f);
    ASSERT_TRUE(cvar_set("r.shadow_local_max", "1"));
    ASSERT_TRUE(cvar_set("r.shadow_atlas_updates_per_frame", "12"));
    const u32 material = material_add(.5f, .6f, 0, .5f);
    const u32 mesh = lx_mesh_plane(f, 1, 1, 4);
    world_set(&f->w, 0, mesh, at(0, 0, 0), material);
    world_set(&f->w, 1, mesh, at(0, 0, -3), material);
    f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
    const DeLight light = {.kind = DE_LIGHT_POINT,
                           .color = {1, 1, 1},
                           .intensity = 10,
                           .range = 20,
                           .cast_shadows = true};
    world_light(&f->w, 0, &light, at(0, 0, 4));
    world_begin(&f->w);
    ASSERT_EQ(de__gpu_scene_update(f->gs, &f->w.f), DE_OK);
    const u64 entity = f->w.f.instances.entity[0];
    const de__view_hidden *hidden = NULL;
    ASSERT_EQ(de__view_hidden_create(&f->alloc, &entity, 1, &hidden), DE_OK);
    de_gpu_buffer ids =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = sizeof(u64),
                                                           .stride = sizeof(u64),
                                                           .usage = DE_USAGE_STORAGE,
                                                           .memory = DE_MEM_UPLOAD});
    ASSERT_TRUE(ids.id != 0);
    memcpy(de_gpu_buffer_map(f->dev, ids), &entity, sizeof entity);
    const de__rw_view camera = {.offset = {0, 0, 20}};
    const de_render_settings settings = {.shadows = DE_QUALITY_LOW};
    de__shadow_atlas_frame_begin(f->atlas, 0, &camera);
    for (u32 i = 0; i < 2; ++i)
        ASSERT_EQ(de__shadow_atlas_frame_view_hidden(f->atlas, (de_view){.id = i + 1}, 1,
                                                     DE_QUALITY_LOW, i ? hidden : NULL),
                  DE_OK);
    de_gpu_texture canonical = {0};
    for (u32 i = 0; i < 2; ++i) {
        de__rg_reset(f->g);
        de_arena_reset(f->arena);
        ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
        ASSERT_EQ(de__gpu_scene_add_passes(f->gs, f->g), DE_OK);
        const de__view_frame vf = {.view = {.id = i + 1},
                                   .graph = f->g,
                                   .world = &f->w.f,
                                   .camera = &camera,
                                   .settings = &settings,
                                   .frame_index = 0,
                                   .hidden = i ? hidden : NULL,
                                   .hidden_buffer = i ? ids : (de_gpu_buffer){0}};
        const de__vis_context vc = {.gpu_scene = f->gs, .frames_completed = 1, .layer_mask = 1};
        ASSERT_EQ(de__shadow_atlas_build(f->atlas, &vf, &vc), DE_OK);
        de__atlas_info info;
        ASSERT_TRUE(de__shadow_atlas_get(f->atlas, vf.view, &info));
        EXPECT_EQ(info.lights, 1u);
        if (i)
            EXPECT_TRUE(info.depth.id != canonical.id);
        else
            canonical = info.depth;
        const f32 *pixels = atlas_end_read_view(f, &info, vf.view, 0);
        u32 covered = 0;
        for (u64 pixel = 0; pixel < (u64)info.size * info.size; ++pixel)
            covered += pixels[pixel] > 0;
        if (i)
            EXPECT_EQ(covered, 0u);
        else
            EXPECT_GT(covered, 100u);
    }
    de_gpu_wait_idle(f->dev);
    de_gpu_buffer_destroy(f->dev, ids);
    de__view_hidden_release(hidden);
    EXPECT_EQ(validation_errors(f->dev), 0u);
}
static void receiver_admission(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    lx_save_shadow_budget(f);
    ASSERT_TRUE(cvar_set("r.shadow_local_max", "1"));
    ASSERT_TRUE(cvar_set("r.shadow_atlas_updates_per_frame", "12"));
    const u32 mat = material_add(.5f, .6f, 0, .5f), mesh = lx_mesh_plane(f, 1, 1, 4);
    for (u32 i = 0; i < 2; ++i) {
        world_set(&f->w, i, mesh, at(i ? 1000.f : 0.f, 0, 0), mat);
        f->w.f.instances.layer_mask[i] = 1u << i;
        f->w.f.instances.flags[i] |= DE__RW_INST_CAST_SHADOWS;
        const DeLight light = {.kind = DE_LIGHT_POINT,
                               .color = {1, 1, 1},
                               .intensity = i ? 1e9f : 10,
                               .range = 8,
                               .cast_shadows = true};
        world_light(&f->w, i, &light, at(i ? 1000.f : 0.f, 0, 4));
    }
    world_begin(&f->w);
    ASSERT_EQ(de__gpu_scene_update(f->gs, &f->w.f), DE_OK);
    de__rw_view camera = {.offset = {0, 0, 20}};
    de_render_settings settings = {.shadows = DE_QUALITY_LOW};
    de__shadow_atlas_frame_begin(f->atlas, 0, &camera);
    for (u32 i = 0; i < 2; ++i)
        ASSERT_EQ(
            de__shadow_atlas_frame_view(f->atlas, (de_view){.id = i + 1}, 1u << i, DE_QUALITY_LOW),
            DE_OK);
    for (u32 i = 0; i < 2; ++i) {
        de__rg_reset(f->g);
        de_arena_reset(f->arena);
        ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
        ASSERT_EQ(de__gpu_scene_add_passes(f->gs, f->g), DE_OK);
        de__view_frame vf = {.view = {.id = i + 1},
                             .graph = f->g,
                             .world = &f->w.f,
                             .camera = &camera,
                             .settings = &settings,
                             .frame_index = 0};
        de__vis_context vc = {.gpu_scene = f->gs, .frames_completed = 1, .layer_mask = 1u << i};
        ASSERT_EQ(de__shadow_atlas_build(f->atlas, &vf, &vc), DE_OK);
        de__atlas_info info;
        ASSERT_TRUE(de__shadow_atlas_get(f->atlas, vf.view, &info));
        EXPECT_EQ(info.lights, 1);
        EXPECT_EQ(info.rendered, 6);
        EXPECT_EQ(info.data[1].identity[0], 5000u + i);
        const f32 *pixels = atlas_end_read_view(f, &info, vf.view, 0);
        u32 covered = 0;
        for (u64 j = 0; j < (u64)info.size * info.size; ++j)
            covered += pixels[j] > 0;
        EXPECT_GT(covered, 100u);
        printf("DENG-644 view%u selected=%u lights=%u faces=%u occupied=%u depth-pixels=%u\n", i,
               info.data[1].identity[0], info.lights, info.rendered, info.occupied, covered);
        const char *dir = getenv("DE_SHADOW_EVIDENCE_DIR");
        if (dir && *dir) {
            char path[1024];
            snprintf(path, sizeof path, "%s/view%u.pgm", dir, i);
            FILE *file = fopen(path, "wb");
            ASSERT_TRUE(file != NULL);
            u32 rect[4];
            tile_rect(&info, 0, 5, rect); /* -Z cube face: the plane beneath the light. */
            fprintf(file, "P5\n%u %u\n255\n", rect[2], rect[3]);
            for (u32 y = 0; y < rect[3]; ++y)
                for (u32 x = 0; x < rect[2]; ++x) {
                    const f32 d = pixels[(rect[1] + y) * info.size + rect[0] + x];
                    const u8 value = d > 0 ? (u8)fminf(255, 64 + d * 191) : 0;
                    ASSERT_EQ(fwrite(&value, 1, 1, file), 1);
                }
            ASSERT_EQ(fclose(file), 0);
        }
    }
    /* Retire an empty receiver group, then reactivate it with geometry under the other light. */
    for (u32 phase = 1; phase <= 2; ++phase) {
        if (phase == 1) {
            f->w.f.instances.entity[0] = 0;
            f->w.f.removed_instance_count = 1;
            f->w.f.removed_instances[0] = 0;
        } else {
            f->w.f.removed_instance_count = 0;
            world_set(&f->w, 0, mesh, at(1000, 0, 0), mat);
            f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
        }
        world_begin(&f->w);
        ASSERT_EQ(de__gpu_scene_update(f->gs, &f->w.f), DE_OK);
        de__shadow_atlas_frame_begin(f->atlas, phase, &camera);
        de__rg_reset(f->g);
        de_arena_reset(f->arena);
        ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
        ASSERT_EQ(de__gpu_scene_add_passes(f->gs, f->g), DE_OK);
        de__view_frame vf = {.view = {.id = 1},
                             .graph = f->g,
                             .world = &f->w.f,
                             .camera = &camera,
                             .settings = &settings,
                             .frame_index = phase};
        de__vis_context vc = {.gpu_scene = f->gs, .frames_completed = phase + 1, .layer_mask = 1};
        ASSERT_EQ(de__shadow_atlas_build(f->atlas, &vf, &vc), DE_OK);
        de__atlas_info info;
        ASSERT_TRUE(de__shadow_atlas_get(f->atlas, vf.view, &info));
        EXPECT_EQ(info.lights, phase == 1 ? 0 : 1);
        EXPECT_EQ(info.rendered, phase == 1 ? 0 : 6);
        if (phase == 1) {
            EXPECT_EQ(info.occupied, 0);
            EXPECT_EQ(info.data[1].identity[0], 0);
            de__shadow_atlas_complete(f->atlas, vf.view, f->g, false, false);
            de__gpu_scene_frame_end(f->gs, f->g, false);
        } else {
            EXPECT_EQ(info.data[1].identity[0], 5001);
            const f32 *pixels = atlas_end_read_view(f, &info, vf.view, phase);
            u32 covered = 0;
            for (u64 j = 0; j < (u64)info.size * info.size; ++j)
                covered += pixels[j] > 0;
            EXPECT_GT(covered, 100u);
        }
    }
    EXPECT_EQ(validation_errors(f->dev), 0);
}
#define ADMISSION_TEST(tag, backend)                                                               \
    TEST_F(shadow_atlas, atlas_receiver_admission_##tag, lx) {                                     \
        receiver_admission(f, backend);                                                            \
    }
ADMISSION_TEST(metal, DE_GPU_METAL)
ADMISSION_TEST(vulkan, DE_GPU_VULKAN)
ADMISSION_TEST(d3d12, DE_GPU_D3D12)
#define HIDDEN_SHADOW_TEST(tag, backend)                                                           \
    TEST_F(shadow_atlas, hidden_shadow_groups_##tag, lx) {                                         \
        hidden_shadow_groups(f, backend);                                                          \
    }
HIDDEN_SHADOW_TEST(metal, DE_GPU_METAL)
HIDDEN_SHADOW_TEST(vulkan, DE_GPU_VULKAN)
HIDDEN_SHADOW_TEST(d3d12, DE_GPU_D3D12)

/* The orthographic camera sees x[-1,1]; the entire caster is outside that frustum.
 * Light(3,0,8) -> caster(1.5,0,4) -> visible floor(0,0,0) is an analytic shadow ray. */
static void off_camera_caster(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    f->width = f->height = 64;
    f->ortho_height = 2;
    f->eye = (de_vec3){0, 0, 10};
    f->target = (de_vec3){0};
    ASSERT_EQ(de__shadow_atlas_create_ex(f->dev, &f->alloc, &f->atlas), DE_OK);
    f->shadow_quality = DE_QUALITY_LOW;
    const u32 mat = material_add(.2158605f, .6f, 0, .5f);
    const u32 floor = lx_mesh_plane(f, 1, 1, 2), caster = lx_mesh_plane(f, 1, 1, .5f);
    world_set(&f->w, 0, floor, at(0, 0, 0), mat);
    world_set(&f->w, 1, caster, at(1.5f, 0, 4), mat);
    EXPECT_TRUE(f->w.f.instances.bounds[1].min.x > .5f * f->ortho_height);
    const DeLight light = {.kind = DE_LIGHT_SPOT,
                           .color = {1, 1, 1},
                           .intensity = 10000,
                           .range = 20,
                           .spot_inner = .7f,
                           .spot_outer = .9f,
                           .cast_shadows = true};
    world_light(&f->w, 0, &light, at(3, 0, 8));
    f32 baseline[16 * 16] = {0};
    for (u32 phase = 0; phase < 3; ++phase) {
        if (phase == 1)
            f->w.f.instances.flags[1] |= DE__RW_INST_CAST_SHADOWS;
        else
            f->w.f.instances.flags[1] &= (u8)~DE__RW_INST_CAST_SHADOWS;
        f->w.touch_all = true;
        ASSERT_TRUE(lx_frame(f));
        u32 checked = 0;
        for (u32 y = 24; y < 40; ++y)
            for (u32 x = 24; x < 40; ++x) {
                const u32 index = (y - 24) * 16 + x - 24;
                const f32 value = f->res.lighting[(y * f->width + x) * 4];
                EXPECT_TRUE(isfinite(value));
                if (phase == 0) {
                    EXPECT_TRUE(value > .01f);
                    baseline[index] = value;
                } else if (phase == 1) {
                    EXPECT_TRUE(value < baseline[index] * .1f);
                } else {
                    EXPECT_FLOAT_EQ(value, baseline[index], baseline[index] * .01f + 1e-5f);
                }
                ++checked;
            }
        EXPECT_EQ(checked, 256);
        printf("off-camera caster phase%u:256visible floor samples checked\n", phase);
        const char *dir = getenv("DE_SHADOW_EVIDENCE_DIR");
        if (dir && *dir) {
            char path[1024];
            snprintf(path, sizeof path, "%s/off-camera-phase%u.rgba32f", dir, phase);
            FILE *file = fopen(path, "wb");
            ASSERT_TRUE(file != NULL);
            ASSERT_EQ(fwrite(f->res.lighting, sizeof(f32), (size_t)f->width * f->height * 4, file),
                      (size_t)f->width * f->height * 4);
            ASSERT_EQ(fclose(file), 0);
            snprintf(path, sizeof path, "%s/off-camera-phase%u.ppm", dir, phase);
            file = fopen(path, "wb");
            ASSERT_TRUE(file != NULL);
            fprintf(file, "P6\n%u %u\n255\n", f->width, f->height);
            for (u32 i = 0; i < f->width * f->height; ++i)
                for (u32 channel = 0; channel < 3; ++channel) {
                    const f32 raw = f->res.lighting[i * 4 + channel];
                    const f32 v = isfinite(raw) ? fmaxf(0, raw) : 0;
                    const u8 value = (u8)(255.f * (v / (1.f + v)) + .5f);
                    ASSERT_EQ(fwrite(&value, 1, 1, file), 1);
                }
            ASSERT_EQ(fclose(file), 0);
        }
    }
    EXPECT_EQ(validation_errors(f->dev), 0);
}
#define OFF_CAMERA_TEST(tag, backend)                                                              \
    TEST_F(shadow_atlas, atlas_off_camera_caster_##tag, lx) {                                      \
        off_camera_caster(f, backend);                                                             \
    }
OFF_CAMERA_TEST(metal, DE_GPU_METAL)
OFF_CAMERA_TEST(vulkan, DE_GPU_VULKAN)
OFF_CAMERA_TEST(d3d12, DE_GPU_D3D12)

#define ATLAS_TESTS(tag, backend)                                                                  \
    TEST_F(shadow_atlas, atlas_dirty_caster_rerender_##tag, lx) {                                  \
        cached_removal(f, backend);                                                                \
    }                                                                                              \
    TEST_F(shadow_atlas, atlas_static_cache_zero_renders_##tag, lx) {                              \
        static_cache(f, backend);                                                                  \
    }                                                                                              \
    TEST_F(shadow_atlas, atlas_budget_deterministic_##tag, lx) {                                   \
        budget_movers(f, backend);                                                                 \
    }                                                                                              \
    TEST_F(shadow_atlas, atlas_point_face_coherence_##tag, lx) {                                   \
        point_coherence(f, backend);                                                               \
    }                                                                                              \
    TEST_F(shadow_atlas, atlas_spot_shadow_cpu_ref_##tag, lx) {                                    \
        spot_reference(f, backend);                                                                \
    }                                                                                              \
    TEST_F(shadow_atlas, atlas_point_light_no_seams_##tag, lx) {                                   \
        point_cage(f, backend);                                                                    \
    }                                                                                              \
    TEST_F(shadow_atlas, atlas_receiver_deferred_forward_##tag, lx) {                              \
        atlas_receiver_test(f, backend);                                                           \
    }                                                                                              \
    TEST_F(shadow_atlas, atlas_local_max_deterministic_##tag, lx) {                                \
        local_limit(f, backend);                                                                   \
    }                                                                                              \
    TEST_F(shadow_atlas, atlas_semantic_invalidation_##tag, lx) {                                  \
        invalidation(f, backend);                                                                  \
    }                                                                                              \
    TEST_F(shadow_atlas, atlas_inactive_gap_##tag, lx) {                                           \
        inactive_gap(f, backend);                                                                  \
    }                                                                                              \
    TEST_F(shadow_atlas, atlas_lifecycle_retry_##tag, lx) {                                        \
        lifecycle(f, backend);                                                                     \
    }                                                                                              \
    TEST_F(shadow_atlas, atlas_capture_global_budget_##tag, lx) {                                  \
        capture_progress(f, backend);                                                              \
    }                                                                                              \
    TEST_F(shadow_atlas, atlas_rotating_group_budget_##tag, lx) {                                  \
        rotating_groups(f, backend);                                                               \
    }                                                                                              \
    TEST_F(shadow_atlas, atlas_32_points_192_faces_##tag, lx) {                                    \
        point_capacity(f, backend);                                                                \
    }                                                                                              \
    TEST_F(shadow_atlas, atlas_wide_spot_##tag, lx) {                                              \
        wide_spot(f, backend);                                                                     \
    }                                                                                              \
    TEST_F(shadow_atlas, atlas_ultra_storage_##tag, lx) {                                          \
        ultra_storage(f, backend);                                                                 \
    }
ATLAS_TESTS(metal, DE_GPU_METAL)
ATLAS_TESTS(vulkan, DE_GPU_VULKAN)
ATLAS_TESTS(d3d12, DE_GPU_D3D12)

#define DEFAULT_POINT_TEST(tag, backend)                                                           \
    TEST_F(shadow_atlas, atlas_32_points_default_budget_##tag, lx) {                               \
        point_default_budget(f, backend);                                                          \
    }
DEFAULT_POINT_TEST(metal, DE_GPU_METAL)
DEFAULT_POINT_TEST(vulkan, DE_GPU_VULKAN)
DEFAULT_POINT_TEST(d3d12, DE_GPU_D3D12)

/* No platform/native backend: teardown must restore non-default budgets after an early return. */
TEST(shadow_atlas, atlas_fixture_restores_saved_budgets) {
    lx f = {0};
    lx_setup(&f);
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL}, &f.dev), DE_OK);
    ASSERT_EQ(de__shadow_atlas_create_ex(f.dev, &f.alloc, &f.atlas), DE_OK);
    const i64 original_max = de_cvar_int(de_cvar_find("r.shadow_local_max"));
    const i64 original_updates = de_cvar_int(de_cvar_find("r.shadow_atlas_updates_per_frame"));
    EXPECT_TRUE(cvar_set("r.shadow_local_max", "2"));
    EXPECT_TRUE(cvar_set("r.shadow_atlas_updates_per_frame", "9"));
    lx_save_shadow_budget(&f);
    EXPECT_TRUE(cvar_set("r.shadow_local_max", "1"));
    EXPECT_TRUE(cvar_set("r.shadow_atlas_updates_per_frame", "12"));
    lx_teardown(&f); /* Same cleanup invoked when a fixture assertion leaves its body. */
    EXPECT_EQ(de_cvar_int(de_cvar_find("r.shadow_local_max")), 2);
    EXPECT_EQ(de_cvar_int(de_cvar_find("r.shadow_atlas_updates_per_frame")), 9);
    char value[32];
    snprintf(value, sizeof value, "%lld", (long long)original_max);
    EXPECT_TRUE(cvar_set("r.shadow_local_max", value));
    snprintf(value, sizeof value, "%lld", (long long)original_updates);
    EXPECT_TRUE(cvar_set("r.shadow_atlas_updates_per_frame", value));
}
