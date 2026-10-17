/* Actual renderer RT shadow production integration, using the shared native scene fixture. */
#include "scene_as_fixture.h"
#include "render/rt/rt_shadows.h"
#define SW 256u
#define SH 128u
typedef struct shadow_fixture {
    rt_fixture rt;
    de__rw_view camera;
    de_gpu_buffer mask_rb, hdr_rb;
    f32 sum[SW * SH];
    bool rt_pass, raster;
} shadow_fixture;
typedef struct hdr_pass {
    shadow_fixture *f;
    de_rg_resource target;
} hdr_pass;
static void hdr_copy(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    hdr_pass *p = user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, p->f->hdr_rb, 0, SW * 8, de_rg_texture(pass, p->target),
                                      0, 0);
}
static void hdr_stage(de__view_frame *vf, void *user) {
    shadow_fixture *f = user;
    if (vf->view.id != f->rt.view.id)
        return;
    const de__lighting_context *lc = de__lighting_frame_context(
        de__views_lighting(f->rt.r), vf->view, vf->graph, vf->frame_index);
    ASSERT_TRUE(lc != NULL);
    f->rt_pass = false;
    f->raster = lc->shadow_records.id != 0;
    for (u32 i = 0; i < vf->graph->pass_count; ++i)
        if (strcmp(vf->graph->passes[i]->name, "RT.Shadows") == 0)
            f->rt_pass = true;
    hdr_pass *p = de_arena_push_zero(vf->graph->arena, sizeof *p, 16);
    ASSERT_TRUE(p != NULL);
    p->f = f;
    p->target = lc->target;
    de_rg_pass *pass =
        de_rg_add_pass(vf->graph, "RT shadow HDR acceptance", DE_QUEUE_GRAPHICS, hdr_copy, p);
    de_rg_read(pass, p->target, DE_ACCESS_COPY_SRC);
    de_rg_write(pass, de_rg_import_buffer(vf->graph, f->hdr_rb, DE_ACCESS_COPY_DST),
                DE_ACCESS_COPY_DST);
    de_rg_pass_side_effect(pass);
}
static void shadow_fixture_setup(shadow_fixture *f) {
    rt_fixture *r = &f->rt;
    rt_fixture_setup(r);
    de__view_unregister_stage(r->r, trace_stage, r);
    de_view_destroy(r->r, r->view);
    de_gpu_texture_destroy(r->d, r->target);
    r->target = de_gpu_texture_create(
        r->d, &(de_gpu_texture_desc){.width = SW,
                                     .height = SH,
                                     .format = DE_FMT_RGBA8_UNORM,
                                     .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED});
    r->view = de_view_create(
        r->r, &(de_view_desc){.target = r->target, .viewport = {0, 0, 1, 1}, .name = "RT shadows"});
    de_view_set_layer_mask(r->r, r->view, 1u << 24);
    f->camera = (de__rw_view){.entity = 999,
                              .camera = {.projection = DE_PROJ_ORTHOGRAPHIC,
                                         .ortho_height = 4,
                                         .near_z = .1f,
                                         .view = r->view,
                                         .exposure_ev100 = 0},
                              .offset = {3, 0, 8},
                              .rotation = {0, 0, 0, 1},
                              .transform = at(3, 0, 8)};
    r->w.f.views = &f->camera;
    r->w.f.view_count = 1;
    r->w.f.main_view = 0;
    g_mat[0].alpha_mode = DE_COOKED_ALPHA_OPAQUE;
    g_mat[0].standard.alpha_mode = DE_COOKED_ALPHA_OPAQUE;
    g_mat[0].standard.base_color_tex.id = 0;
    g_mat[0].textures[0].id = 0;
    world_set(&r->w, 0, 1, trs((de_vec3){3, 0, 0}, (de_vec3){0, 0, 1}, 0, 4), 1);
    r->w.f.instances.flags[0] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
    r->w.f.instances.layer_mask[0] = 1u << 24;
    mesh_plane(&g_mesh[1], r->d, 1, 1, 2);
    g_mesh_count = 2;
    de_render_mesh *m = &g_mesh[1].rm;
    m->buffers[DE_RENDER_MESH_INDICES] = r->indices;
    m->bindless[DE_RENDER_MESH_INDICES] = de_gpu_buffer_bindless(r->d, r->indices);
    m->index_count = g_mesh[0].rm.lods[0].index_count;
    m->lods[0].index_count = m->index_count;
    m->lod_submeshes = r->ranges;
    m->submeshes = &r->submesh;
    ASSERT_EQ(de__rt_mesh_publish(r->r->rt_scene, m), DE_OK);
    g_mat[1] = g_mat[0];
    g_mat_count = 2;
    world_set(&r->w, 1, 2, trs((de_vec3){3, 0, 1}, (de_vec3){0, 0, 1}, 0, .4f), 2);
    r->w.f.instances.flags[1] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
    r->w.f.instances.layer_mask[1] = 1u << 24;
    DeLight sun = {.kind = DE_LIGHT_DIRECTIONAL,
                   .color = {1, 1, 1},
                   .intensity = 10,
                   .source_radius = .1f,
                   .cast_shadows = true};
    world_light(&r->w, 0, &sun, at(0, 0, 0));
    de_render_settings settings = {
        .tier = DE_TIER_RT, .shadows = DE_QUALITY_HIGH, .rt_shadows = true, .render_scale = 1};
    de_renderer_set_settings(r->r, &settings);
    (void)cvar_set("r.occlusion", "0");
    f->mask_rb = de_gpu_buffer_create(r->d, &(de_gpu_buffer_desc){.size = SW * SH * 2 * 4,
                                                                  .usage = DE_USAGE_COPY_DST,
                                                                  .memory = DE_MEM_READBACK});
    f->hdr_rb = de_gpu_buffer_create(r->d, &(de_gpu_buffer_desc){.size = SW * SH * 8,
                                                                 .usage = DE_USAGE_COPY_DST,
                                                                 .memory = DE_MEM_READBACK});
    ASSERT_EQ(
        de__view_register_stage(r->r, DE__VIEW_SLOT_LIGHTING, 1, "RT shadow HDR", hdr_stage, f),
        DE_OK);
}
static void shadow_fixture_teardown(shadow_fixture *f) {
    rt_fixture *r = &f->rt;
    if (r->d) {
        de_gpu_wait_idle(r->d);
        mesh_free(&g_mesh[1], r->d);
        de_gpu_buffer_destroy(r->d, f->mask_rb);
        de_gpu_buffer_destroy(r->d, f->hdr_rb);
    }
    rt_fixture_teardown(r);
}
static void shadow_frame(shadow_fixture *f) {
    rt_fixture *r = &f->rt;
    r->w.touch_all = true;
    r->w.touch_lights = true;
    world_begin(&r->w);
    r->handoff.last = &r->w.f;
    r->handoff.current = &r->w.f;
    r->r->render_world = &r->handoff;
    de__renderer_kick_prepare(r->r);
    ASSERT_EQ(de_renderer_render_frame(r->r), DE_OK);
    r->handoff.current = NULL;
    de_gpu_wait_idle(r->d);
    ASSERT_FALSE(de_gpu_device_is_lost(r->d));
}
static const u16 *shadow_read(shadow_fixture *f, de__rt_shadows_info *info) {
    rt_fixture *r = &f->rt;
    ASSERT_TRUE(de__rt_shadows_get(r->r->rt_shadows, r->view, info));
    de_gpu_cmd *c = de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "RT shadow readback");
    de_gpu_barrier b = {
        .texture = info->visibility, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(c, f->mask_rb, 0, SW * 2, info->visibility, 0, 0);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(c, &b, 1);
    ASSERT_EQ(
        de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &c, .cmd_count = 1}),
        DE_OK);
    de_gpu_wait_idle(r->d);
    return de_gpu_buffer_map(r->d, f->mask_rb);
}
TEST(rt_shadows, light_sampling) {
    for (u32 kind = 0; kind < 5; ++kind) {
        de__cluster_light light = {.position = {0, 0, 10, 100},
                                   .direction = {0, 0, -1, .1f},
                                   .shape = {0, 2, 1, 0},
                                   .axis = {1, 0, 0, 0}};
        memcpy(&light.shape[3], &kind, 4);
        for (u32 i = 0; i < 1024; ++i) {
            de_vec2 u = {(i + .5f) / 1024, (f32)((i * 373u) % 1024) / 1024};
            de__rt_ray r = de__rt_shadow_ray(&light, (de_vec3){0}, (de_vec3){0, 0, 1}, u);
            f32 n = r.direction[0] * r.direction[0] + r.direction[1] * r.direction[1] +
                    r.direction[2] * r.direction[2];
            EXPECT_FLOAT_EQ(n, 1, 1e-5);
            EXPECT_TRUE(r.direction[3] > 0);
            if (kind == 0)
                EXPECT_TRUE(r.direction[2] >= cosf(.1f) - 1e-6f);
            else {
                f32 x = r.origin[0] + r.direction[0] * r.direction[3],
                    y = r.origin[1] + r.direction[1] * r.direction[3];
                EXPECT_TRUE(fabsf(x) <= 2.001f && fabsf(y) <= 1.001f);
                if (kind == 4)
                    EXPECT_TRUE(x * x / 4 + y * y <= 1.001f);
            }
        }
    }
}
TEST_F(rt_shadows, toggle, shadow_fixture) {
    shadow_frame(f);
    de__rt_shadows_info info;
    const u16 *mask = shadow_read(f, &info);
    printf("RT shadow extent %u x %u slots%u maskcenter%g rays%llu\n", info.width, info.height,
           info.slots, half_to_f32(mask[(SH / 2) * SW + SW / 2]), (unsigned long long)info.rays);
    EXPECT_TRUE(f->rt_pass);
    EXPECT_EQ(info.width, SW);
    EXPECT_EQ(info.spp, 1u);
    EXPECT_TRUE(de_renderer_stats(f->rt.r)->rays_traced > 0);
    de_gpu_pipeline pipeline = f->rt.r->rt_scene->trace;
    de_render_settings settings = de_renderer_get_settings(f->rt.r);
    settings.rt_shadows = false;
    de_renderer_set_settings(f->rt.r, &settings);
    shadow_frame(f);
    EXPECT_EQ(de_renderer_stats(f->rt.r)->rays_traced, 0u);
    EXPECT_FALSE(f->rt_pass);
    EXPECT_TRUE(f->raster);
    settings.rt_shadows = true;
    de_renderer_set_settings(f->rt.r, &settings);
    shadow_frame(f);
    EXPECT_EQ(f->rt.r->rt_scene->trace.id, pipeline.id);
    settings.shadows = DE_QUALITY_LOW;
    de_renderer_set_settings(f->rt.r, &settings);
    shadow_frame(f);
    ASSERT_TRUE(de__rt_shadows_get(f->rt.r->rt_shadows, f->rt.view, &info));
    EXPECT_EQ(info.width, SW / 2);
    EXPECT_EQ(info.scale, 2u);
    settings.shadows = DE_QUALITY_ULTRA;
    de_renderer_set_settings(f->rt.r, &settings);
    shadow_frame(f);
    ASSERT_TRUE(de__rt_shadows_get(f->rt.r->rt_shadows, f->rt.view, &info));
    EXPECT_EQ(info.spp, 2u);
}
static f32 edge_crossing(const f32 *row, f32 level, u32 lo, u32 hi) {
    for (u32 x = lo + 1; x <= hi; ++x)
        if (row[x] <= level && row[x - 1] > level)
            return (f32)(x - 1) + (row[x - 1] - level) / fmaxf(row[x - 1] - row[x], 1e-6f);
    printf("missing edge level %g [%u,%u], ends %g %g\n", level, lo, hi, row[lo], row[hi]);
    return -1;
}
static f32 measure_width(shadow_fixture *f, f32 height) {
    rt_fixture *r = &f->rt;
    world_set(&r->w, 1, 2, trs((de_vec3){3, 0, height}, (de_vec3){0, 0, 1}, 0, 1), 2);
    r->w.f.instances.flags[1] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
    r->w.f.instances.layer_mask[1] = 1u << 24;
    f32 row[SW] = {0};
    for (u32 frame = 0; frame < 64; ++frame) {
        shadow_frame(f);
        de__rt_shadows_info info;
        const u16 *mask = shadow_read(f, &info);
        for (u32 x = 0; x < SW; ++x)
            for (u32 y = SH / 2 - 4; y < SH / 2 + 4; ++y)
                row[x] += half_to_f32(mask[y * SW + x]) / (64 * 8);
    }
    const f32 edge = 3 - 1 - height * tanf(.55f);
    u32 lo = (u32)((edge - .7f + 1) * SW / 8), hi = (u32)((edge + .7f + 1) * SW / 8);
    if (hi >= SW)
        hi = SW - 1;
    f32 a = edge_crossing(row, .9f, lo, hi), b = edge_crossing(row, .1f, lo, hi);
    ASSERT_TRUE(a >= 0 && b > a);
    printf("blocker height %g, penumbra 10-90 width %g pixels (%g m), edge %g\n", height, b - a,
           (b - a) * 8 / SW, edge);
    return b - a;
}
TEST_F(rt_shadows, penumbra_width, shadow_fixture) {
    rt_fixture *r = &f->rt;
    (void)cvar_set("r.rt.shadows.max_lights", "1");
    r->w.f.lights.light[0].source_radius = .15f;
    r->w.f.lights.transform[0] = trs((de_vec3){0}, (de_vec3){0, 1, 0}, .55f, 1);
    f32 first = measure_width(f, 1), second = measure_width(f, 2);
    printf("penumbra ratio %g, analytic distance ratio 2 (tolerance 10%%)\n", second / first);
    EXPECT_FLOAT_EQ(second / first, 2, .2);
    (void)cvar_set("r.rt.shadows.max_lights", "4");
}
TEST_F(rt_shadows, alpha_mask, shadow_fixture) {
    rt_fixture *r = &f->rt;
    (void)cvar_set("r.rt.shadows.max_lights", "1");
    r->w.f.lights.light[0].source_radius = 0;
    r->w.f.lights.transform[0] = trs((de_vec3){0}, (de_vec3){0, 1, 0}, .65f, 1);
    world_set(&r->w, 1, 2, trs((de_vec3){3, 0, 2}, (de_vec3){0, 0, 1}, 0, .6f), 2);
    r->w.f.instances.flags[1] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
    r->w.f.instances.layer_mask[1] = 1u << 24;
    g_mat[1].alpha_mode = DE_COOKED_ALPHA_MASK;
    g_mat[1].standard.alpha_mode = DE_COOKED_ALPHA_MASK;
    g_mat[1].standard.alpha_cutoff = .5f;
    g_mat[1].standard.base_color_tex.id = 1;
    g_mat[1].textures[0].id = 1;
    shadow_frame(f);
    de__rt_shadows_info info;
    const u16 *mask = shadow_read(f, &info);
    const f32 center = 3 - 2 * tanf(.65f);
    const u32 left = (u32)((center - .3f + 1) * SW / 8), right = (u32)((center + .3f + 1) * SW / 8);
    f32 transparent = half_to_f32(mask[(SH / 2) * SW + left]),
        opaque = half_to_f32(mask[(SH / 2) * SW + right]);
    printf("foliage shadow transparent %g opaque %g at pixels %u/%u\n", transparent, opaque, left,
           right);
    EXPECT_TRUE(transparent > .95f);
    EXPECT_TRUE(opaque < .05f);
    const u16 *hdr = de_gpu_buffer_map(r->d, f->hdr_rb);
    f32 lit = half_to_f32(hdr[((SH / 2) * SW + left) * 4]),
        dark = half_to_f32(hdr[((SH / 2) * SW + right) * 4]);
    printf("actual deferred HDR through cutout %g vs shadow %g\n", lit, dark);
    EXPECT_TRUE(lit > dark * 1.5f);
    r->w.f.instances.flags[1] &= ~DE__RW_INST_CAST_SHADOWS;
    shadow_frame(f);
    mask = shadow_read(f, &info);
    EXPECT_TRUE(half_to_f32(mask[(SH / 2) * SW + right]) > .95f);
    (void)cvar_set("r.rt.shadows.max_lights", "4");
}

TEST_F(rt_shadows, no_heap_after_warmup, shadow_fixture) {
    for (u32 i = 0; i < 24; ++i)
        shadow_frame(f);
    const de_tracking_stats before = de_tracking_allocator_stats(&f->rt.track);
    const de_tracking_stats gpu_before = de_tracking_allocator_stats(&f->rt.dev_track);
    for (u32 i = 0; i < 8; ++i)
        shadow_frame(f);
    const de_tracking_stats after = de_tracking_allocator_stats(&f->rt.track);
    const de_tracking_stats gpu_after = de_tracking_allocator_stats(&f->rt.dev_track);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(after.realloc_count, before.realloc_count);
    EXPECT_EQ(gpu_after.alloc_count, gpu_before.alloc_count);
    EXPECT_EQ(gpu_after.realloc_count, gpu_before.realloc_count);
}

TEST_F(rt_shadows, analytic_lights, shadow_fixture) {
    rt_fixture *r = &f->rt;
    (void)cvar_set("r.rt.shadows.max_lights", "1");
    for (u32 kind = DE_LIGHT_POINT; kind <= DE_LIGHT_DISK; ++kind) {
        DeLight l = {.kind = (de_light_kind)kind,
                     .color = {1, 1, 1},
                     .intensity = 100,
                     .range = 20,
                     .source_radius = .1f,
                     .area_size = {.5f, .5f},
                     .spot_inner = .6f,
                     .spot_outer = .8f,
                     .cast_shadows = true};
        world_light(&r->w, 0, &l, trs((de_vec3){0, 0, 4}, (de_vec3){0, 1, 0}, -.785398163f, 1));
        shadow_frame(f);
        de__rt_shadows_info info;
        const u16 *mask = shadow_read(f, &info);
        f32 minimum = 1;
        for (u32 i = 0; i < SW * SH; ++i)
            minimum = fminf(minimum, half_to_f32(mask[i]));
        printf("analytic light %u minimum visibility %g\n", kind, minimum);
        EXPECT_TRUE(minimum < .05f);
        r->w.f.lights.light[0].cast_shadows = false;
        shadow_frame(f);
        mask = shadow_read(f, &info);
        minimum = 1;
        for (u32 i = 0; i < SW * SH; ++i)
            minimum = fminf(minimum, half_to_f32(mask[i]));
        EXPECT_TRUE(minimum > .95f);
    }
    (void)cvar_set("r.rt.shadows.max_lights", "4");
}
