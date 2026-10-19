#define SW 64u
#define SH 32u
#include "rt_shadow_fixture.h"
#include "render/rt/rtao.h"

typedef struct rtao_fixture {
    shadow_fixture shadow;
    de_gpu_buffer rb;
    de__rtao_info info;
    f64 accumulated[SW * SH], raw_sum[SW * SH];
    const de__lighting_view *lighting;
    bool rt_pass, gtao_pass, ao_input, canonical_reset;
} rtao_fixture;
static void graph_stage(de__view_frame *vf, void *user) {
    rtao_fixture *f = user;
    if (vf->view.id != f->shadow.rt.view.id)
        return;
    const de__vis_context *vc =
        de__vis_frame_context(de__views_vis(vf->renderer), vf->view, vf->graph, vf->frame_index);
    f->canonical_reset = vc && (vc->view_new || vc->occlusion_reset);
    f->rt_pass = false;
    f->gtao_pass = false;
    for (u32 i = 0; i < vf->graph->pass_count; ++i) {
        const char *name = vf->graph->passes[i]->name;
        f->rt_pass |= strcmp(name, "RT.AO") == 0;
        f->gtao_pass |= strcmp(name, "gtao.raw") == 0;
    }
    de__lighting *lt = de__views_lighting(f->shadow.rt.r);
    const de__lighting_view_state *state = &lt->views[(vf->view.id & 255u) - 1u];
    f->lighting = (const de__lighting_view *)(state->constants_map +
                                              (size_t)state->entry * sizeof(de__lighting_view));
}
static void rtao_fixture_setup(rtao_fixture *f) {
    shadow_fixture_setup(&f->shadow);
    rt_fixture *r = &f->shadow.rt;
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.rt_shadows = false;
    settings.shadows = DE_QUALITY_OFF;
    settings.ao = DE_QUALITY_HIGH;
    settings.rt_ao = true;
    de_renderer_set_settings(r->r, &settings);
    g_mat[1].standard.double_sided = true;
    world_set(&r->w, 1, 2, trs((de_vec3){1, 0, 2}, (de_vec3){0, 1, 0}, DE_PI / 2, 2), 2);
    world_set(&r->w, 2, 2, trs((de_vec3){3, -1.5f, 2}, (de_vec3){1, 0, 0}, DE_PI / 2, 2), 2);
    for (u32 i = 1; i < 3; ++i) {
        r->w.f.instances.flags[i] |= DE__RW_INST_VISIBLE_IN_RT;
        r->w.f.instances.layer_mask[i] = 1u << 24;
    }
    ASSERT_TRUE(cvar_set("r.rt.ao.radius", "1.5"));
    f->rb = de_gpu_buffer_create(r->d, &(de_gpu_buffer_desc){.size = 65536,
                                                             .usage = DE_USAGE_COPY_DST,
                                                             .memory = DE_MEM_READBACK});
    ASSERT_TRUE(f->rb.id != 0);
    ASSERT_EQ(de__view_register_stage(r->r, DE__VIEW_SLOT_LIGHTING, 2, "RTAO graph acceptance",
                                      graph_stage, f),
              DE_OK);
}
static void rtao_fixture_teardown(rtao_fixture *f) {
    if (f->shadow.rt.d) {
        de_gpu_wait_idle(f->shadow.rt.d);
        de_gpu_buffer_destroy(f->shadow.rt.d, f->rb);
    }
    (void)cvar_set("r.rt.ao.radius", "1.5");
    (void)cvar_set("r.rt.golden_sample", "-1");
    (void)cvar_set("r.denoise.reset", "0");
    shadow_fixture_teardown(&f->shadow);
}
static const u8 *ao_frame(rtao_fixture *f) {
    rt_fixture *r = &f->shadow.rt;
    shadow_frame(&f->shadow);
    f->ao_input = f->lighting && f->lighting->inputs.ao != 0 &&
                  (f->lighting->inputs.reserved0 & DE__INPUT_AO_GTAO) != 0;
    ASSERT_TRUE(de__rtao_get(r->r->rtao, r->view, &f->info));
    de_gpu_cmd *c = de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "RTAO acceptance readback");
    de_gpu_barrier b[3] = {
        {.texture = f->info.ao, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC},
        {.texture = f->info.distance, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC},
        {.texture = f->info.raw, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC}};
    de_gpu_cmd_barriers(c, b, 3);
    de_gpu_cmd_copy_texture_to_buffer(c, f->rb, 0, 256, f->info.ao, 0, 0);
    de_gpu_cmd_copy_texture_to_buffer(c, f->rb, 16384, 256, f->info.distance, 0, 0);
    de_gpu_cmd_copy_texture_to_buffer(c, f->rb, 24576, 512, f->info.raw, 0, 0);
    for (u32 i = 0; i < 3; ++i) {
        b[i].before = DE_ACCESS_COPY_SRC;
        b[i].after = DE_ACCESS_SHADER_READ;
    }
    de_gpu_cmd_barriers(c, b, 3);
    ASSERT_EQ(
        de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &c, .cmd_count = 1}),
        DE_OK);
    de_gpu_wait_idle(r->d);
    return de_gpu_buffer_map(r->d, f->rb);
}
static f64 cpu_visibility(f64 x, f64 y, f64 radius) {
    /* Independent double-precision cosine hemisphere and exact finite plane intersections. */
    const u32 count = 262144;
    u32 visible = 0;
    for (u32 i = 0; i < count; ++i) {
        u32 bits = i;
        bits = (bits << 16) | (bits >> 16);
        bits = ((bits & 0x00ff00ffu) << 8) | ((bits & 0xff00ff00u) >> 8);
        bits = ((bits & 0x0f0f0f0fu) << 4) | ((bits & 0xf0f0f0f0u) >> 4);
        bits = ((bits & 0x33333333u) << 2) | ((bits & 0xccccccccu) >> 2);
        bits = ((bits & 0x55555555u) << 1) | ((bits & 0xaaaaaaaau) >> 1);
        f64 u = ((f64)i + .5) / count,
            phi = 6.2831853071795864769 * ((f64)bits + .5) / 4294967296.0;
        f64 dx = sqrt(u) * cos(phi), dy = sqrt(u) * sin(phi), dz = sqrt(1 - u), t;
        bool hit = false;
        if (dx < 0) {
            t = (1 - x) / dx;
            f64 yy = y + t * dy, z = t * dz;
            hit |= t > 0 && t <= radius && yy >= -2 && yy <= 2 && z <= 4;
        }
        if (dy < 0) {
            t = (-1.5 - y) / dy;
            f64 xx = x + t * dx, z = t * dz;
            hit |= t > 0 && t <= radius && xx >= 1 && xx <= 5 && z <= 4;
        }
        visible += !hit;
    }
    return (f64)visible / count;
}
TEST_F(rtao, reference_corner, rtao_fixture) {
    const u32 probes[5][2] = {{18, 25}, {20, 25}, {18, 21}, {24, 21}, {36, 10}};
    for (u32 frame = 0; frame < 64; ++frame) {
        const u8 *ao = ao_frame(f);
        for (u32 i = 0; i < SW * SH; ++i) {
            f->accumulated[i] += (f64)ao[(i / SW) * 256 + i % SW] / (255 * 64);
            f->raw_sum[i] +=
                half_to_f32(*(const u16 *)(ao + 24576 + (i / SW) * 512 + (i % SW) * 8)) / 64;
        }
    }
    EXPECT_TRUE(f->rt_pass);
    EXPECT_FALSE(f->gtao_pass);
    EXPECT_TRUE(f->ao_input);
    for (u32 i = 0; i < 5; ++i) {
        u32 x = probes[i][0], y = probes[i][1];
        f64 wx = -1 + ((f64)x + .5) * 8 / SW, wy = 2 - ((f64)y + .5) * 4 / SH;
        f64 reference = cpu_visibility(wx, wy, 1.5), actual = f->accumulated[y * SW + x],
            error = fabs(actual - reference) / reference;
        printf("corner probe%u (%g,%g): GPU %.8f CPU %.8f relative error %.8f\n", i, wx, wy, actual,
               reference, error);
        printf("raw64 %.8f relative error %.8f\n", f->raw_sum[y * SW + x],
               fabs(f->raw_sum[y * SW + x] - reference) / reference);
        EXPECT_TRUE(error <= .03);
    }
    const u8 *bytes = de_gpu_buffer_map(f->shadow.rt.d, f->rb);
    for (u32 i = 0; i < 5; ++i) {
        u32 x = probes[i][0], y = probes[i][1];
        f32 distance = half_to_f32(*(const u16 *)(bytes + 16384 + y * 256 + x * 2));
        EXPECT_TRUE(distance > 0 && distance <= 1.505f);
        if (i == 4)
            EXPECT_FLOAT_EQ(distance, 1.5, .002);
    }
    EXPECT_EQ(f->lighting->inputs.ao,
              de_gpu_texture_bindless(f->shadow.rt.d, f->info.ao, 0, false));
    EXPECT_EQ(f->info.rays, (u64)SW * SH);
    EXPECT_EQ(de_renderer_stats(f->shadow.rt.r)->rays_traced, f->info.rays);
    EXPECT_EQ(f->info.spp, 1u);
}
TEST_F(rtao, radius_quality_and_fallback, rtao_fixture) {
    ASSERT_TRUE(cvar_set("r.rt.ao.radius", ".1"));
    const u8 *ao = ao_frame(f);
    f64 short_ao = 0;
    for (u32 y = 8; y < 28; ++y)
        for (u32 x = 18; x < 26; ++x)
            short_ao += ao[y * 256 + x];
    ASSERT_TRUE(cvar_set("r.rt.ao.radius", "2"));
    f64 long_ao = 0;
    for (u32 frame = 0; frame < 16; ++frame) {
        ao = ao_frame(f);
        for (u32 y = 8; y < 28; ++y)
            for (u32 x = 18; x < 26; ++x)
                long_ao += ao[y * 256 + x] / 16.0;
    }
    printf("runtime radius AO sum short%g long%g\n", short_ao, long_ao);
    EXPECT_TRUE(long_ao < short_ao * .8);
    rt_fixture *r = &f->shadow.rt;
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.ao = DE_QUALITY_ULTRA;
    de_renderer_set_settings(r->r, &settings);
    ao_frame(f);
    EXPECT_EQ(f->info.spp, 2u);
    EXPECT_EQ(f->info.rays, (u64)SW * SH * 2);
    settings.ao = DE_QUALITY_LOW;
    de_renderer_set_settings(r->r, &settings);
    ao_frame(f);
    EXPECT_EQ(f->info.trace_width, SW / 2);
    EXPECT_EQ(f->info.trace_height, SH / 2);
    EXPECT_EQ(f->info.rays, (u64)SW * SH / 4);
    /* Both producers share TLAS/query buffers but retain independent per-dispatch flags. */
    settings.ao = DE_QUALITY_HIGH;
    settings.shadows = DE_QUALITY_HIGH;
    settings.rt_shadows = true;
    de_renderer_set_settings(r->r, &settings);
    world_light(&r->w, 0,
                &(DeLight){.kind = DE_LIGHT_POINT,
                           .color = {1, 1, 1},
                           .intensity = 100,
                           .range = 10,
                           .cast_shadows = true},
                at(3, 0, 3));
    ASSERT_TRUE(cvar_set("r.rt.restir_di", "1"));
    ao_frame(f);
    EXPECT_TRUE(f->rt_pass);
    EXPECT_TRUE(f->shadow.di_active);
    EXPECT_TRUE(f->ao_input);
    ASSERT_TRUE(cvar_set("r.rt.restir_di", "0"));
    settings.rt_ao = false;
    de_renderer_set_settings(r->r, &settings);
    shadow_frame(&f->shadow);
    EXPECT_FALSE(f->rt_pass);
    EXPECT_TRUE(f->gtao_pass);
    EXPECT_TRUE(f->lighting && f->lighting->inputs.ao != 0 &&
                (f->lighting->inputs.reserved0 & DE__INPUT_AO_GTAO) != 0);
    EXPECT_FALSE(de__rtao_get(r->r->rtao, r->view, &f->info));
}
TEST_F(rtao, no_heap_after_warmup, rtao_fixture) {
    for (u32 i = 0; i < 24; ++i)
        ao_frame(f);
    de_tracking_stats cpu = de_tracking_allocator_stats(&f->shadow.rt.track),
                      gpu = de_tracking_allocator_stats(&f->shadow.rt.dev_track);
    for (u32 i = 0; i < 8; ++i)
        ao_frame(f);
    EXPECT_EQ(de_tracking_allocator_stats(&f->shadow.rt.track).alloc_count, cpu.alloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&f->shadow.rt.track).realloc_count, cpu.realloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&f->shadow.rt.dev_track).alloc_count, gpu.alloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&f->shadow.rt.dev_track).realloc_count,
              gpu.realloc_count);
}

TEST(rtao, fallback_graph) {
    de_gpu_device *d = NULL;
    de_type_registry *types = NULL;
    de_world *w = NULL;
    de_renderer *r = NULL;
    ASSERT_EQ(de_type_registry_create(NULL, &types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = types}, &w), DE_OK);
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL}, &d), DE_OK);
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = d, .types = types}, &r), DE_OK);
    de_renderer_attach(r, w);
    ASSERT_TRUE(cvar_set("r.pipelined", "0"));
    de_render_settings settings = {
        .tier = DE_TIER_RT, .ao = DE_QUALITY_HIGH, .rt_ao = true, .render_scale = 1};
    de_renderer_set_settings(r, &settings);
    EXPECT_EQ(de_renderer_get_settings(r).tier, DE_TIER_BASE);
    const DeRenderComponents ids = *de__render_components(w);
    const de_builtin_component_ids bi = de_builtin_components(w);
    de_entity camera = DE_ENTITY_NULL, mesh = DE_ENTITY_NULL;
    const de_component cc[2] = {bi.transform, ids.camera},
                       mc[2] = {bi.transform, ids.mesh_renderer};
    ASSERT_EQ(de_entity_spawn_with(w, cc, 2, &camera), DE_OK);
    DeTransform *ct = de_entity_get(w, camera, bi.transform);
    ct->position = (de_dvec3){0, 0, 8};
    ct->rotation = (de_quat){0, 0, 0, 1};
    ct->scale = (de_vec3){1, 1, 1};
    ASSERT_EQ(de_entity_spawn_with(w, mc, 2, &mesh), DE_OK);
    DeTransform *mt = de_entity_get(w, mesh, bi.transform);
    mt->rotation = (de_quat){0, 0, 0, 1};
    mt->scale = (de_vec3){1, 1, 1};
    ((DeMeshRenderer *)de_entity_get(w, mesh, ids.mesh_renderer))->mesh.id = 1;
    de__gpu_null_log_enable(d, true);
    ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_POST_UPDATE, 1.f / 60, 0), DE_OK);
    ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_EXTRACT, 1.f / 60, 0), DE_OK);
    ASSERT_EQ(de__render_extract_and_kick(r, w), DE_OK);
    de__gpu_null_log_enable(d, false);
    bool gtao = false, rt = false;
    for (u32 i = 0; i < de__gpu_null_log_count(d); ++i) {
        const de__gpu_null_entry *e = de__gpu_null_log_entry(d, i);
        if (e->op == DE__GPU_NULL_OP_MARKER_PUSH) {
            gtao |= strcmp(e->text, "gtao.raw") == 0;
            rt |= strcmp(e->text, "RT.AO") == 0;
        }
    }
    EXPECT_TRUE(gtao);
    EXPECT_FALSE(rt);
    (void)cvar_set("r.pipelined", "1");
    de_renderer_destroy(r);
    de_world_destroy(w);
    de_gpu_device_destroy(d);
    de_type_registry_destroy(types);
}

TEST_F(rtao, alpha_without_shadow_flag, rtao_fixture) {
    rt_fixture *r = &f->shadow.rt;
    for (u32 i = 1; i < 3; ++i)
        r->w.f.instances.flags[i] &= ~DE__RW_INST_CAST_SHADOWS;
    for (u32 frame = 0; frame < 32; ++frame) {
        const u8 *ao = ao_frame(f);
        for (u32 i = 0; i < SW * SH; ++i)
            f->accumulated[i] += ao[(i / SW) * 256 + i % SW] / (255.0 * 32);
    }
    EXPECT_TRUE(f->accumulated[25 * SW + 18] < .6);
    g_mat[1].alpha_mode = DE_COOKED_ALPHA_MASK;
    g_mat[1].standard.alpha_mode = DE_COOKED_ALPHA_MASK;
    g_mat[1].standard.alpha_cutoff = .5f;
    g_mat[1].standard.base_color_tex.id = 1;
    g_mat[1].textures[0].id = 1;
    for (u32 frame = 0; frame < 32; ++frame) {
        const u8 *ao = ao_frame(f);
        for (u32 i = 0; i < SW * SH; ++i)
            f->raw_sum[i] += ao[(i / SW) * 256 + i % SW] / (255.0 * 32);
    }
    u32 openings = 0, partial = 0;
    for (u32 i = 0; i < SW * SH; ++i) {
        if (f->raw_sum[i] > f->accumulated[i] + .1)
            ++openings;
        if (f->raw_sum[i] < .95 && f->raw_sum[i] > f->accumulated[i] + .08)
            ++partial;
    }
    printf("AO alpha texture: %u lighter cutout pixels, %u partial visibility pixels\n", openings,
           partial);
    EXPECT_TRUE(openings > 20);
    EXPECT_TRUE(partial > 10);
    g_mat[1].standard.base_color.w = 0;
    const u8 *ao = ao_frame(f);
    EXPECT_EQ(ao[25 * 256 + 18], 255u);
}

TEST_F(rtao, radius_changes_match_fresh_history_without_resetting_view_guides, rtao_fixture) {
    ASSERT_TRUE(cvar_set("r.rt.golden_sample", "0"));
    ASSERT_TRUE(cvar_set("r.rt.ao.radius", "2"));
    for (u32 i = 0; i < 16; ++i)
        ao_frame(f);
    /* An unchanged radius must retain temporal history when the ray sample changes. */
    ASSERT_TRUE(cvar_set("r.rt.golden_sample", "7"));
    const u8 *bytes = ao_frame(f);
    u8 retained[SW * SH], changed[SW * SH];
    u16 changed_distance[SW * SH];
    for (u32 y = 0; y < SH; ++y)
        memcpy(retained + y * SW, bytes + y * 256, SW);
    ASSERT_TRUE(cvar_set("r.denoise.reset", "1"));
    bytes = ao_frame(f);
    u32 retained_difference = 0;
    for (u32 y = 0; y < SH; ++y)
        for (u32 x = 0; x < SW; ++x)
            retained_difference += retained[y * SW + x] != bytes[y * 256 + x];
    printf("unchanged-radius temporal output differs from fresh history at %u pixels\n",
           retained_difference);
    EXPECT_GT(retained_difference, 0u);
    const char *radii[] = {".1", "2", ".2", "1.5", "100", ".01"};
    for (u32 i = 0; i < sizeof radii / sizeof radii[0]; ++i) {
        ASSERT_TRUE(cvar_set("r.rt.ao.radius", radii[i]));
        bytes = ao_frame(f);
        EXPECT_FALSE(f->canonical_reset);
        for (u32 y = 0; y < SH; ++y) {
            memcpy(changed + y * SW, bytes + y * 256, SW);
            memcpy(changed_distance + y * SW, bytes + 16384 + y * 256, SW * sizeof(u16));
        }
        /* Explicit shared-denoiser reset is an independent fresh-history oracle.
         * Both frames trace identical rays against identical geometry. */
        char reset[16];
        snprintf(reset, sizeof reset, "%u", i + 2);
        ASSERT_TRUE(cvar_set("r.denoise.reset", reset));
        bytes = ao_frame(f);
        u32 differences = 0;
        for (u32 y = 0; y < SH; ++y)
            for (u32 x = 0; x < SW; ++x) {
                u16 distance;
                memcpy(&distance, bytes + 16384 + y * 256 + x * sizeof distance, sizeof distance);
                differences += changed[y * SW + x] != bytes[y * 256 + x];
                differences += changed_distance[y * SW + x] != distance;
            }
        printf("radius %s immediate/fresh visibility+distance differences: %u\n", radii[i],
               differences);
        EXPECT_EQ(differences, 0u);
    }
}
