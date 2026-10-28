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
#include "render/shadows/contact_internal.h"
#include <stb_image_write.h>
#include <stb_image.h>
#include "flip.h"

typedef struct pcss_query {
    f32 uvz_radius[4];
    u32 control[4];
} pcss_query;
static de_gpu_texture depth_fixture(lx *f, u32 size, u32 pattern, f32 depth) {
    de_gpu_texture texture = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.width = size,
                                       .height = size,
                                       .format = DE_FMT_R32_FLOAT,
                                       .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST,
                                       .name = "pcss.independent_depth"});
    ASSERT_TRUE(texture.id);
    de_gpu_buffer upload =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)size * size * 4u,
                                                           .memory = DE_MEM_UPLOAD,
                                                           .usage = DE_USAGE_COPY_SRC});
    ASSERT_TRUE(upload.id);
    f32 *pixels = de_gpu_buffer_map(f->dev, upload);
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) {
            bool inside =
                x >= size / 4u && x < size * 3u / 4u && y >= size / 4u && y < size * 3u / 4u;
            pixels[y * size + x] = pattern == 0u   ? (x < size / 2u ? depth : 0.f)
                                   : inside        ? depth
                                   : pattern == 1u ? 1.f
                                                   : 0.f;
        }
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "pcss.fixture_upload");
    de_gpu_barrier barrier = {
        .texture = texture, .before = DE_ACCESS_NONE, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_buffer_to_texture(cmd, texture, 0, 0, upload, 0, size * 4u);
    barrier.before = DE_ACCESS_COPY_DST;
    barrier.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(f->dev);
    de_gpu_buffer_destroy(f->dev, upload);
    return texture;
}
static uintptr_t query(lx *f, const void *maps, u32 bytes, u32 stride, const pcss_query *queries,
                       u32 count, f32 *result) {
    /* Reclaim completed queries before allocating again: exercise actual allocation and
     * descriptor reuse, rather than letting deferred destruction keep every query distinct. */
    de__gpu_collect(f->dev);
    const char *entry = "cs_pcss_query";
    de__engine_shader_code code;
    char why[768];
    ASSERT_EQ(de__engine_shader_load(f->dev, &f->alloc, "pcss_query", 0, &entry, 1, &code, why,
                                     sizeof why),
              DE_OK);
    de__vis_kernel kernel;
    const u32 threads[3] = {64u, 1u, 1u};
    ASSERT_EQ(
        de__vis_kernel_create(f->vis, &code, 0, "physical PCSS acceptance", threads, 16, &kernel),
        DE_OK);
    de_gpu_buffer mb = de_gpu_buffer_create(
        f->dev,
        &(de_gpu_buffer_desc){
            .size = bytes, .stride = stride, .memory = DE_MEM_UPLOAD, .usage = DE_USAGE_STORAGE});
    ASSERT_TRUE(mb.id);
    memcpy(de_gpu_buffer_map(f->dev, mb), maps, bytes);
    de_gpu_buffer qb = upload_buf(f->dev, queries, (u64)count * sizeof *queries);
    de_gpu_buffer ob = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){.size = (u64)count * 32u,
                                      .stride = 16u,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    de_gpu_buffer rb =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)count * 32u,
                                                           .memory = DE_MEM_READBACK,
                                                           .usage = DE_USAGE_COPY_DST});
    ASSERT_TRUE(ob.id && rb.id);
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "pcss.production_query");
    de_gpu_barrier barriers[2] = {
        {.buffer = ob, .before = DE_ACCESS_NONE, .after = DE_ACCESS_SHADER_WRITE},
        {.buffer = rb, .before = DE_ACCESS_NONE, .after = DE_ACCESS_COPY_DST}};
    de_gpu_cmd_barriers(cmd, barriers, 2);
    const u32 pc[4] = {de_gpu_buffer_bindless(f->dev, mb), de_gpu_buffer_bindless(f->dev, qb),
                       de_gpu_buffer_bindless(f->dev, ob), count};
    de_gpu_cmd_set_pipeline(cmd, kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, (count + 63u) / 64u, 1, 1);
    barriers[0].before = DE_ACCESS_SHADER_WRITE;
    barriers[0].after = DE_ACCESS_COPY_SRC;
    de_gpu_cmd_barriers(cmd, barriers, 1);
    de_gpu_cmd_copy_buffer(cmd, rb, 0, ob, 0, (u64)count * 32u);
    const u64 signal = ++f->frames;
    ASSERT_EQ(de_gpu_queue_submit(
                  f->dev, DE_QUEUE_GRAPHICS,
                  &(de_gpu_submit){
                      .cmds = &cmd, .cmd_count = 1, .signal = f->fence, .signal_value = signal}),
              DE_OK);
    ASSERT_TRUE(lx_wait(f, signal, "PCSS production query"));
    f->warm = true;
    void *mapped = de_gpu_buffer_map(f->dev, rb);
    ASSERT_TRUE(mapped != NULL);
    const uintptr_t readback_address = (uintptr_t)mapped;
    memcpy(result, mapped, (size_t)count * 32u);
    de__vis_kernel_destroy(f->vis, &kernel);
    de__engine_shader_code_free(&code);
    de_gpu_buffer_destroy(f->dev, mb);
    de_gpu_buffer_destroy(f->dev, qb);
    de_gpu_buffer_destroy(f->dev, ob);
    de_gpu_buffer_destroy(f->dev, rb);
    return readback_address;
}
static f32 reverse_depth(bool perspective, f32 distance) {
    return perspective ? .1f * (20.f - distance) / (distance * 19.9f) : 1.f - distance / 20.f;
}
static de__shadow_map_ref map_for(lx *f, de_gpu_texture tex, bool perspective) {
    return (de__shadow_map_ref){
        .texture = de_gpu_texture_bindless(f->dev, tex, 0, false),
        .taps = 32u,
        .resolution = 1024u,
        .uv_clamp = {.5f / 1024.f, .5f / 1024.f, 1.f - .5f / 1024.f, 1.f - .5f / 1024.f},
        .depth_projection = {perspective ? .1f : 0.f, 20.f, 1.f, perspective ? .25f : .05f},
        .uv_projection = {perspective ? .5f : .05f, perspective ? .5f : .05f, 256.f, 0.f},
        .filter_state = {perspective ? 2u : 1u, DE_QUALITY_ULTRA, 19u, 32u}};
}
static void zero_size(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    de_gpu_texture texture = depth_fixture(f, 1024u, 0u, .7f);
    de__shadow_map_ref maps[2] = {map_for(f, texture, true), map_for(f, texture, false)};
    maps[0].depth_projection[3] = 0.f;
    maps[1].filter_state[1] = DE_QUALITY_LOW;
    pcss_query q[128] = {0};
    f32 out[128 * 8] = {0};
    for (u32 i = 0; i < 128u; ++i) {
        q[i].uvz_radius[0] = .47f + (f32)(i % 16u) * .004f;
        q[i].uvz_radius[1] = .4f + (f32)(i / 16u) * .02f;
        q[i].uvz_radius[2] = i % 3u == 0u ? .8f : .6f;
        q[i].uvz_radius[3] = 1.5f / 1024.f;
        q[i].control[0] = i & 1u;
    }
    query(f, maps, sizeof maps, sizeof maps[0], q, 128u, out);
    for (u32 i = 0; i < 128u; ++i)
        EXPECT_EQ(memcmp(out + i * 8u, out + i * 8u + 1u, sizeof(f32)), 0);
    de_gpu_texture_destroy(f->dev, texture);
}
static double disk_quantile(double probability) {
    double lo = -1., hi = 1.;
    for (u32 i = 0; i < 60u; ++i) {
        double x = (lo + hi) * .5;
        double cdf = .5 + (asin(x) + x * sqrt(1. - x * x)) / PI_D;
        if (cdf < probability)
            lo = x;
        else
            hi = x;
    }
    return (lo + hi) * .5;
}
static void penumbra(lx *f, de_gpu_backend backend, bool perspective) {
    lx_open(f, backend);
    de_gpu_texture texture = depth_fixture(f, 1024u, 0u, reverse_depth(perspective, 2.f));
    de__shadow_map_ref m = map_for(f, texture, perspective);
    enum { X = 161, Y = 64, N = X * Y };
    pcss_query *q = zalloc(N * sizeof *q);
    f32 *out = zalloc(N * 32u);
    double previous_width = 0.;
    const f32 receivers[3] = {3.f, 5.f, 9.f};
    for (u32 k = 0; k < 3u; ++k) {
        const double dr = receivers[k], db = 2.;
        const double radius =
            perspective ? (dr - db) * .25 / (db * dr) * .5 : (dr - db) * tan(.05) / 20.;
        for (u32 x = 0; x < X; ++x)
            for (u32 y = 0; y < Y; ++y) {
                u32 i = x * Y + y;
                q[i].uvz_radius[0] = (f32)(.5 + ((double)x / (X - 1u) * 3. - 1.5) * radius);
                q[i].uvz_radius[1] = .3f + .4f * (f32)y / (Y - 1u);
                q[i].uvz_radius[2] = reverse_depth(perspective, receivers[k]);
                q[i].uvz_radius[3] = 1.5f / 1024.f;
            }
        query(f, &m, sizeof m, sizeof m, q, N, out);
        double crossing[2] = {0., 0.};
        const double levels[2] = {.05, .95};
        for (u32 level = 0; level < 2u; ++level) {
            double prev = 0.;
            bool found = false;
            for (u32 x = 0; x < X; ++x) {
                double mean = 0.;
                for (u32 y = 0; y < Y; ++y) {
                    f32 v = out[(x * Y + y) * 8u];
                    ASSERT_TRUE(isfinite(v) && v >= 0.f && v <= 1.f);
                    mean += v / Y;
                }
                if (!found && mean >= levels[level]) {
                    crossing[level] =
                        ((double)x - 1. + (levels[level] - prev) / fmax(mean - prev, 1e-8)) /
                            (X - 1u) * 3. -
                        1.5;
                    found = true;
                }
                prev = mean;
            }
            ASSERT_TRUE(found);
        }
        const double measured = (crossing[1] - crossing[0]) * radius;
        const double expected = (disk_quantile(.95) - disk_quantile(.05)) * radius;
        printf("%s receiver%.1f blocker2: width %.9f expected %.9f relative %.5f\n",
               perspective ? "perspective" : "directional", dr, measured, expected,
               fabs(measured - expected) / expected);
        EXPECT_TRUE(fabs(measured - expected) <= .15 * expected);
        EXPECT_TRUE(measured > previous_width);
        previous_width = measured;
    }
    free(q);
    free(out);
    de_gpu_texture_destroy(f->dev, texture);
}

static void tile_clamp(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    pcss_query q[128] = {0};
    f32 out[128 * 8] = {0};
    for (u32 mode = 0; mode < 2u; ++mode) {
        de_gpu_texture t = depth_fixture(f, 1024u, mode == 0u ? 1u : 2u, mode == 0u ? .3f : .7f);
        de__shadow_map_ref m = map_for(f, t, false);
        m.depth_projection[3] = .8f;
        m.uv_clamp[0] = m.uv_clamp[1] = 256.5f / 1024.f;
        m.uv_clamp[2] = m.uv_clamp[3] = 767.5f / 1024.f;
        for (u32 i = 0; i < 128u; ++i) {
            q[i].uvz_radius[0] = i & 1u ? 257.f / 1024.f : 766.f / 1024.f;
            q[i].uvz_radius[1] = .26f + .48f * (f32)(i / 2u) / 63.f;
            q[i].uvz_radius[2] = .5f;
            q[i].uvz_radius[3] = .25f;
        }
        query(f, &m, sizeof m, sizeof m, q, 128u, out);
        for (u32 i = 0; i < 128u; ++i)
            EXPECT_FLOAT_EQ(out[i * 8u], mode == 0u ? 1.f : 0.f, 0.f);
        de_gpu_texture_destroy(f->dev, t);
    }
}
static void layout(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    de__csm_gpu csm[2] = {0};
    de__atlas_gpu atlas[2] = {0};
    for (u32 i = 0; i < 2u; ++i) {
        de__shadow_map_ref m = {.texture = 37u + i,
                                .uv_clamp = {.125f, .25f, .5f, .875f},
                                .depth_projection = {.1f, 99.f, .75f, 3.125f + (f32)i},
                                .uv_projection = {.25f, .5f, 127.f + (f32)i, 0.f},
                                .filter_state = {2u, 4u, 0x12345678u + i, 32u}};
        csm[i].map = m;
        atlas[i].map = m;
    }
    pcss_query q[2] = {0};
    f32 out[16];
    for (u32 mode = 1; mode <= 2u; ++mode) {
        for (u32 i = 0; i < 2u; ++i) {
            q[i].control[0] = i;
            q[i].control[1] = mode;
        }
        query(f, mode == 1u ? (void *)csm : (void *)atlas, sizeof csm, sizeof csm[0], q, 2u, out);
        for (u32 i = 0; i < 2u; ++i) {
            u32 texture, frame;
            memcpy(&texture, out + i * 8u, 4);
            memcpy(&frame, out + i * 8u + 3u, 4);
            EXPECT_EQ(texture, csm[i].map.texture);
            EXPECT_EQ(frame, csm[i].map.filter_state[2]);
            EXPECT_FLOAT_EQ(out[i * 8u + 1u], csm[i].map.depth_projection[3], 0.f);
            EXPECT_FLOAT_EQ(out[i * 8u + 2u], csm[i].map.uv_projection[2], 0.f);
            EXPECT_EQ(memcmp(out + i * 8u + 4u, csm[i].map.uv_clamp, 16), 0);
        }
    }
}
static void deterministic(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    de_gpu_texture texture = depth_fixture(f, 1024u, 0u, reverse_depth(true, 2.f));
    de__shadow_map_ref m = map_for(f, texture, true);
    pcss_query q[128] = {0};
    f32 a[128 * 8], b[128 * 8];
    for (u32 i = 0; i < 128u; ++i) {
        q[i].uvz_radius[0] = .47f + .06f * (f32)(i % 32u) / 31.f;
        q[i].uvz_radius[1] = .2f + .1f * (f32)(i / 32u);
        q[i].uvz_radius[2] = reverse_depth(true, 5.f);
    }
    uintptr_t addresses[8];
    addresses[0] = query(f, &m, sizeof m, sizeof m, q, 128u, a);
    bool reused = false;
    for (u32 repeat = 1; repeat < 8u; ++repeat) {
        addresses[repeat] = query(f, &m, sizeof m, sizeof m, q, 128u, b);
        EXPECT_EQ(memcmp(a, b, sizeof a), 0);
        for (u32 prior = 0; prior < repeat; ++prior)
            reused |= addresses[repeat] == addresses[prior];
    }
    if (backend == DE_GPU_VULKAN)
        EXPECT_TRUE(reused); /* the persistently mapped Vulkan heap range was reused */
    ++m.filter_state[2];
    query(f, &m, sizeof m, sizeof m, q, 128u, b);
    u32 changed = 0;
    for (u32 i = 0; i < 128u; ++i)
        changed += a[i * 8u] != b[i * 8u];
    EXPECT_GT(changed, 0u);
    de_gpu_texture_destroy(f->dev, texture);
}
static void contact_scene(lx *f, de_gpu_backend backend, u32 mode) {
    const bool gap = mode != 0u;
    lx_open(f, backend);
    f->width = 320u;
    f->height = 192u;
    f->ortho_height = 2.5f;
    f->eye = (de_vec3){0.f, 0.f, 4.f};
    f->shadow_quality = DE_QUALITY_LOW;
    ASSERT_EQ(de__csm_create_ex(f->dev, &f->alloc, &f->csm), DE_OK);
    ASSERT_EQ(de__contact_create_ex(f->dev, &f->alloc, f->vis, &f->contact), DE_OK);
    const u32 mat = material_add(.5f, .8f, 0.f, .5f);
    const u32 floor = lx_mesh_plane(f, 1u, 1u, 6.f);
    world_set(&f->w, 0u, floor, at(0.f, 0.f, 0.f), mat);
    f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
    if (gap) {
        const u32 blocker = lx_mesh_plane(f, 1u, 1u, .5f);
        world_set(&f->w, 1u, blocker, at(0.f, 0.f, .03f), mat);
        f->w.f.instances.flags[1] |= DE__RW_INST_CAST_SHADOWS;
    }
    DeLight sun = {.kind = DE_LIGHT_DIRECTIONAL,
                   .color = {1.f, 1.f, 1.f},
                   .intensity = 2.f,
                   .cast_shadows = true};
    de_affine transform = at(0.f, 0.f, 0.f);
    transform.m[0] = .6f;
    transform.m[2] = .8f;
    transform.m[6] = -.8f;
    transform.m[8] = .6f;
    world_light(&f->w, 0u, &sun, transform);
    de__ibl_environment environment = {.intensity = 1.f};
    environment.sh9[0][0] = environment.sh9[0][1] = environment.sh9[0][2] = .7f;
    de__lighting_set_environment(f->lt, &environment);
    if (mode >= 2u)
        ASSERT_TRUE(cvar_set("r.ambient_fallback", "0.25"));
    const u32 pixels = f->width * f->height;
    f32 *baseline = zalloc((size_t)pixels * 16u);
    de__contact_config cfg = {
        .length = 0.f, .thickness = .025f, .normal_bias = .001f, .steps = 16u, .max_locals = 4u};
    f->contact_config = &cfg;
    ASSERT_TRUE(lx_frame(f));
    memcpy(baseline, f->res.lighting, (size_t)pixels * 16u);
    cfg.length = .3f;
    ASSERT_TRUE(lx_frame(f));
    de__contact_info info;
    ASSERT_TRUE(de__contact_get(f->contact, (de_view){.id = 1u}, &info));
    EXPECT_EQ(info.metadata.header[3], 1u);
    EXPECT_EQ(info.metadata.light[0].entity[0], 5000u);
    EXPECT_EQ(info.steps, 16u);
    EXPECT_TRUE(f->res.lc.view.inputs.reserved1 == 0u);
    u32 checked = 0u, dark = 0u, unchanged = 0u;
    f32 maximum_relative = 0.f;
    for (u32 i = 0; i < pixels; ++i) {
        const f32 *p = f->res.pos + i * 4u;
        if (p[3] < .5f || fabsf(p[2]) > .001f || fabsf(p[0]) > .9f || fabsf(p[1]) > .9f)
            continue;
        ++checked;
        const f32 before = baseline[i * 4u], after = f->res.lighting[i * 4u];
        ASSERT_TRUE(isfinite(before) && isfinite(after));
        maximum_relative = fmaxf(maximum_relative, fabsf(after - before) / fmaxf(before, 1e-4f));
        dark += after < before * .5f;
        unchanged += memcmp(baseline + i * 4u, f->res.lighting + i * 4u, 12u) == 0;
    }
    printf("contact %s: checked%u dark%u unchanged%u maxRelative%.6f\n", gap ? "gap" : "flat",
           checked, dark, unchanged, maximum_relative);
    EXPECT_GT(checked, 1000u);
    if (gap) {
        EXPECT_GT(dark, 10u);
        EXPECT_GT(unchanged, checked / 2u);
        u8 *image = zalloc((size_t)pixels * 3u);
        for (u32 i = 0; i < pixels; ++i)
            for (u32 c = 0; c < 3u; ++c) {
                f32 x = fmaxf(f->res.lighting[i * 4u + c], 0.f);
                x /= 1.f + x;
                x = x <= .0031308f ? 12.92f * x : 1.055f * powf(x, 1.f / 2.4f) - .055f;
                image[i * 3u + c] = (u8)floorf(fminf(x, 1.f) * 255.f + .5f);
            }
        const char *out = getenv("DE_PCSS_GAP_CAPTURE");
        if (out && out[0])
            EXPECT_TRUE(stbi_write_png(out, (int)f->width, (int)f->height, 3, image,
                                       (int)(f->width * 3u)) != 0);
        if (mode == 1u && backend == DE_GPU_METAL) {
            int w = 0, h = 0, channels = 0;
            u8 *reference = stbi_load(DE_PCSS_GAP_REFERENCE, &w, &h, &channels, 3);
            ASSERT_TRUE(reference != NULL);
            ASSERT_EQ(w, (i32)f->width);
            ASSERT_EQ(h, (i32)f->height);
            f32 *errors = zalloc((size_t)pixels * sizeof *errors);
            de_flip_image a = {reference, f->width, f->height, 3u, 0u};
            de_flip_image b = {image, f->width, f->height, 3u, 0u};
            ASSERT_TRUE(de_flip_ldr(&a, &b, 0, errors));
            de_flip_stats stats;
            ASSERT_TRUE(de_flip_stats_compute(errors, pixels, &stats));
            printf("contact gap golden FLIP mean%.9f p99%.9f\n", stats.mean, stats.p99);
            EXPECT_TRUE(stats.mean <= .005 && stats.p99 <= .05);
            free(errors);
            stbi_image_free(reference);
        }
        free(image);
    } else {
        EXPECT_EQ(dark, 0u);
        EXPECT_TRUE(maximum_relative <= .001f);
    }
    if (mode >= 2u) {
        if (mode == 2u) {
            sun.color = (de_vec3){0.f, 0.f, 0.f};
            world_light(&f->w, 0u, &sun, transform);
        } else {
            const u32 receiver = material_add(.5f, .8f, 0.f, .5f);
            g_mat[receiver - 1u].alpha_mode = DE_COOKED_ALPHA_BLEND;
            g_mat[receiver - 1u].standard.alpha_mode = DE_COOKED_ALPHA_BLEND;
            const u32 mesh = lx_mesh_plane(f, 1u, 1u, 2.f);
            world_set(&f->w, 2u, mesh, at(0.f, 0.f, .06f), receiver);
            ASSERT_EQ(de__forward_create_ex(f->vis, f->lt, &f->forward), DE_OK);
        }
        cfg.length = 0.f;
        ASSERT_TRUE(lx_frame(f));
        memcpy(baseline, f->res.lighting, (size_t)pixels * 16u);
        cfg.length = .3f;
        ASSERT_TRUE(lx_frame(f));
        u32 isolated = 0u;
        for (u32 i = 0; i < pixels; ++i) {
            const f32 *p = f->res.pos + i * 4u;
            if (p[3] < .5f || fabsf(p[2]) > .001f || fabsf(p[0]) > .8f || fabsf(p[1]) > .8f)
                continue;
            EXPECT_TRUE(baseline[i * 4u] > .01f);
            EXPECT_EQ(memcmp(baseline + i * 4u, f->res.lighting + i * 4u, 12u), 0);
            ++isolated;
        }
        EXPECT_GT(isolated, 1000u);
        printf("contact %s isolation: %u bitwise-equal pixels\n", mode == 2u ? "IBL" : "forward",
               isolated);
        ASSERT_TRUE(cvar_set("r.ambient_fallback", "0"));
    }
    free(baseline);
}
TEST(pcss_contact, contact_view_selection_precedes_the_local_budget) {
    u64 ids[] = {10, 20, 30};
    de_affine transforms[] = {at(0, 0, 4), at(0, 0, 4), at(0, 0, 4)};
    DeLight lights[] = {{.kind = DE_LIGHT_DIRECTIONAL,
                         .intensity = 100,
                         .color = {1, 1, 1},
                         .view_mask = 2,
                         .cast_shadows = true},
                        {.kind = DE_LIGHT_POINT,
                         .intensity = 1000,
                         .color = {1, 1, 1},
                         .view_mask = 2,
                         .cast_shadows = true},
                        {.kind = DE_LIGHT_POINT,
                         .intensity = 1,
                         .color = {1, 1, 1},
                         .view_mask = 1,
                         .cast_shadows = true}};
    de__rw_lights world = {.count = 3, .entity = ids, .transform = transforms, .light = lights};
    de__contact_metadata selected;
    de__contact_select_view(&world, ids[0], 1, .05f, ~1u, NULL, &selected);
    EXPECT_EQ(selected.header[3], 0u);
    EXPECT_EQ(selected.header[2], 1u);
    EXPECT_EQ(selected.light[1].entity[0], 30u);
    const de__view_hidden *hidden = NULL;
    ASSERT_EQ(de__view_hidden_create(de_heap_allocator(), &ids[2], 1, &hidden), DE_OK);
    de__contact_select_view(&world, ids[0], 1, .05f, ~1u, hidden, &selected);
    EXPECT_EQ(selected.header[2], 0u);
    de__view_hidden_release(hidden);
    de__contact_select_view(&world, ids[0], 1, .05f, ~0u, NULL, &selected);
    EXPECT_EQ(selected.header[2], 0u);
    EXPECT_EQ(selected.header[3], 0u);
}
TEST(pcss_contact, contact_physical_selection_and_identity) {
    u64 ids[8] = {900u, 100u, 5004u, 5003u, 5002u, 5001u, 5000u, 4999u};
    de_affine transforms[8];
    DeLight light[8] = {0};
    for (u32 i = 0; i < 8u; ++i) {
        transforms[i] = at(0.f, 0.f, 4.f);
        light[i] = (DeLight){.kind = i < 2u ? DE_LIGHT_DIRECTIONAL : DE_LIGHT_POINT,
                             .intensity = 100.f,
                             .color = {1.f, 1.f, 1.f},
                             .range = 8.f,
                             .cast_shadows = true};
    }
    light[7].cast_shadows = false;
    de__rw_lights w = {
        .count = 8u, .live = 8u, .entity = ids, .transform = transforms, .light = light};
    de__contact_metadata a, b;
    de__contact_select(&w, 900u, 4u, .05f, &a);
    EXPECT_EQ(a.header[3], 1u);
    EXPECT_EQ(a.light[0].entity[0], 900u); // Match CSM's actual selection, not smallest entity.
    for (u32 i = 0; i < 4u; ++i)
        EXPECT_EQ(a.light[i + 1u].entity[0], 5000u + i);
    for (u32 i = 0; i < 4u; ++i) {
        u64 id = ids[i];
        ids[i] = ids[7u - i];
        ids[7u - i] = id;
        DeLight l = light[i];
        light[i] = light[7u - i];
        light[7u - i] = l;
    }
    de__contact_select(&w, 900u, 4u, .05f, &b);
    EXPECT_EQ(memcmp(&a, &b, sizeof a), 0);
}

#define PCSS_TESTS(tag, backend)                                                                   \
    TEST_F(pcss_contact, pcss_zero_size_equals_pcf_##tag, lx) {                                    \
        zero_size(f, backend);                                                                     \
    }                                                                                              \
    TEST_F(pcss_contact, pcss_penumbra_linear_##tag, lx) {                                         \
        penumbra(f, backend, true);                                                                \
    }                                                                                              \
    TEST_F(pcss_contact, pcss_directional_angular_##tag, lx) {                                     \
        penumbra(f, backend, false);                                                               \
    }                                                                                              \
    TEST_F(pcss_contact, pcss_atlas_tile_clamp_##tag, lx) {                                        \
        tile_clamp(f, backend);                                                                    \
    }                                                                                              \
    TEST_F(pcss_contact, pcss_gpu_layout_##tag, lx) {                                              \
        layout(f, backend);                                                                        \
    }                                                                                              \
    TEST_F(pcss_contact, pcss_frame_deterministic_##tag, lx) {                                     \
        deterministic(f, backend);                                                                 \
    }                                                                                              \
    TEST_F(pcss_contact, contact_shadows_gap_golden_##tag, lx) {                                   \
        contact_scene(f, backend, 1u);                                                             \
    }                                                                                              \
    TEST_F(pcss_contact, contact_shadows_no_acne_##tag, lx) {                                      \
        contact_scene(f, backend, 0u);                                                             \
    }                                                                                              \
    TEST_F(pcss_contact, contact_direct_only_##tag, lx) {                                          \
        contact_scene(f, backend, 2u);                                                             \
    }                                                                                              \
    TEST_F(pcss_contact, contact_forward_neutral_##tag, lx) {                                      \
        contact_scene(f, backend, 3u);                                                             \
    }
PCSS_TESTS(metal, DE_GPU_METAL)
PCSS_TESTS(vulkan, DE_GPU_VULKAN)
PCSS_TESTS(d3d12, DE_GPU_D3D12)
