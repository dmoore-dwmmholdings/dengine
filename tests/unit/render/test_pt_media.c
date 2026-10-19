#include "scene_as_fixture.h"
#include "render/pathtracer/pt_internal.h"

typedef struct pt_fixture {
    rt_fixture rt;
    de__rw_view camera;
    de_gpu_buffer rb, sphere_indices;
    de_cooked_mesh_range sphere_range;
    de_render_submesh sphere_submesh;
    de__pt_info info;
    de_gpu_pipeline owner_pipeline;
    de_gpu_buffer owner_params, owner_output;
    u32 probe_count;
} pt_fixture;
static void pt_fixture_setup(pt_fixture *f) {
    rt_fixture *r = &f->rt;
    rt_fixture_setup(r);
    if (!de_gpu_get_caps(r->d)->rt_pipelines)
        SKIP("native RT pipelines unavailable");
    de__view_unregister_stage(r->r, trace_stage, r);
    de__view_capture_camera(r->r, r->view, NULL);
    f->camera = (de__rw_view){.entity = 999,
                              .camera = {.projection = DE_PROJ_PERSPECTIVE,
                                         .fov_y = .9f,
                                         .near_z = .1f,
                                         .view = r->view,
                                         .exposure_ev100 = 0},
                              .offset = {3, 0, 4},
                              .rotation = {0, 0, 0, 1}};
    f->camera.transform = at(3, 0, 4);
    r->w.f.views = &f->camera;
    r->w.f.view_count = 1;
    g_mat[0].standard.alpha_mode = DE_COOKED_ALPHA_OPAQUE;
    g_mat[0].alpha_mode = DE_COOKED_ALPHA_OPAQUE;
    g_mat[0].standard.base_color_tex = (de_texture){0};
    g_mat[0].textures[0] = (de_texture){0};
    g_mat[0].standard.base_color = (de_vec4){2, 1, .5f, 1};
    g_mat[0].standard.unlit = true;
    g_mat[0].standard.emissive = (de_vec3){2, 1, .5f};
    g_mat[0].standard.emissive_strength = 1;
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.path_tracer = true;
    settings.path_max_bounces = 8;
    settings.render_scale = 1;
    de_renderer_set_settings(r->r, &settings);
    f->rb = de_gpu_buffer_create(r->d, &(de_gpu_buffer_desc){.size = 64 * 64 * 16,
                                                             .usage = DE_USAGE_COPY_DST,
                                                             .memory = DE_MEM_READBACK});
    ASSERT_TRUE(f->rb.id);
    (void)cvar_set("r.gpu_profile", "0");
}
static void pt_fixture_teardown(pt_fixture *f) {
    if (f->rt.d) {
        de_gpu_wait_idle(f->rt.d);
        de_gpu_buffer_destroy(f->rt.d, f->rb);
        de_gpu_pipeline_destroy(f->rt.d, f->owner_pipeline);
        de_gpu_buffer_destroy(f->rt.d, f->owner_params);
        de_gpu_buffer_destroy(f->rt.d, f->owner_output);
        if (f->sphere_indices.id) {
            mesh_free(&g_mesh[1], f->rt.d);
            de_gpu_buffer_destroy(f->rt.d, f->sphere_indices);
        }
    }
    (void)cvar_set("r.ambient_fallback", "0");
    (void)cvar_set("r.pt.seed", "0");
    (void)cvar_set("r.pt.sampling_strategy", "0");
    rt_fixture_teardown(&f->rt);
}
static void pt_frame(pt_fixture *f) {
    rt_fixture *r = &f->rt;
    world_begin(&r->w);
    r->handoff.last = &r->w.f;
    r->handoff.current = &r->w.f;
    r->r->render_world = &r->handoff;
    de__renderer_kick_prepare(r->r);
    ASSERT_EQ(de_renderer_render_frame(r->r), DE_OK);
    de_gpu_wait_idle(r->d);
    r->handoff.current = NULL;
    ASSERT_TRUE(de__pt_get(r->r, r->view, &f->info));
    ASSERT_FALSE(de_gpu_device_is_lost(r->d));
}
static float *pt_read(pt_fixture *f) {
    rt_fixture *r = &f->rt;
    de_gpu_cmd *cmd = de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "PT accumulation readback");
    de_gpu_barrier b = {.texture = f->info.accumulation,
                        .before = DE_ACCESS_SHADER_READ,
                        .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, f->rb, 0, f->info.width * 16, f->info.accumulation, 0,
                                      0);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    ASSERT_EQ(de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(r->d);
    return de_gpu_buffer_map(r->d, f->rb);
}

TEST(pt_media, coefficients_and_phase) {
    de_vec3 s, a, s2, a2;
    de__pt_sss_coefficients((de_vec3){1, 2, 3}, (de_vec3){.2f, .5f, .8f}, &s, &a);
    de__pt_sss_coefficients((de_vec3){2, 4, 6}, (de_vec3){.2f, .5f, .8f}, &s2, &a2);
    EXPECT_TRUE(s.x > 0 && a.x > 0 && s.z > 0 && a.z > 0);
    EXPECT_FLOAT_EQ(s2.x, s.x * .5f, 1e-6);
    EXPECT_FLOAT_EQ(a2.z, a.z * .5f, 1e-6);
    const double g[] = {-.8, 0, .5};
    for (u32 k = 0; k < 3; ++k) {
        double integral = 0;
        const u32 n = 262144;
        for (u32 i = 0; i < n; ++i)
            integral += de__pt_hg((f32)(-1 + 2 * ((double)i + .5) / n), (f32)g[k]);
        integral *= 4 * DE_PI / n;
        printf("HG g=%g integral %.9g\n", g[k], integral);
        EXPECT_FLOAT_EQ(integral, 1, 1e-4);
    }
}
static double media_mean(pt_fixture *f, u32 channel) {
    float *p = pt_read(f);
    double sum = 0;
    u32 n = 0;
    for (u32 y = 24; y < 40; ++y)
        for (u32 x = 24; x < 40; ++x) {
            u32 i = (y * 64 + x) * 4;
            EXPECT_FLOAT_EQ(p[i + 3], de_renderer_path_samples(f->rt.r, f->rt.view), 0);
            sum += p[i + channel] / p[i + 3];
            ++n;
        }
    return sum / n;
}
static void media_samples(pt_fixture *f, u32 n) {
    for (u32 i = 0; i < n; ++i)
        pt_frame(f);
}
/* Closed slab: the six independently wound faces belong to ONE BLAS/instance. */
static void media_slab(pt_fixture *f, float thickness) {
    rt_fixture *r = &f->rt;
    fmesh *fm = &g_mesh[1];
    mesh_plane(fm, r->d, 6, 1, 8);
    g_mesh_count = 2;
    de_render_mesh *m = &fm->rm;
    u8 *v = de_gpu_buffer_map(r->d, fm->buf[0]);
    u8 *records = de_gpu_buffer_map(r->d, fm->buf[1]);
    u32 ix[6 * PATCH_TRIS * 3], n = 0;
    const de_vec3 normals[6] = {{0, 0, 1},  {0, 0, -1}, {1, 0, 0},
                                {-1, 0, 0}, {0, 1, 0},  {0, -1, 0}};
    for (u32 face = 0; face < 6; ++face) {
        de_vec3 normal = normals[face];
        for (u32 y = 0; y < PATCH; ++y)
            for (u32 x = 0; x < PATCH; ++x) {
                float u = -4 + 8 * (float)x / (PATCH - 1), w = -4 + 8 * (float)y / (PATCH - 1);
                de_vec3 pos;
                if (face == 0)
                    pos = (de_vec3){u, w, 0};
                else if (face == 1)
                    pos = (de_vec3){-u, w, -thickness};
                else if (face == 2)
                    pos = (de_vec3){4, u, -thickness * .5f + w * thickness / 8};
                else if (face == 3)
                    pos = (de_vec3){-4, -u, -thickness * .5f + w * thickness / 8};
                else if (face == 4)
                    pos = (de_vec3){u, 4, -thickness * .5f - w * thickness / 8};
                else
                    pos = (de_vec3){u, -4, -thickness * .5f + w * thickness / 8};
                u32 index = face * PATCH_VERT + y * PATCH + x;
                memcpy(v + index * 12, &pos, 12);
                float d = fabsf(normal.x) + fabsf(normal.y) + fabsf(normal.z), ox = normal.x / d,
                      oy = normal.y / d;
                if (normal.z < 0) {
                    float save = ox;
                    ox = (1 - fabsf(oy)) * (save < 0 ? -1 : 1);
                    oy = (1 - fabsf(save)) * (oy < 0 ? -1 : 1);
                }
                put_u32(v + m->stream_offset[DE_COOKED_STREAM_NORMAL] + index * 4,
                        (u16)(i16)lrintf(ox * 32767) | ((u32)(u16)(i16)lrintf(oy * 32767) << 16));
            }
        for (u32 y = 0; y + 1 < PATCH; ++y)
            for (u32 x = 0; x + 1 < PATCH; ++x) {
                u32 a = face * PATCH_VERT + y * PATCH + x, b = a + 1, c = b + PATCH, d = a + PATCH;
                ix[n++] = a;
                ix[n++] = b;
                ix[n++] = c;
                ix[n++] = a;
                ix[n++] = c;
                ix[n++] = d;
            }
        put_f32(records + face * DE_COOKED_MESHLET_SIZE + 16, 0);
        put_f32(records + face * DE_COOKED_MESHLET_SIZE + 20, 0);
        put_f32(records + face * DE_COOKED_MESHLET_SIZE + 24, -thickness * .5f);
        put_f32(records + face * DE_COOKED_MESHLET_SIZE + 28, 8);
    }
    f->sphere_indices = de_gpu_buffer_create(
        r->d, &(de_gpu_buffer_desc){.size = sizeof ix,
                                    .usage = DE_USAGE_STORAGE | DE_USAGE_ACCEL_INPUT,
                                    .memory = DE_MEM_UPLOAD});
    memcpy(de_gpu_buffer_map(r->d, f->sphere_indices), ix, sizeof ix);
    m->buffers[DE_RENDER_MESH_INDICES] = f->sphere_indices;
    m->bindless[DE_RENDER_MESH_INDICES] = de_gpu_buffer_bindless(r->d, f->sphere_indices);
    m->index_count = n;
    m->lods[0].index_count = n;
    f->sphere_range = (de_cooked_mesh_range){.index_count = n};
    m->lod_submeshes = &f->sphere_range;
    m->submeshes = &f->sphere_submesh;
    m->bounds_min[0] = m->bounds_min[1] = -4;
    m->bounds_max[0] = m->bounds_max[1] = 4;
    m->bounds_min[2] = -thickness;
    m->bounds_max[2] = 0;
    m->sphere[3] = 8;
    ASSERT_EQ(de__rt_mesh_publish(r->r->rt_scene, m), DE_OK);
    world_set(&r->w, 0, 1, at(3, 0, -2), 1);
    world_set(&r->w, 1, 2, at(3, 0, 0), 2);
    for (u32 i = 0; i < 2; ++i) {
        r->w.f.instances.flags[i] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
        r->w.f.instances.layer_mask[i] = 1u << 24;
    }
    material_add(1, .001f, 0, .5f);
    g_mat[1].standard.ior = 1.000001f;
    g_mat[1].standard.transmission = 1;
    g_mat[1].standard.thickness = thickness;
    g_mat[1].standard.attenuation_distance = 1;
    g_mat[1].standard.attenuation_color = (de_vec3){.8f, .6f, .4f};
    g_mat[1].standard.double_sided = true;
    g_mat[0].standard.base_color = (de_vec4){1, 1, 1, 1};
    r->w.touch_all = true;
    f->camera.camera.fov_y = .05f;
}
/* Fixed before measurement: Beer error <1%; fog <3%; SSS small-radius <1%, larger
 * radius adds >.01 backlit radiance, and white-environment outgoing energy <=1. */
TEST_F(pt_media, glass_absorption, pt_fixture) {
    const float thickness[] = {.2f, .5f, 1};
    media_slab(f, thickness[0]);
    for (u32 k = 0; k < 3; ++k) {
        if (k) {
            de__rt_mesh_evict(f->rt.r->rt_scene, &g_mesh[1].rm);
            mesh_free(&g_mesh[1], f->rt.d);
            de_gpu_buffer_destroy(f->rt.d, f->sphere_indices);
            f->sphere_indices = (de_gpu_buffer){0};
            --g_mat_count;
            media_slab(f, thickness[k]);
        }
        media_samples(f, 128);
        for (u32 c = 0; c < 3; ++c) {
            double color = c == 0   ? .8
                           : c == 1 ? .6
                                    : .4,
                   expected = pow(color, thickness[k]), actual = media_mean(f, c),
                   error = fabs(actual - expected) / expected;
            printf("Beer d=%g channel%u actual %.9g expected %.9g error %.6g%%\n", thickness[k], c,
                   actual, expected, error * 100);
            EXPECT_TRUE(error <= .01);
        }
    }
}
TEST_F(pt_media, fog_point_single_scatter, pt_fixture) {
    rt_fixture *r = &f->rt;
    g_mat[0].standard.base_color = (de_vec4){0, 0, 0, 1};
    r->w.f.fog_count = 1;
    r->w.f.fog[0] = (de__fog_snapshot){
        .entity = 777,
        .transform = at(3, 0, 2),
        .volume = {
            .extent = {20, 20, 2}, .density = .4f, .albedo = {.7f, .7f, .7f}, .anisotropy = .3f}};
    world_light(
        &r->w, 0,
        &(DeLight){.kind = DE_LIGHT_POINT, .color = {1, 1, 1}, .intensity = 20, .range = 100},
        at(4, 0, 2));
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.path_max_bounces = 1;
    de_renderer_set_settings(r->r, &settings);
    f->camera.camera.fov_y = .001f;
    /* Camera z4 to box z3..1; point at (4,0,2), inside the wide x/y bounds.
     * Independent midpoint integration includes extinction on BOTH camera/light paths. */
    double reference = 0;
    const u32 steps = 262144;
    for (u32 i = 0; i < steps; ++i) {
        double z = 3 - 2 * ((double)i + .5) / steps, dz = 2 - z, d = sqrt(1 + dz * dz),
               cosine = -dz / d;
        reference += exp(-.4 * (3 - z)) * .4 * .7 * de__pt_hg((float)cosine, .3f) *
                     (20 / (4 * DE_PI)) / (d * d) * exp(-.4 * d);
    }
    reference *= 2.0 / steps;
    media_samples(f, 32);
    double prefix = media_mean(f, 0);
    media_samples(f, 2016);
    double actual = media_mean(f, 0), error = fabs(actual - reference) / reference;
    printf("Fog single scatter actual %.9g analytic %.9g relative %.6g%%\n", actual, reference,
           error * 100);
    EXPECT_TRUE(error <= .03);
    r->w.f.lights.light[0].cast_shadows = true;
    r->w.touch_lights = true;
    de_renderer_path_reset(r->r, r->view);
    media_samples(f, 32);
    double shadowed = media_mean(f, 0);
    printf("Fog shadow-flag prefix %.9g vs %.9g\n", shadowed, prefix);
    EXPECT_FLOAT_EQ(shadowed, prefix, 1e-6);
    r->w.f.lights.light[0].cast_shadows = false;
    r->w.touch_lights = true;
    /* KHR unlit uses base colour; its ignored standard emissive field must
     * not create an area emitter or change this black receiver's transport. */
    g_mat[0].standard.emissive = (de_vec3){200, 100, 50};
    r->w.touch_all = true;
    de_renderer_path_reset(r->r, r->view);
    media_samples(f, 32);
    double ignored = media_mean(f, 0);
    printf("Fog ignored-emission prefix %.9g vs %.9g\n", ignored, prefix);
    EXPECT_FLOAT_EQ(ignored, prefix, 1e-6);
}
TEST_F(pt_media, sss_radius_and_energy, pt_fixture) {
    media_slab(f, .2f);
    g_mat[1].standard.transmission = 0;
    g_mat[1].standard.thickness = 0;
    g_mat[1].standard.base_color = (de_vec4){.5f, .5f, .5f, 1};
    g_mat[1].standard.subsurface = 0;
    ASSERT_TRUE(cvar_set("r.ambient_fallback", "1"));
    media_samples(f, 256);
    double diffuse = media_mean(f, 0);
    g_mat[1].standard.subsurface = 1;
    g_mat[1].standard.subsurface_radius = (de_vec3){1e-8f, 1e-8f, 1e-8f};
    f->rt.w.touch_all = true;
    media_samples(f, 256);
    double tiny = media_mean(f, 0);
    printf("SSS radius->0 diffuse %.9g tiny %.9g\n", diffuse, tiny);
    EXPECT_TRUE(fabs(tiny - diffuse) / fmax(diffuse, 1e-8) <= .01);
    g_mat[1].standard.subsurface_radius = (de_vec3){.5f, .5f, .5f};
    f->rt.w.touch_all = true;
    media_samples(f, 512);
    double energy = media_mean(f, 0);
    printf("SSS white furnace outgoing %.9g\n", energy);
    EXPECT_TRUE(energy <= 1);
    ASSERT_TRUE(cvar_set("r.ambient_fallback", "0"));
    de_renderer_path_reset(f->rt.r, f->rt.view);
    media_samples(f, 512);
    double backlit = media_mean(f, 0);
    g_mat[1].standard.subsurface = 0;
    f->rt.w.touch_all = true;
    media_samples(f, 256);
    double opaque = media_mean(f, 0);
    printf("SSS backlit large %.9g opaque %.9g\n", backlit, opaque);
    EXPECT_TRUE(backlit > opaque + .01);
    /* An exactly index-matched exit must also transmit, with no singular
     * interior microfacet evaluation or zero-density thin-wall rejection. */
    g_mat[1].standard.subsurface = 1;
    g_mat[1].standard.ior = 1.f;
    f->rt.w.touch_all = true;
    media_samples(f, 512);
    double matched = media_mean(f, 0);
    printf("SSS exactly matched IOR backlit %.9g\n", matched);
    EXPECT_TRUE(isfinite(matched) && matched > opaque + .01 && matched <= 1);
}

#if DE_HAS_GPU_METAL
static void owner_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    (void)pass;
    pt_fixture *f = user;
    u32 pc[] = {de_gpu_buffer_bindless(f->rt.d, f->owner_params), 0};
    de_gpu_cmd_set_pipeline(cmd, f->owner_pipeline);
    de_gpu_cmd_push_constants(cmd, pc, sizeof pc);
    de_gpu_cmd_dispatch_rays(cmd, f->probe_count ? f->probe_count : 5, 1, 1);
    de_gpu_barrier barrier = {
        .buffer = f->owner_output, .before = DE_ACCESS_SHADER_WRITE, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_buffer(cmd, f->rb, 0, f->owner_output, 0,
                           (f->probe_count ? f->probe_count : 5) * 16);
}
static void owner_stage(de__view_frame *vf, void *user) {
    pt_fixture *f = user;
    rt_fixture *r = &f->rt;
    if (vf->view.id != r->view.id)
        return;
    de__rt_context *context = de__rt_context_get(r->r, vf->view, vf->graph, vf->frame_index);
    ASSERT_TRUE(context != NULL);
    ++r->stages;
    /* PtParams' query/output words; the probe uses no shading/material parameters. */
    u32 *params = de_gpu_buffer_map(r->d, f->owner_params);
    params[0] = de_gpu_buffer_bindless(r->d, context->query);
    params[9] = de_gpu_buffer_bindless(r->d, f->owner_output);
    if (f->probe_count == 8)
        params[90] = 2; /* PtMediaParams.features: closed-volume absorption. */
    de_rg_pass *p = de_rg_add_pass(vf->graph, "SSS owning-boundary acceptance", DE_QUEUE_GRAPHICS,
                                   owner_execute, f);
    ASSERT_TRUE(p != NULL);
    de_rg_read(p, context->dependency, DE_ACCESS_SHADER_READ);
    de_rg_read(p, de_rg_import_buffer(vf->graph, context->query, DE_ACCESS_SHADER_READ),
               DE_ACCESS_SHADER_READ);
    de_rg_read(p, de_rg_import_buffer(vf->graph, f->owner_params, DE_ACCESS_SHADER_READ),
               DE_ACCESS_SHADER_READ);
    de_rg_write(p, de_rg_import_buffer(vf->graph, f->owner_output, DE_ACCESS_COPY_SRC),
                DE_ACCESS_SHADER_WRITE);
    de_rg_write(p, de_rg_import_buffer(vf->graph, f->rb, DE_ACCESS_COPY_DST), DE_ACCESS_COPY_DST);
    de_rg_pass_side_effect(p);
}
#endif
TEST_F(pt_media, sss_walk_ignores_foreign_interior_surfaces, pt_fixture) {
#if DE_HAS_GPU_METAL
    media_slab(f, .2f);
    u32 foreign = material_add(.2f, .01f, 0, .5f);
    g_mat[foreign - 1].standard.double_sided = true;
    world_set(&f->rt.w, 2, 1, at(3, 0, -.1f), foreign);
    f->rt.w.f.instances.flags[2] |= DE__RW_INST_VISIBLE_IN_RT;
    f->rt.w.f.instances.layer_mask[2] = 1u << 24;
    /* Coincident with the owning exit: rejecting the wrong instance must
     * happen during traversal, without stepping past both surfaces. */
    world_set(&f->rt.w, 3, 1, at(3, 0, -.2f), foreign);
    f->rt.w.f.instances.flags[3] |= DE__RW_INST_VISIBLE_IN_RT;
    f->rt.w.f.instances.layer_mask[3] = 1u << 24;
    de_render_settings settings = de_renderer_get_settings(f->rt.r);
    settings.path_tracer = false;
    de_renderer_set_settings(f->rt.r, &settings);
    extern const unsigned char de__pt_metal_library[];
    extern const unsigned long long de__pt_metal_library_bytes;
    de_gpu_shader_blob ray = {de__pt_metal_library, de__pt_metal_library_bytes,
                              "pt_sss_owner_probe"};
    de_gpu_shader_blob miss = ray;
    miss.entry = "pt_miss";
    de_gpu_rt_hit_group hit = {.closest_hit = ray, .any_hit = ray};
    hit.closest_hit.entry = "pt_closest";
    hit.any_hit.entry = "pt_alpha";
    de_gpu_rt_desc rt = {.raygen = ray,
                         .miss = &miss,
                         .miss_count = 1,
                         .hit_groups = &hit,
                         .hit_group_count = 1,
                         .max_recursion = 1,
                         .payload_bytes = 64,
                         .attribute_bytes = 8};
    f->owner_pipeline = de_gpu_pipeline_create(
        f->rt.d,
        &(de_gpu_pipeline_desc){.kind = DE_PIPE_RAYTRACING, .rt = &rt, .push_constant_bytes = 8});
    ASSERT_TRUE(f->owner_pipeline.id);
    f->owner_params = de_gpu_buffer_create(
        f->rt.d,
        &(de_gpu_buffer_desc){.size = 416, .usage = DE_USAGE_STORAGE, .memory = DE_MEM_UPLOAD});
    f->owner_output = de_gpu_buffer_create(
        f->rt.d, &(de_gpu_buffer_desc){.size = 80, .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    ASSERT_TRUE(f->owner_params.id && f->owner_output.id);
    memset(de_gpu_buffer_map(f->rt.d, f->owner_params), 0, 416);
    ASSERT_EQ(de__view_register_stage(f->rt.r, DE__VIEW_SLOT_LIGHTING, 1, "SSS owner acceptance",
                                      owner_stage, f),
              DE_OK);
    typedef struct owner_hit {
        f32 t;
        u32 instance, primitive, lod;
    } owner_hit;
    (void)frame_rt(&f->rt);
    const owner_hit *result = de_gpu_buffer_map(f->rt.d, f->rb);
    /* Origin z=-.05: foreign plane z=-.1, owning exit z=-.2. Owner0's
     * rear plane z=-2 is outside tmax1, so that owner must report a miss. */
    printf("SSS boundaries: ordinary%u t%g owner1=%u t%g owner0=%u t%g owner2=%u t%g\n",
           result[0].instance, result[0].t, result[1].instance, result[1].t, result[2].instance,
           result[2].t, result[3].instance, result[3].t);
    EXPECT_EQ(result[0].instance, 2u);
    EXPECT_FLOAT_EQ(result[0].t, .05, 1e-6);
    EXPECT_EQ(result[1].instance, 1u);
    EXPECT_FLOAT_EQ(result[1].t, .15, 1e-6);
    EXPECT_TRUE(result[2].t < 0);
    EXPECT_EQ(result[3].instance, 2u);
    EXPECT_FLOAT_EQ(result[3].t, .05, 1e-6);
    EXPECT_EQ(result[4].instance, 3u);
    EXPECT_FLOAT_EQ(result[4].t, .15, 1e-6);
    de__view_unregister_stage(f->rt.r, owner_stage, f);
#endif
}

TEST_F(pt_media, shadow_absorption_tracks_closed_volume_boundaries, pt_fixture) {
#if DE_HAS_GPU_METAL
    media_slab(f, .2f);
    de_render_settings settings = de_renderer_get_settings(f->rt.r);
    settings.path_tracer = false;
    de_renderer_set_settings(f->rt.r, &settings);
    f->probe_count = 8;
    extern const unsigned char de__pt_metal_library[];
    extern const unsigned long long de__pt_metal_library_bytes;
    de_gpu_shader_blob ray = {de__pt_metal_library, de__pt_metal_library_bytes,
                              "pt_shadow_boundary_probe"};
    de_gpu_shader_blob miss = ray;
    miss.entry = "pt_miss";
    de_gpu_rt_hit_group hit = {.closest_hit = ray, .any_hit = ray};
    hit.closest_hit.entry = "pt_closest";
    hit.any_hit.entry = "pt_alpha";
    de_gpu_rt_desc rt = {.raygen = ray,
                         .miss = &miss,
                         .miss_count = 1,
                         .hit_groups = &hit,
                         .hit_group_count = 1,
                         .max_recursion = 1,
                         .payload_bytes = 64,
                         .attribute_bytes = 8};
    f->owner_pipeline = de_gpu_pipeline_create(
        f->rt.d,
        &(de_gpu_pipeline_desc){.kind = DE_PIPE_RAYTRACING, .rt = &rt, .push_constant_bytes = 8});
    ASSERT_TRUE(f->owner_pipeline.id);
    f->owner_params = de_gpu_buffer_create(
        f->rt.d,
        &(de_gpu_buffer_desc){.size = 416, .usage = DE_USAGE_STORAGE, .memory = DE_MEM_UPLOAD});
    f->owner_output = de_gpu_buffer_create(
        f->rt.d, &(de_gpu_buffer_desc){.size = 128, .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    ASSERT_TRUE(f->owner_params.id && f->owner_output.id);
    memset(de_gpu_buffer_map(f->rt.d, f->owner_params), 0, 416);
    ASSERT_EQ(de__view_register_stage(f->rt.r, DE__VIEW_SLOT_LIGHTING, 1,
                                      "Shadow boundary acceptance", owner_stage, f),
              DE_OK);

    (void)frame_rt(&f->rt);
    const f32 *values = de_gpu_buffer_map(f->rt.d, f->rb);
    const f32 color[] = {.8f, .6f, .4f};
    const f32 distance[] = {.2f, .15f, .15f, .1f, .05f, 0, 0, .2f};
    for (u32 ray = 0; ray < 8; ++ray)
        for (u32 c = 0; c < 3; ++c) {
            f32 expected = ray == 6 ? 0 : powf(color[c], distance[ray]);
            printf("shadow ray%u channel%u actual%g expected%g\n", ray, c, values[ray * 4 + c],
                   expected);
            EXPECT_FLOAT_EQ(values[ray * 4 + c], expected, 2e-5);
        }
    /* Reuse one closed BLAS for a nested volume, then a partially overlapping
     * one. Exiting a non-top owner must leave the inner medium active. */
    u32 inner = material_add(1, .001f, 0, .5f);
    g_mat[inner - 1].standard = g_mat[1].standard;
    g_mat[inner - 1].standard.attenuation_color = (de_vec3){.5f, .7f, .9f};
    const f32 inner_color[] = {.5f, .7f, .9f};
    for (u32 overlap = 0; overlap < 2; ++overlap) {
        de_affine x = at(3, 0, overlap ? -.1f : -.05f);
        x.m[8] = overlap ? 1 : .5f;
        world_set(&f->rt.w, 2, 2, x, inner);
        f->rt.w.f.instances.flags[2] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
        f->rt.w.f.instances.layer_mask[2] = 1u << 24;
        (void)frame_rt(&f->rt);
        for (u32 c = 0; c < 3; ++c) {
            f32 expected = powf(color[c], .1f) * powf(inner_color[c], overlap ? .2f : .1f);
            printf("shadow overlap%u channel%u actual%g expected%g\n", overlap, c, values[c],
                   expected);
            EXPECT_FLOAT_EQ(values[c], expected, 2e-5);
            EXPECT_FLOAT_EQ(values[7 * 4 + c], expected, 2e-5);
            /* No initial stack: discover the enclosing owner after crossing
             * its inner volume, without charging inner lengths twice. */
            f32 inside = powf(color[c], .05f) * powf(inner_color[c], overlap ? .2f : .1f);
            EXPECT_FLOAT_EQ(values[2 * 4 + c], inside, 2e-5);
        }
    }
    /* Skipping non-casting glass still finds an opaque caster behind it. */
    world_set(&f->rt.w, 0, 1, at(3, 0, -.5f), 1);
    f->rt.w.f.instances.flags[0] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
    f->rt.w.f.instances.layer_mask[0] = 1u << 24;
    for (u32 i = 1; i <= 2; ++i)
        f->rt.w.f.instances.flags[i] &= ~DE__RW_INST_CAST_SHADOWS;
    (void)frame_rt(&f->rt);
    for (u32 c = 0; c < 3; ++c)
        EXPECT_FLOAT_EQ(values[6 * 4 + c], 0, 0);
    f->rt.w.f.instances.flags[0] &= ~DE__RW_INST_CAST_SHADOWS;
    (void)frame_rt(&f->rt);
    for (u32 c = 0; c < 3; ++c) {
        f32 expected = powf(color[c], .1f) * powf(inner_color[c], .2f);
        EXPECT_FLOAT_EQ(values[6 * 4 + c], expected, 2e-5);
    }
    de__view_unregister_stage(f->rt.r, owner_stage, f);
#endif
}

TEST_F(pt_media, gpu_stack_overflow, pt_fixture) {
#if DE_HAS_GPU_METAL
    extern const unsigned char de__pt_metal_library[];
    extern const unsigned long long de__pt_metal_library_bytes;
    de_gpu_device *d = f->rt.d;
    de_gpu_pipeline pipeline = de_gpu_pipeline_create(
        d, &(de_gpu_pipeline_desc){
               .kind = DE_PIPE_COMPUTE,
               .cs = {de__pt_metal_library, de__pt_metal_library_bytes, "pt_media_stack_probe"},
               .push_constant_bytes = 4,
               .threadgroup_size = {1, 1, 1}});
    ASSERT_TRUE(pipeline.id);
    de_gpu_buffer output = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = 8, .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    de_gpu_cmd *cmd =
        de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "Actual production medium stack safety");
    de_gpu_barrier barrier = {.buffer = output, .after = DE_ACCESS_SHADER_WRITE};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_set_pipeline(cmd, pipeline);
    u32 index = de_gpu_buffer_bindless(d, output);
    de_gpu_cmd_push_constants(cmd, &index, 4);
    de_gpu_cmd_dispatch(cmd, 1, 1, 1);
    barrier.before = DE_ACCESS_SHADER_WRITE;
    barrier.after = DE_ACCESS_COPY_SRC;
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_buffer(cmd, f->rb, 0, output, 0, 8);
    ASSERT_EQ(
        de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
        DE_OK);
    de_gpu_wait_idle(d);
    u32 *result = de_gpu_buffer_map(d, f->rb);
    EXPECT_EQ(result[0], 4u);
    EXPECT_EQ(result[1], 1u);
    de_gpu_pipeline_destroy(d, pipeline);
    de_gpu_buffer_destroy(d, output);
#endif
}

TEST_F(pt_media, subsurface_entry_samples_do_not_repeat_prior_events, pt_fixture) {
#if DE_HAS_GPU_METAL
    extern const unsigned char de__pt_metal_library[];
    extern const unsigned long long de__pt_metal_library_bytes;
    de_gpu_device *d = f->rt.d;
    de_gpu_pipeline pipeline = de_gpu_pipeline_create(
        d, &(de_gpu_pipeline_desc){
               .kind = DE_PIPE_COMPUTE,
               .cs = {de__pt_metal_library, de__pt_metal_library_bytes, "pt_sss_sampling_probe"},
               .push_constant_bytes = 4,
               .threadgroup_size = {64, 1, 1}});
    ASSERT_TRUE(pipeline.id);
    de_gpu_buffer output = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = 256 * 16, .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    ASSERT_TRUE(output.id);
    de_gpu_cmd *cmd = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "SSS random coordinate independence");
    de_gpu_barrier barrier = {.buffer = output, .after = DE_ACCESS_SHADER_WRITE};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_set_pipeline(cmd, pipeline);
    u32 index = de_gpu_buffer_bindless(d, output);
    de_gpu_cmd_push_constants(cmd, &index, sizeof index);
    de_gpu_cmd_dispatch(cmd, 4, 1, 1);
    barrier.before = DE_ACCESS_SHADER_WRITE;
    barrier.after = DE_ACCESS_COPY_SRC;
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_buffer(cmd, f->rb, 0, output, 0, 256 * 16);
    ASSERT_EQ(
        de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
        DE_OK);
    de_gpu_wait_idle(d);
    const u32 *result = de_gpu_buffer_map(d, f->rb);
    u32 collisions = 0, identical = 0;
    for (u32 event = 0; event < 256; ++event) {
        identical += result[event * 4 + 3];
        for (u32 coordinate = 0; coordinate < 3; ++coordinate) {
            u32 i = event * 4 + coordinate;
            for (u32 earlier = 0; earlier < i; ++earlier)
                if (earlier % 4 != 3 && result[i] == result[earlier])
                    ++collisions;
        }
    }
    printf("SSS entry sampler: %u reused dimensions, %u identical earlier/later draws\n",
           collisions, identical);
    EXPECT_EQ(collisions, 0u);
    /* Independent 24-bit samples may coincide, but systematic reuse must not. */
    EXPECT_TRUE(identical < 4);
    de_gpu_pipeline_destroy(d, pipeline);
    de_gpu_buffer_destroy(d, output);
#endif
}
