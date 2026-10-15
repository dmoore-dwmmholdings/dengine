/*
 * vis_frame.c: the visibility-buffer frame (SPEC §12.5; DENG-229): the per-renderer state, the
 * engine-shader loader of the vis stages, the per-view context and the stage order, and the view
 * stage "vis.frame" the renderer registers. vis_stages.h describes the contract. The stages'
 * shaders are engine shaders (DENG-421): one line each in shaders/engine_shaders.cmake.
 *
 * Registration: views.c calls de__vis_frame_create at the end of de__views_create, which adds
 * "vis.frame" to the view stage list at (DE__VIEW_SLOT_VISIBILITY, DE__VIS_FRAME_ORDER), after
 * the GPU scene's "gpu_scene.view" (INT32_MIN + 1); de__views_destroy calls de__vis_frame_destroy.
 */
#include "render/passes/vis/vis_stages.h"

#include "render/graph/rg_internal.h"
#include "render/renderer_internal.h"
#include "render/world/render_world.h"

#include <stdio.h>
#include <string.h>

/* =================================================================================================
 * Layout constants (vis_types.slang, cull_common.slang)
 * ============================================================================================== */

static const de__vis_layout_const k_consts[] = {
    {"DE_VIS_CHUNK_MESHLETS", DE__VIS_CHUNK_MESHLETS},
    {"DE_VIS_MAX_GROUPS_X", DE__VIS_MAX_GROUPS_X},
    {"DE_VIS_ID_TRIANGLE_BITS", DE__VIS_ID_TRIANGLE_BITS},
    {"DE_VIS_ID_TRIANGLE_MASK", DE__VIS_ID_TRIANGLE_MASK},
    {"DE_VIS_ID_MAX_VISIBLE_MESHLETS", DE__VIS_ID_MAX_VISIBLE_MESHLETS},
    {"DE_VIS_ID_EMPTY", DE__VIS_ID_EMPTY},
    {"DE_VIS_COUNTER_WORK_ITEMS", DE__VIS_COUNTER_WORK_ITEMS},
    {"DE_VIS_COUNTER_INSTANCES_VISIBLE", DE__VIS_COUNTER_INSTANCES_VISIBLE},
    {"DE_VIS_COUNTER_INSTANCES_TESTED", DE__VIS_COUNTER_INSTANCES_TESTED},
    {"DE_VIS_COUNTER_MESHLETS", DE__VIS_COUNTER_MESHLETS},
    {"DE_VIS_COUNTER_BYTES", DE__VIS_COUNTER_BYTES},
    {"DE_VIS_CULL_FRUSTUM", DE__VIS_CULL_FRUSTUM},
    {"DE_VIS_CULL_FROZEN", DE__VIS_CULL_FROZEN},
    {"DE_VIS_CULL_OCCLUSION", DE__VIS_CULL_OCCLUSION},
    {"DE_VIS_CULL_PLANES", DE__VIS_CULL_PLANES},
};

const de__vis_layout_const *de__vis_layout_consts(u32 *count) {
    *count = (u32)(sizeof k_consts / sizeof k_consts[0]);
    return k_consts;
}

/* =================================================================================================
 * Engine shaders (DENG-421: shaders/engine_shaders.cmake, render/shaders/engine_shaders.h)
 * ============================================================================================== */

de_result de__vis_shader_load(de__vis_shared *s, const char *name, const char *const *entries,
                              u32 count, de__engine_shader_code *out) {
    char why[768] = "";
    const de_result r =
        de__engine_shader_load(s->dev, s->alloc, name, 0, entries, count, out, why, sizeof why);
    if (r == DE_OK)
        return DE_OK;
    if (r == DE_ERR_OUT_OF_MEMORY || r == DE_ERR_INVALID_ARGUMENT)
        return DE_FAIL(r, "vis: engine shader %s: %.600s", name, why);
    /* No code for this backend in this build (a cross build without the build-time cook, DXIL
     * without DXC): the frame is off. */
    return DE_FAIL(DE_ERR_UNSUPPORTED, "vis: engine shader %s: %.600s", name, why);
}

de_result de__vis_kernel_create(de__vis_shared *s, const de__engine_shader_code *code, u32 entry,
                                const char *name, const u32 threadgroup[3], u32 push_bytes,
                                de__vis_kernel *out) {
    memset(out, 0, sizeof *out);
    if (entry >= code->count)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "vis: %s: entry %u of %u", name, entry,
                       code->count);
    const de__engine_shader_entry *e = &code->entries[entry];
    const bool reflected = e->threadgroup[0] != 0; /* a placeholder may have none */
    if (reflected && (e->threadgroup[0] != threadgroup[0] || e->threadgroup[1] != threadgroup[1] ||
                      e->threadgroup[2] != threadgroup[2]))
        return DE_FAIL(DE_ERR_INTERNAL,
                       "vis: %s: [numthreads(%u, %u, %u)] in the shader, %u x %u x %u expected",
                       e->blob.entry, e->threadgroup[0], e->threadgroup[1], e->threadgroup[2],
                       threadgroup[0], threadgroup[1], threadgroup[2]);
    memcpy(out->threadgroup, threadgroup, sizeof out->threadgroup);
    out->push_bytes = e->push_constant_bytes > push_bytes ? e->push_constant_bytes : push_bytes;
    out->push_bytes = (out->push_bytes + 3u) & ~3u;
    out->pipeline = de_gpu_pipeline_create(
        s->dev, &(de_gpu_pipeline_desc){
                    .kind = DE_PIPE_COMPUTE,
                    .cs = e->blob,
                    .push_constant_bytes = out->push_bytes,
                    .threadgroup_size = {threadgroup[0], threadgroup[1], threadgroup[2]},
                    .name = name});
    if (!DE_HANDLE_VALID(out->pipeline)) {
        const de_error *er = de_error_last();
        return DE_FAIL_WRAP(er != NULL && er->result != DE_OK ? er->result : DE_ERR_INTERNAL,
                            "vis: the pipeline %s", name);
    }
    return DE_OK;
}

void de__vis_kernel_destroy(de__vis_shared *s, de__vis_kernel *k) {
    if (DE_HANDLE_VALID(k->pipeline))
        de_gpu_pipeline_destroy(s->dev, k->pipeline);
    memset(k, 0, sizeof *k);
}

/* =================================================================================================
 * Per renderer
 * ============================================================================================== */

typedef struct stage_fns {
    const char *name;
    de_result (*init)(de__vis_shared *s);
    void (*shutdown)(de__vis_shared *s);
} stage_fns;

static const stage_fns k_stages[DE__VIS_STAGE_COUNT] = {
    {"cull_instances", de__vis_cull_instances_init, de__vis_cull_instances_shutdown},
    {"cull_meshlets", de__vis_cull_meshlets_init, de__vis_cull_meshlets_shutdown},
    {"raster", de__vis_raster_init, de__vis_raster_shutdown},
    {"hzb_build", de__vis_hzb_build_init, de__vis_hzb_build_shutdown},
    {"raster_alpha", de__vis_raster_alpha_init, de__vis_raster_alpha_shutdown},
    {"classify", de__vis_classify_init, de__vis_classify_shutdown},
    {"resolve", de__vis_resolve_init, de__vis_resolve_shutdown},
};

static void register_cvars(de__vis_shared *s) {
    s->cv_cull_freeze = de_cvar_register(&(de_cvar_desc){
        .name = "r.cull_freeze",
        .help = "GPU culling: keep culling (and choosing LODs) from the camera of the frame this "
                "was turned on, to look at the culled scene from elsewhere",
        .type = DE_CVAR_BOOL,
        .flags = DE_CVAR_DEV_ONLY});
    s->cv_cull_disable = de_cvar_register(&(de_cvar_desc){
        .name = "r.cull_disable",
        .help = "GPU culling: no frustum (or occlusion) test; every instance is drawn",
        .type = DE_CVAR_BOOL,
        .flags = DE_CVAR_DEV_ONLY});
    s->cv_lod_error_px = de_cvar_register(&(de_cvar_desc){
        .name = "r.lod_error_px",
        .help = "Mesh LODs: the largest screen-space error, in pixels, of the LOD drawn (the "
                "coarsest LOD within it is chosen)",
        .type = DE_CVAR_FLOAT,
        .default_number = 1.0,
        .min = 0.0625,
        .max = 64.0});
    s->cv_lod_bias = de_cvar_register(&(de_cvar_desc){
        .name = "r.lod_bias",
        .help = "Mesh LODs: added to the chosen LOD (positive: coarser), clamped to the mesh's",
        .type = DE_CVAR_INT,
        .default_number = 0,
        .min = -7,
        .max = 7});
    /* DENG-233: occlusion and the HZB. */
    s->cv_occlusion = de_cvar_register(
        &(de_cvar_desc){.name = "r.occlusion",
                        .help = "GPU culling: two-phase occlusion culling against each view's HZB",
                        .type = DE_CVAR_BOOL,
                        .default_number = 1});
    s->cv_occlusion_reset = de_cvar_register(&(de_cvar_desc){
        .name = "r.occlusion_reset",
        .help = "GPU culling: forget the occlusion history for one frame (a camera cut); clears "
                "itself",
        .type = DE_CVAR_BOOL});
    s->cv_occlusion_cut_m = de_cvar_register(&(de_cvar_desc){
        .name = "r.occlusion_cut_m",
        .help = "GPU culling: a camera move longer than this (metres) between frames is a cut: "
                "the occlusion history is dropped for one frame",
        .type = DE_CVAR_FLOAT,
        .default_number = 10.0,
        .min = 0.01,
        .max = 1.0e6});
    s->cv_hzb_debug = de_cvar_register(
        &(de_cvar_desc){.name = "r.hzb_debug",
                        .help = "Show HZB mip N of each view instead of its colour (-1: off)",
                        .type = DE_CVAR_INT,
                        .flags = DE_CVAR_DEV_ONLY,
                        .default_number = -1,
                        .min = -1,
                        .max = DE__VIS_HZB_MAX_MIPS - 1});
    s->cv_hzb_single_pass = de_cvar_register(&(de_cvar_desc){
        .name = "r.hzb_single_pass",
        .help = "HZB: reduce 6 mips per dispatch through groupshared memory (0: one dispatch "
                "per mip)",
        .type = DE_CVAR_BOOL,
        .default_number = 1});
}

static void shutdown_stages(de__vis_shared *s, u32 count) {
    for (u32 i = count; i-- > 0;)
        k_stages[i].shutdown(s);
}

de_result de__vis_shared_create(const de__vis_shared_desc *desc, de__vis_shared **out) {
    if (out != NULL)
        *out = NULL;
    if (desc == NULL || out == NULL || desc->device == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__vis_shared_create: %s",
                       desc == NULL || out == NULL ? "NULL argument" : "no device");
    const de_allocator *a = desc->allocator != NULL ? desc->allocator : de_heap_allocator();
    de__vis_shared *s = DE_NEW(a, de__vis_shared, DE_MEM_RENDER);
    if (s == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__vis_shared_create: no memory");
    memset(s, 0, sizeof *s);
    s->renderer = desc->renderer;
    s->material_models = desc->material_models;
    s->dev = desc->device;
    s->alloc = a;
    s->log = de_log_register_category("render", DE_LOG_INFO);
    register_cvars(s);
    s->enabled = true;
    for (u32 i = 0; i < DE__VIS_STAGE_COUNT; ++i) {
        const de_result r = k_stages[i].init(s);
        if (r == DE_OK)
            continue;
        shutdown_stages(s, i);
        if (r != DE_ERR_UNSUPPORTED) {
            const de_result w = DE_FAIL_WRAP(r, "vis: the %s stage", k_stages[i].name);
            DE_FREE(a, s, sizeof *s);
            return w;
        }
        /* No shader code for this backend in this build (or a failed Dev cook): the frame is
         * off. */
        const de_error *e = de_error_last();
        snprintf(s->why, sizeof s->why, "%.250s", e != NULL ? e->message : "no engine shaders");
        s->enabled = false;
        DE_WARN(s->log, "vis: the visibility-buffer frame is off: %s", s->why);
        de_error_clear(); /* the create succeeds */
        break;
    }
    if (s->enabled) {
        /* DENG-233: the per-view visibility flags and the HZB debug kernel. */
        const de_result r = de__vis_occlusion_init(s);
        if (r == DE_ERR_UNSUPPORTED) {
            const de_error *e = de_error_last();
            snprintf(s->why, sizeof s->why, "%.250s", e != NULL ? e->message : "no shaders");
            shutdown_stages(s, DE__VIS_STAGE_COUNT);
            s->enabled = false;
            DE_WARN(s->log, "vis: the visibility-buffer frame is off: %s", s->why);
            de_error_clear();
        } else if (r != DE_OK) {
            shutdown_stages(s, DE__VIS_STAGE_COUNT);
            DE_FREE(a, s, sizeof *s);
            return DE_FAIL_WRAP(r, "vis: occlusion");
        }
    }
    /* DENG-239: picking (its CPU state also when the frame is off). */
    const de_result pr = de__vis_pick_init(s);
    if (pr != DE_OK) {
        de__vis_occlusion_shutdown(s);
        if (s->enabled)
            shutdown_stages(s, DE__VIS_STAGE_COUNT);
        DE_FREE(a, s, sizeof *s);
        return DE_FAIL_WRAP(pr, "vis: picking");
    }
    *out = s;
    return DE_OK;
}

void de__vis_shared_destroy(de__vis_shared *s) {
    if (s == NULL)
        return;
    de__vis_pick_shutdown(s);      /* DENG-239 */
    de__vis_occlusion_shutdown(s); /* DENG-233 */
    if (s->enabled)
        shutdown_stages(s, DE__VIS_STAGE_COUNT);
    DE_FREE(s->alloc, s, sizeof *s);
}

/* =================================================================================================
 * One view
 * ============================================================================================== */

/* "vis.depth_clear": the depth starts each frame at 0 (reverse-Z far). */
static void depth_clear_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const de_rg_resource *depth = (const de_rg_resource *)user;
    const de_gpu_attachment a = {
        .texture = de_rg_texture(pass, *depth), .clear = true, .clear_depth = 0.0f, .store = true};
    de_gpu_cmd_begin_render(cmd, NULL, 0, &a);
    de_gpu_cmd_end_render(cmd);
}

/* DENG-233: the view's depth (cleared, or the test's), its HZB and its visibility flags. */
static void prepare_occlusion(de__vis_shared *s, de_rg *g, const de__vis_frame_desc *d,
                              de__vis_context *vc) {
    if (DE_HANDLE_VALID(d->depth_override)) {
        vc->depth = d->depth_override;
        vc->depth_width = d->depth_override_width;
        vc->depth_height = d->depth_override_height;
    } else if (vc->width > 0 && vc->height > 0) {
        vc->depth = de_rg_create_texture(
            g, &(de_gpu_texture_desc){.format = DE_FMT_D32_FLOAT,
                                      .width = vc->width,
                                      .height = vc->height,
                                      .usage = DE_USAGE_DEPTH | DE_USAGE_SAMPLED |
                                               DE_USAGE_COPY_SRC, /* tests read it */
                                      .name = "vis.depth"});
        de_rg_resource *user = (de_rg_resource *)de_arena_push(g->arena, sizeof *user, 8);
        if (DE_HANDLE_VALID(vc->depth) && user != NULL) {
            *user = vc->depth;
            de_rg_pass *p =
                de_rg_add_pass(g, "vis.depth_clear", DE_QUEUE_GRAPHICS, depth_clear_execute, user);
            de_rg_write(p, vc->depth, DE_ACCESS_DEPTH_WRITE);
            vc->depth_width = vc->width;
            vc->depth_height = vc->height;
            vc->depth_target = true; /* DENG-234: raster draws into it */
        } else {
            vc->depth = (de_rg_resource){0};
        }
    }
    (void)s;
    de__vis_hzb_prepare(g, vc);
    de__vis_occlusion_prepare(g, vc);
}

static bool mat_is_zero(const de_mat4 *m) {
    for (u32 i = 0; i < 16; ++i)
        if (m->m[i] != 0.0f)
            return false;
    return true;
}

de_result de__vis_frame_build(de__vis_shared *s, de_rg *g, const de__vis_frame_desc *d,
                              de__vis_context **out) {
    if (out != NULL)
        *out = NULL;
    if (s == NULL || g == NULL || d == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__vis_frame_build: NULL argument");
    if (d->view_slot >= DE__VIEW_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__vis_frame_build: view slot %u", d->view_slot);
    if (!s->enabled)
        return DE_OK;
    if (g->arena == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__vis_frame_build: the graph is not begun");
    de__vis_context *vc = (de__vis_context *)de_arena_push_zero(g->arena, sizeof *vc, 16);
    if (vc == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__vis_frame_build: the frame arena is full");
    vc->shared = s;
    vc->graph = g;
    vc->arena = g->arena;
    vc->gpu_scene = d->gpu_scene;
    vc->view = d->view;
    vc->view_slot = d->view_slot;
    vc->view_new = s->views[d->view_slot].id != d->view.id;
    s->views[d->view_slot] = d->view;
    vc->frame_index = d->frame_index;
    vc->frames_completed = d->frames_completed;
    vc->width = d->width;
    vc->height = d->height;
    vc->view_matrix = d->view_matrix;
    vc->proj = d->proj;
    vc->view_proj = de_m4_mul(&d->proj, &d->view_matrix);
    vc->prev_view_proj = mat_is_zero(&d->prev_view_proj) ? vc->view_proj : d->prev_view_proj;
    vc->camera_position = d->camera_position;
    vc->origin = d->origin;
    vc->settings = d->settings;
    vc->layer_mask = d->layer_mask != 0 ? d->layer_mask : ~0u;
    vc->output_width = d->output_width != 0 ? d->output_width : d->width;     /* DENG-239 */
    vc->output_height = d->output_height != 0 ? d->output_height : d->height; /* DENG-239 */
    vc->work_capacity_fixed = d->work_capacity_fixed;
    vc->cull_freeze = de_cvar_bool(s->cv_cull_freeze);
    vc->cull_disable = de_cvar_bool(s->cv_cull_disable);
    vc->lod_error_px = (f32)de_cvar_float(s->cv_lod_error_px);
    if (!(vc->lod_error_px > 0.0f))
        vc->lod_error_px = 1.0f;
    vc->lod_bias = (i32)de_cvar_int(s->cv_lod_bias);

    if (d->gpu_scene != NULL) {
        const de__gpu_scene_table_info ti = de__gpu_scene_table(d->gpu_scene, DE__GS_INSTANCES);
        vc->instance_count = ti.count;
        vc->scene_views_bindless = de__gpu_scene_table(d->gpu_scene, DE__GS_VIEWS).bindless;
        if (vc->instance_count > 0) {
            vc->scene_instances = de__gpu_scene_import(d->gpu_scene, g, DE__GS_INSTANCES);
            vc->scene_meshes = de__gpu_scene_import(d->gpu_scene, g, DE__GS_MESHES);
            vc->scene_materials = de__gpu_scene_import(d->gpu_scene, g, DE__GS_MATERIALS);
            /* DENG-237: classification reads the table's entries */
            vc->scene_material_count = de__gpu_scene_table(d->gpu_scene, DE__GS_MATERIALS).count;
            vc->scene_views = de__gpu_scene_import(d->gpu_scene, g, DE__GS_VIEWS);
        }
    }

    /* DENG-233: the depth, the HZB and the visibility flags of a view that culls. */
    if (vc->instance_count > 0)
        prepare_occlusion(s, g, d, vc);

    de__vis_cull_instances(g, vc, DE__VIS_PHASE_1);
    de__vis_cull_meshlets(g, vc, DE__VIS_PHASE_1);
    de__vis_raster(g, vc, DE__VIS_PHASE_1);
    de__vis_raster_alpha(g, vc, DE__VIS_PHASE_1); /* DENG-236: before the HZB (cutout occluders) */
    de__vis_hzb_build(g, vc);
    de__vis_cull_instances(g, vc, DE__VIS_PHASE_2);
    de__vis_cull_meshlets(g, vc, DE__VIS_PHASE_2);
    de__vis_raster(g, vc, DE__VIS_PHASE_2);
    de__vis_raster_alpha(g, vc, DE__VIS_PHASE_2);
    de__vis_pick(g, vc); /* DENG-239: reads this frame's vis IDs and visible list */
    de__vis_classify(g, vc);
    de__vis_resolve(g, vc);
    if (out != NULL)
        *out = vc;
    return DE_OK;
}

/* =================================================================================================
 * The renderer's view stage
 * ============================================================================================== */

static void vis_frame_stage(de__view_frame *vf, void *user) {
    de__vis_shared *s = (de__vis_shared *)user;
    de_renderer *r = vf->renderer;
    if (!s->enabled || r->gpu_scene == NULL)
        return;
    de__vis_frame_desc d = {.gpu_scene = r->gpu_scene,
                            .view = vf->view,
                            .view_slot = (vf->view.id & 0xFFu) - 1u, /* views.c: slot + 1 */
                            .frame_index = vf->frame_index,
                            .frames_completed = de_gpu_fence_value(r->dev, r->frame_fence),
                            .width = vf->width,
                            .height = vf->height,
                            .view_matrix = vf->view_matrix,
                            .proj = vf->proj,
                            .prev_view_proj = vf->prev_view_proj,
                            .settings = vf->settings,
                            .layer_mask = vf->layer_mask,       /* DENG-239 */
                            .output_width = vf->viewport_px[2], /* DENG-239: pick coordinates */
                            .output_height = vf->viewport_px[3]};
    if (vf->camera != NULL)
        d.camera_position = vf->camera->offset;
    if (vf->world != NULL)
        d.origin = vf->world->origin;
    de__vis_context *vc = NULL;
    const de_result res = de__vis_frame_build(s, vf->graph, &d, &vc);
    if (res != DE_OK) {
        const de_error *e = de_error_last();
        DE_ERROR(s->log, "vis: view \"%s\": %s", vf->name,
                 e != NULL ? e->message : de_result_str(res));
    }
    /* de_render_stats.instances_visible: the sum over this frame's views of what each read
     * back (one to a few frames late). */
    if (!s->stats_any || s->stats_frame != vf->frame_index) {
        s->stats_any = true;
        s->stats_frame = vf->frame_index;
        r->stats.instances_visible = 0;
    }
    if (vc != NULL && vc->stats.valid)
        r->stats.instances_visible += vc->stats.instances_visible;
    /* r.hzb_debug (DENG-233): the view shows one HZB mip instead of its colour. */
    const i64 mip = DE_HANDLE_VALID(s->cv_hzb_debug) ? de_cvar_int(s->cv_hzb_debug) : -1;
    if (vc != NULL && mip >= 0) {
        const de_rg_resource dbg = de__vis_hzb_debug(vf->graph, vc, (u32)mip);
        if (DE_HANDLE_VALID(dbg)) {
            vf->color = dbg;
            vf->color_format = DE_FMT_RGBA16_FLOAT;
        }
    }
}

de_result de__vis_frame_create(de_renderer *r, struct de__vis_shared **out) {
    *out = NULL;
    de__vis_shared *s = NULL;
    de_result res =
        de__vis_shared_create(&(de__vis_shared_desc){.device = r->dev,
                                                     .allocator = r->alloc,
                                                     .renderer = r,
                                                     .material_models = r->material_models},
                              &s);
    if (res != DE_OK)
        return res;
    res = de__view_register_stage(r, DE__VIEW_SLOT_VISIBILITY, DE__VIS_FRAME_ORDER, "vis.frame",
                                  vis_frame_stage, s);
    if (res != DE_OK) {
        de__vis_shared_destroy(s);
        return res;
    }
    *out = s;
    return DE_OK;
}

void de__vis_frame_destroy(de_renderer *r, struct de__vis_shared *s) {
    (void)r; /* the stage list goes with the views */
    de__vis_shared_destroy(s);
}
