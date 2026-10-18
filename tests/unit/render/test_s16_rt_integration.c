/* Original S16 exit: simultaneous production effects, not isolated effect fixtures. */
#define SW 128u
#define SH 128u
#include "rt_shadow_fixture.h"
#include "render/rt/rtao.h"
#include "render/rt/rt_reflections.h"
#include "render/rt/rt_gi_probes.h"
#include "render/rt/rt_refraction.h"
#include "render/rt/restir_di.h"

typedef struct integration_fixture {
    shadow_fixture shadow;
    de_gpu_buffer rb;
    const de__lighting_view *lighting;
    bool gi_consumed, gtao, ssr, sun_mask, di_consumed;
    de__rt_shadows_info shadows;
    de__rtao_info ao;
    de__rt_reflections_info reflection;
    de__rt_gi_info gi;
    de__rt_refraction_info refraction;
    de__di_info di;
    f32 all_on[SW * SH * 3];
    f64 deltas[5], drift[5];
    u32 rt_passes, gi_record_index;
    de__gi_gpu gi_record;
} integration_fixture;

typedef struct identity_pass {
    integration_fixture *f;
    de_rg_resource records;
} identity_pass;
static void identity_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    (void)cmd;
    identity_pass *p = user;
    de_gpu_device *d = p->f->shadow.rt.d;
    de_gpu_buffer records = de_rg_buffer(pass, p->records);
    p->f->gi_record_index = de_gpu_buffer_bindless(d, records);
    const de__gi_gpu *mapped = de_gpu_buffer_map(d, records);
    ASSERT_TRUE(mapped);
    p->f->gi_record = mapped[1];
}
static void integration_stage(de__view_frame *vf, void *user) {
    integration_fixture *f = user;
    if (vf->view.id != f->shadow.rt.view.id)
        return;
    hdr_stage(vf, &f->shadow);
    de__lighting *lt = de__views_lighting(f->shadow.rt.r);
    const de__lighting_context *lc =
        de__lighting_frame_context(lt, vf->view, vf->graph, vf->frame_index);
    ASSERT_TRUE(lc);
    const de__lighting_view_state *state = &lt->views[(vf->view.id & 255u) - 1u];
    f->lighting = (const de__lighting_view *)(state->constants_map +
                                              (size_t)state->entry * sizeof(de__lighting_view));
    f->gi_consumed = lc->gi_records.id && lc->gi_texture_count >= 2;
    f->sun_mask = lc->rt_shadow_visibility.id && lc->rt_shadow_keys.id;
    f->di_consumed = lc->restir_diffuse.id && lc->restir_specular.id;
    f->gtao = f->ssr = false;
    f->rt_passes = 0;
    f->gi_record_index = 0;
    if (f->gi_consumed) {
        identity_pass *p = de_arena_push_zero(vf->graph->arena, sizeof *p, 16);
        ASSERT_TRUE(p);
        *p = (identity_pass){f, lc->gi_records};
        de_rg_pass *pass = de_rg_add_pass(vf->graph, "S16 GI consumed identity", DE_QUEUE_GRAPHICS,
                                          identity_execute, p);
        de_rg_read(pass, p->records, DE_ACCESS_SHADER_READ);
        de_rg_pass_side_effect(pass);
    }
    for (u32 i = 0; i < vf->graph->pass_count; ++i) {
        const char *name = vf->graph->passes[i]->name;
        f->rt_passes += strncmp(name, "RT.", 3) == 0;
        f->gtao |= strcmp(name, "gtao.raw") == 0;
        f->ssr |= strstr(name, "ssr.") != NULL;
    }
}
static void plane(integration_fixture *f, u32 slot, de_affine transform, u32 material) {
    rt_fixture *r = &f->shadow.rt;
    world_set(&r->w, slot, 2, transform, material);
    r->w.f.instances.flags[slot] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
    r->w.f.instances.layer_mask[slot] = 1u << 24;
}
static void integration_fixture_setup(integration_fixture *f) {
    shadow_fixture_setup(&f->shadow);
    rt_fixture *r = &f->shadow.rt;
    de__view_unregister_stage(r->r, hdr_stage, &f->shadow);
    ASSERT_EQ(de__view_register_stage(r->r, DE__VIEW_SLOT_TRANSPARENT, 1,
                                      "S16 final HDR consumption", integration_stage, f),
              DE_OK);
    f->shadow.camera.camera.projection = DE_PROJ_PERSPECTIVE;
    f->shadow.camera.camera.fov_y = 2 * atanf(2.f / 8.f);
    g_mat_count = 0;
    material_add(.65f, .8f, 0, .5f); /* Lit room. */
    material_add(1, .01f, 1, 1);     /* Left mirror. */
    material_add(1, .05f, 0, .5f);   /* Right transmissive pane. */
    material_add(1, .8f, 0, 0);      /* Real green target behind glass. */
    material_add(1, .8f, 0, 0);      /* Real gold reflection target above mirror. */
    g_mat[2].standard.transmission = 1;
    g_mat[2].standard.ior = 1.5f;
    g_mat[2].standard.thickness = .3f;
    g_mat[2].standard.attenuation_color = (de_vec3){.6f, .85f, .7f};
    g_mat[2].standard.attenuation_distance = 1;
    g_mat[3].standard.unlit = true;
    g_mat[3].standard.base_color = (de_vec4){.05f, .8f, .12f, 1};
    g_mat[4].standard.unlit = true;
    g_mat[4].standard.base_color = (de_vec4){.9f, .35f, .04f, 1};
    r->w.f.instances.count = 0;
    plane(f, 0, trs((de_vec3){3, 0, 0}, (de_vec3){0, 0, 1}, 0, 2), 1);
    plane(f, 1, trs((de_vec3){2.05f, .25f, .15f}, (de_vec3){0, 0, 1}, 0, .65f), 2);
    plane(f, 2, trs((de_vec3){3.85f, .25f, 1}, (de_vec3){0, 0, 1}, 0, .65f), 3);
    plane(f, 3, trs((de_vec3){3.85f, .25f, .1f}, (de_vec3){0, 0, 1}, 0, .65f), 4);
    plane(f, 4, trs((de_vec3){3, .55f, .8f}, (de_vec3){0, 0, 1}, 0, .25f), 1);
    plane(f, 5, trs((de_vec3){3, -1.5f, 1.5f}, (de_vec3){1, 0, 0}, -DE_PI / 2, 2), 1);
    plane(f, 6, trs((de_vec3){1, 0, 1.5f}, (de_vec3){0, 1, 0}, DE_PI / 2, 2), 1);
    plane(f, 7, trs((de_vec3){5, 0, 1.5f}, (de_vec3){0, 1, 0}, -DE_PI / 2, 2), 1);
    plane(f, 8, trs((de_vec3){3, 1.8f, 1.5f}, (de_vec3){1, 0, 0}, DE_PI / 2, 2), 1);
    /* Backface-culled from this camera, front-facing to the actual reflected ray. */
    plane(f, 9, trs((de_vec3){3, 0, 9}, (de_vec3){0, 1, 0}, DE_PI, 4), 5);
    r->w.f.instances.flags[9] &= ~DE__RW_INST_CAST_SHADOWS;
    world_light(&r->w, 0,
                &(DeLight){.kind = DE_LIGHT_DIRECTIONAL,
                           .color = {1, 1, 1},
                           .intensity = 3,
                           .source_radius = .015f,
                           .cast_shadows = true},
                trs((de_vec3){0}, (de_vec3){0, 1, 0}, .45f, 1));
    world_light(&r->w, 1,
                &(DeLight){.kind = DE_LIGHT_POINT,
                           .color = {1, .8f, .6f},
                           .intensity = 120,
                           .range = 12,
                           .cast_shadows = true},
                at(3, 1, 3));
    r->w.f.irradiance_count = 1;
    r->w.f.irradiance[0] = (de__irradiance_snapshot){
        .entity = 77,
        .position = {3, 0, 1.5f},
        .rotation = {0, 0, 0, 1},
        .volume = {
            .extent = {2, 1.5f, 2}, .counts = {3, 3, 3}, .intensity = 1, .blend_distance = .1f}};
    de_render_settings s = de_renderer_get_settings(r->r);
    s.tier = DE_TIER_RT;
    s.shadows = s.ao = s.reflections = s.gi = DE_QUALITY_HIGH;
    s.rt_shadows = s.rt_ao = s.rt_reflections = s.rt_gi = s.rt_refraction = true;
    s.gi_mode = DE_GI_RT;
    de_renderer_set_settings(r->r, &s);
    ASSERT_TRUE(cvar_set("r.rt.restir_di", "1"));
    ASSERT_TRUE(cvar_set("r.emissive_light_max", "0"));
    f->rb = de_gpu_buffer_create(r->d, &(de_gpu_buffer_desc){.size = 2u << 20,
                                                             .usage = DE_USAGE_COPY_DST,
                                                             .memory = DE_MEM_READBACK});
    ASSERT_TRUE(f->rb.id);
}
static void integration_fixture_teardown(integration_fixture *f) {
    if (f->shadow.rt.d) {
        de_gpu_wait_idle(f->shadow.rt.d);
        de_gpu_buffer_destroy(f->shadow.rt.d, f->rb);
    }
    (void)cvar_set("r.emissive_light_max", "64");
    (void)cvar_set("r.rg_gpu_timers", "1");
    shadow_fixture_teardown(&f->shadow);
}
static void owners(integration_fixture *f, bool active) {
    rt_fixture *r = &f->shadow.rt;
    /* Shadows and DI getters retain last diagnostics; current graph/input ownership is
     * authoritative. */
    if (!active) {
        EXPECT_EQ(f->rt_passes, 0u);
        EXPECT_FALSE(f->sun_mask || f->di_consumed);
        EXPECT_EQ(f->lighting->inputs.rt_shadow_visibility, 0u);
        EXPECT_EQ(f->lighting->inputs.restir_diffuse, 0u);
        return;
    }
    ASSERT_TRUE(de__rt_shadows_get(r->r->rt_shadows, r->view, &f->shadows));
    EXPECT_EQ(de__rtao_get(r->r->rtao, r->view, &f->ao), active);
    EXPECT_EQ(de__rt_reflections_get(r->r->rt_reflections, r->view, &f->reflection), active);
    EXPECT_EQ(de__rt_gi_get(r->r->rt_gi, r->view, 0, &f->gi), active);
    EXPECT_EQ(de__rt_refraction_get(r->r->rt_refraction, r->view, &f->refraction), active);
    EXPECT_EQ(de__restir_di_get(r->r->restir_di, r->view, &f->di), active);
    if (!active)
        return;
    ASSERT_TRUE(f->lighting && f->sun_mask && f->di_consumed && f->gi_consumed);
    const de__lighting_inputs_gpu *in = &f->lighting->inputs;
    EXPECT_EQ(in->ao, de_gpu_texture_bindless(r->d, f->ao.ao, 0, false));
    EXPECT_EQ(in->indirect_specular,
              de_gpu_texture_bindless(r->d, f->reflection.radiance, 0, false));
    EXPECT_EQ(in->rt_shadow_visibility,
              de_gpu_texture_bindless(r->d, f->shadows.visibility, 0, false));
    EXPECT_EQ(in->restir_diffuse, de_gpu_texture_bindless(r->d, f->di.diffuse, 0, false));
    EXPECT_EQ(in->restir_specular, de_gpu_texture_bindless(r->d, f->di.specular, 0, false));
    EXPECT_EQ(in->rt_shadow_keys, de_gpu_buffer_bindless(r->d, f->shadows.keys));
    EXPECT_EQ(in->reserved1, f->gi_record_index);
    EXPECT_EQ(f->gi_record.textures[0], de_gpu_texture_bindless(r->d, f->gi.irradiance, 0, true));
    EXPECT_EQ(f->gi_record.textures[1], de_gpu_texture_bindless(r->d, f->gi.distance, 0, true));
    EXPECT_EQ(f->gi_record.textures[2], de_gpu_buffer_bindless(r->d, f->gi.classification));
    EXPECT_TRUE(f->di.source_count >= 1 && f->gi.probes_updated > 0 && f->refraction.draws > 0);
}
static void texture_copy(de_gpu_cmd *cmd, integration_fixture *f, de_gpu_texture texture,
                         u64 offset, u32 pitch, de_gpu_access access) {
    de_gpu_barrier b = {.texture = texture, .before = access, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, f->rb, offset, pitch, texture, 0, 0);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = access;
    de_gpu_cmd_barriers(cmd, &b, 1);
}
static const u8 *read_effects(integration_fixture *f) {
    rt_fixture *r = &f->shadow.rt;
    de_gpu_cmd *cmd = de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "S16 combined output readback");
    texture_copy(cmd, f, f->ao.ao, 0, 256, DE_ACCESS_SHADER_READ);
    texture_copy(cmd, f, f->ao.raw, 851968, SW * 8, DE_ACCESS_SHADER_READ);
    texture_copy(cmd, f, f->shadows.visibility, 32768, 256, DE_ACCESS_SHADER_READ);
    texture_copy(cmd, f, f->reflection.radiance, 65536, SW * 8, DE_ACCESS_SHADER_READ);
    texture_copy(cmd, f, f->di.diffuse, 196608, SW * 16, DE_ACCESS_SHADER_READ);
    texture_copy(cmd, f, f->gi.irradiance, 458752, 1280, DE__GI_ACCESS);
    de_gpu_barrier b = {.buffer = f->refraction.radiance,
                        .before = DE_ACCESS_SHADER_READ,
                        .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_buffer(cmd, f->rb, 524288, f->refraction.radiance, 0, SW * SH * 16);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_buffer trace[] = {f->reflection.rays, f->reflection.hits};
    for (u32 i = 0; i < 2; ++i) {
        b = (de_gpu_barrier){
            .buffer = trace[i], .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
        de_gpu_cmd_barriers(cmd, &b, 1);
        de_gpu_cmd_copy_buffer(cmd, f->rb, 1048576 + i * 32, trace[i], (56 * SW + 32) * 32, 32);
        b.before = DE_ACCESS_COPY_SRC;
        b.after = DE_ACCESS_SHADER_READ;
        de_gpu_cmd_barriers(cmd, &b, 1);
    }
    ASSERT_EQ(de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(r->d);
    return de_gpu_buffer_map(r->d, f->rb);
}
static f64 hdr(integration_fixture *f, f32 *out, const char *path) {
    const u16 *data = de_gpu_buffer_map(f->shadow.rt.d, f->shadow.hdr_rb);
    FILE *image = path ? fopen(path, "wb") : NULL;
    if (path)
        ASSERT_TRUE(image);
    if (image)
        fprintf(image, "P6\n%u %u\n255\n", SW, SH);
    f64 sum = 0;
    for (u32 i = 0; i < SW * SH; ++i)
        for (u32 c = 0; c < 3; ++c) {
            f32 v = half_to_f32(data[i * 4 + c]) / f->shadow.pre_exposure;
            ASSERT_TRUE(isfinite(v) && v >= 0);
            sum += v;
            if (out)
                out[i * 3 + c] = v;
            if (image) {
                u8 mapped = (u8)lrintf(powf(v / (1 + v), 1.f / 2.2f) * 255);
                fwrite(&mapped, 1, 1, image);
            }
        }
    if (image)
        fclose(image);
    return sum / (SW * SH * 3);
}
static f64 component(u32 bits, u32 mantissa) {
    u32 e = bits >> mantissa, m = bits & ((1u << mantissa) - 1);
    return e ? ldexp(1 + (f64)m / (1u << mantissa), (i32)e - 15)
             : ldexp((f64)m, 1 - 15 - (i32)mantissa);
}
static f64 region_difference(integration_fixture *f, u32 effect) {
    u32 x0 = 48, x1 = 80, y0 = 24, y1 = 112;
    if (effect == 0) {
        x0 = 20;
        x1 = 44;
        y0 = 44;
        y1 = 76;
    }
    if (effect == 1) {
        x0 = 80;
        x1 = 104;
        y0 = 44;
        y1 = 76;
    }
    const u16 *data = de_gpu_buffer_map(f->shadow.rt.d, f->shadow.hdr_rb);
    f64 sum = 0;
    for (u32 y = y0; y < y1; ++y)
        for (u32 x = x0; x < x1; ++x)
            for (u32 c = 0; c < 3; ++c) {
                u32 i = y * SW + x;
                sum += fabs(half_to_f32(data[i * 4 + c]) / f->shadow.pre_exposure -
                            f->all_on[i * 3 + c]);
            }
    return sum / ((x1 - x0) * (y1 - y0) * 3);
}
TEST_F(s16_rt, combined_scene_fallback_and_zero_heap, integration_fixture) {
    rt_fixture *r = &f->shadow.rt;
    shadow_frame(&f->shadow); /* Registers optional graph timing cvars. */
    ASSERT_TRUE(cvar_set("r.rg_gpu_timers", "0"));
    for (u32 frame = 0; frame < 64; ++frame)
        shadow_frame(&f->shadow);
    owners(f, true);
    de_tracking_stats cpu = de_tracking_allocator_stats(&r->track),
                      gpu = de_tracking_allocator_stats(&r->dev_track);
    for (u32 frame = 0; frame < 8; ++frame)
        shadow_frame(&f->shadow);
    de_tracking_stats cpu_end = de_tracking_allocator_stats(&r->track),
                      gpu_end = de_tracking_allocator_stats(&r->dev_track);
    EXPECT_EQ(cpu_end.alloc_count, cpu.alloc_count);
    EXPECT_EQ(cpu_end.realloc_count, cpu.realloc_count);
    EXPECT_EQ(gpu_end.alloc_count, gpu.alloc_count);
    EXPECT_EQ(gpu_end.realloc_count, gpu.realloc_count);
    owners(f, true);
    f64 rt_mean = hdr(f, f->all_on, "/tmp/s16-combined-rt.ppm");
    EXPECT_TRUE(rt_mean > 1e-4);
    const u8 *raw = read_effects(f);
    f64 ao_min = 1, shadow_min = 1, shadow_max = 0, mirror = 0, di = 0, gi = 0, green = 0;
    u32 valid = 0;
    for (u32 y = 0; y < SH; ++y)
        for (u32 x = 0; x < SW; ++x) {
            f64 a = raw[y * 256 + x] / 255.;
            if (a < ao_min)
                ao_min = a;
            const u16 *s = (const u16 *)(raw + 32768 + y * 256);
            f64 v = half_to_f32(s[x]);
            ASSERT_TRUE(isfinite(v));
            if (v < shadow_min)
                shadow_min = v;
            if (v > shadow_max)
                shadow_max = v;
            const f32 *d = (const f32 *)(raw + 196608 + (y * SW + x) * 16);
            for (u32 c = 0; c < 3; ++c) {
                ASSERT_TRUE(isfinite(d[c]));
                di += d[c];
            }
            if (x >= 20 && x < 44 && y >= 44 && y < 76) {
                const u16 *p = (const u16 *)(raw + 65536 + (y * SW + x) * 8);
                for (u32 c = 0; c < 3; ++c) {
                    f32 q = half_to_f32(p[c]);
                    ASSERT_TRUE(isfinite(q));
                    mirror += q;
                }
            }
            if (x >= 80 && x < 104 && y >= 44 && y < 76) {
                const f32 *p = (const f32 *)(raw + 524288 + (y * SW + x) * 16);
                ASSERT_TRUE(isfinite(p[1]));
                green += p[1];
                valid += p[3] > 0;
            }
        }
    for (u32 y = 0; y < f->gi.layout.ih; ++y)
        for (u32 x = 0; x < f->gi.layout.iw; ++x) {
            u32 v;
            memcpy(&v, raw + 458752 + y * 1280 + x * 4, 4);
            gi += component(v & 2047, 6) + component((v >> 11) & 2047, 6) + component(v >> 22, 5);
        }
    const de__rt_ray *ray = (const de__rt_ray *)(raw + 1048576);
    const de__rt_hit *hit = (const de__rt_hit *)(raw + 1048608);
    /* Independent primary-plane intersection from fixed camera/pixel, not a GPU-picked probe. */
    de_vec3 expected = {3 + (2 * 32.5f / SW - 1) * (8 - .15f) * .25f,
                        (1 - 2 * 56.5f / SH) * (8 - .15f) * .25f, .15f};
    EXPECT_TRUE(fabs(ray->origin[0] - expected.x) < .002 &&
                fabs(ray->origin[1] - expected.y) < .002 && fabs(ray->origin[2] - .1501f) < .002);
    de_vec3 wi = de_v3_normalize((de_vec3){expected.x - 3, expected.y, 8 - .15f});
    EXPECT_TRUE(de_v3_dot(wi, (de_vec3){ray->direction[0], ray->direction[1], ray->direction[2]}) >
                .999);
    EXPECT_TRUE(hit->t > 0);
    EXPECT_EQ(hit->instance, 9u);
    printf("S16 actual mirror ray origin(%g,%g,%g) hit%u t%g\n", ray->origin[0], ray->origin[1],
           ray->origin[2], hit->instance, hit->t);
    const u16 *ar = (const u16 *)(raw + 851968 + (56 * SW + 32) * 8);
    printf("S16 mirror raw AO(%g,%g)\n", half_to_f32(ar[0]), half_to_f32(ar[1]));
    const u16 *rp = (const u16 *)(raw + 65536 + (56 * SW + 32) * 8);
    printf("S16 mirror probe hdr(%g,%g,%g) ao%g reflection(%g,%g,%g,%g) flags%u\n",
           f->all_on[(56 * SW + 32) * 3], f->all_on[(56 * SW + 32) * 3 + 1],
           f->all_on[(56 * SW + 32) * 3 + 2], raw[56 * 256 + 32] / 255., half_to_f32(rp[0]),
           half_to_f32(rp[1]), half_to_f32(rp[2]), half_to_f32(rp[3]),
           f->lighting->inputs.reserved0);
    mirror /= 24 * 32 * 3;
    di /= SW * SH * 3;
    gi /= f->gi.layout.iw * f->gi.layout.ih * 3;
    green /= 24 * 32;
    printf("S16 all-on HDR%g AOmin%g sun[%g,%g] mirror%g DI%g GI%g glass%g valid%u\n", rt_mean,
           ao_min, shadow_min, shadow_max, mirror, di, gi, green, valid);
    EXPECT_TRUE(ao_min < .95 && shadow_min < .95 && shadow_max > .95);
    EXPECT_TRUE(mirror > 1e-4 && di > 1e-4 && gi > 1e-4 && green > 1e-4 && valid > 16);
    de_render_settings all = de_renderer_get_settings(r->r);
    for (u32 effect = 0; effect < 5; ++effect) {
        hdr(f, f->all_on, NULL);
        for (u32 frame = 0; frame < 8; ++frame)
            shadow_frame(&f->shadow);
        f->drift[effect] = region_difference(f, effect);
        hdr(f, f->all_on, NULL);
        de_render_settings s = all;
        if (effect == 0)
            s.rt_reflections = false;
        if (effect == 1)
            s.rt_refraction = false;
        if (effect == 2)
            s.ao = DE_QUALITY_OFF;
        if (effect == 3)
            s.gi = DE_QUALITY_OFF;
        if (effect == 4)
            s.rt_shadows = false;
        de_renderer_set_settings(r->r, &s);
        for (u32 frame = 0; frame < 8; ++frame)
            shadow_frame(&f->shadow);
        f->deltas[effect] = region_difference(f, effect);
        printf("S16 contribution%u fixed-region HDR delta%g matched no-op drift%g\n", effect,
               f->deltas[effect], f->drift[effect]);
        EXPECT_TRUE(f->deltas[effect] > 1e-5);
        de_renderer_set_settings(r->r, &all);
        for (u32 frame = 0; frame < 8; ++frame)
            shadow_frame(&f->shadow);
    }
    de_render_settings base = all;
    base.tier = DE_TIER_BASE;
    de_renderer_set_settings(r->r, &base);
    for (u32 frame = 0; frame < 8; ++frame)
        shadow_frame(&f->shadow);
    owners(f, false);
    EXPECT_TRUE(f->gtao && f->ssr && !f->sun_mask && !f->di_consumed);
    f64 base_mean = hdr(f, NULL, "/tmp/s16-combined-base.ppm");
    EXPECT_TRUE(base_mean > 1e-4);
    de_renderer_set_settings(r->r, &all);
    for (u32 frame = 0; frame < 8; ++frame)
        shadow_frame(&f->shadow);
    owners(f, true);
    EXPECT_TRUE(hdr(f, NULL, NULL) > 1e-4);
    FILE *manifest = fopen("/tmp/s16-combined-metrics.json", "wb");
    ASSERT_TRUE(manifest);
    fprintf(manifest,
            "{\"rt_mean\":%.9g,\"base_mean\":%.9g,\"ao_min\":%.9g,"
            "\"sun_min\":%.9g,\"sun_max\":%.9g,\"mirror_mean\":%.9g,\"di_mean\":%.9g,"
            "\"gi_mean\":%.9g,\"glass_green\":%.9g,\"glass_valid\":%u,"
            "\"contribution_deltas\":[%.9g,%.9g,%.9g,%.9g,%.9g],"
            "\"noop_drifts\":[%.9g,%.9g,%.9g,%.9g,%.9g],\"warm_frames\":64,\"guard_frames\":8,"
            "\"renderer_alloc_delta\":%llu,"
            "\"renderer_realloc_delta\":%llu,\"device_alloc_delta\":%llu,\"device_realloc_delta\":%"
            "llu}\n",
            rt_mean, base_mean, ao_min, shadow_min, shadow_max, mirror, di, gi, green, valid,
            f->deltas[0], f->deltas[1], f->deltas[2], f->deltas[3], f->deltas[4], f->drift[0],
            f->drift[1], f->drift[2], f->drift[3], f->drift[4],
            (unsigned long long)(cpu_end.alloc_count - cpu.alloc_count),
            (unsigned long long)(cpu_end.realloc_count - cpu.realloc_count),
            (unsigned long long)(gpu_end.alloc_count - gpu.alloc_count),
            (unsigned long long)(gpu_end.realloc_count - gpu.realloc_count));
    fclose(manifest);
}
