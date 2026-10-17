#ifndef DE_TEST_RT_SHADOW_FIXTURE_H
#define DE_TEST_RT_SHADOW_FIXTURE_H
/* Actual renderer RT shadow production integration, using the shared native scene fixture. */
#include "scene_as_fixture.h"
#include "render/rt/rt_shadows.h"
#ifndef SW
#define SW 256u
#endif
#ifndef SH
#define SH 128u
#endif
typedef struct shadow_fixture {
    rt_fixture rt;
    de__rw_view camera;
    de_gpu_buffer mask_rb, hdr_rb;
    f32 sum[SW * SH];
    bool rt_pass, raster, local_raster, di_active;
    f32 pre_exposure;
} shadow_fixture;
typedef struct hdr_pass {
    shadow_fixture *f;
    de_rg_resource target;
} hdr_pass;
static inline void hdr_copy(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    hdr_pass *p = user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, p->f->hdr_rb, 0, SW * 8, de_rg_texture(pass, p->target),
                                      0, 0);
}
static inline void hdr_stage(de__view_frame *vf, void *user) {
    shadow_fixture *f = user;
    if (vf->view.id != f->rt.view.id)
        return;
    const de__lighting_context *lc = de__lighting_frame_context(
        de__views_lighting(f->rt.r), vf->view, vf->graph, vf->frame_index);
    ASSERT_TRUE(lc != NULL);
    f->pre_exposure = lc->view.pre_exposure;
    f->rt_pass = false;
    f->raster = lc->shadow_records.id != 0;
    f->local_raster = lc->atlas_records.id != 0;
    f->di_active = lc->restir_diffuse.id != 0;
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
static inline void shadow_fixture_setup(shadow_fixture *f) {
    rt_fixture *r = &f->rt;
    rt_fixture_setup(r);
    (void)cvar_set("r.rt.restir_di", "0");
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
static inline void shadow_fixture_teardown(shadow_fixture *f) {
    rt_fixture *r = &f->rt;
    if (r->d) {
        de_gpu_wait_idle(r->d);
        mesh_free(&g_mesh[1], r->d);
        de_gpu_buffer_destroy(r->d, f->mask_rb);
        de_gpu_buffer_destroy(r->d, f->hdr_rb);
    }
    (void)cvar_set("r.rt.restir_di", "-1");
    rt_fixture_teardown(r);
}
static inline void shadow_frame(shadow_fixture *f) {
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
static inline const u16 *shadow_read(shadow_fixture *f, de__rt_shadows_info *info) {
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

#endif
