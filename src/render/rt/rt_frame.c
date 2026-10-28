/* DENG-44: one GPU-produced TLAS per view, before any hybrid lighting effect. */
#include "render/rt/rt_internal.h"
#include "render/passes/vis/vis_stages.h"
#include "render/graph/rg_internal.h"
#include <string.h>
typedef struct rt_pass {
    de__rt_scene *scene;
    de__rt_context context;
    u32 pc[8];
} rt_pass;
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    (void)pass;
    rt_pass *p = user;
    de_gpu_cmd_set_pipeline(cmd, p->scene->generate);
    de_gpu_cmd_push_constants(cmd, p->pc, sizeof p->pc);
    de_gpu_cmd_dispatch(cmd, p->context.count ? (p->context.count + 63) / 64 : 1, 1, 1);
    de_gpu_barrier b = {.buffer = p->context.instances,
                        .before = DE_ACCESS_SHADER_WRITE,
                        .after = DE_ACCESS_ACCEL_READ};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_build_tlas(cmd, p->context.tlas, p->context.instances, p->context.count, false);
    de_gpu_barrier ready = {
        .accel = p->context.tlas, .before = DE_ACCESS_ACCEL_WRITE, .after = DE_ACCESS_ACCEL_READ};
    de_gpu_cmd_barriers(cmd, &ready, 1);
}
static de_result reserve(de__rt_scene *s, de__rt_view *v, u32 capacity) {
    de_gpu_device *d = s->renderer->dev;
    if (capacity > v->context.capacity) {
        u32 cap = v->context.capacity ? v->context.capacity : 16;
        while (cap < capacity)
            cap *= 2;
        de_gpu_buffer instances = de_gpu_buffer_create(
            d, &(de_gpu_buffer_desc){.size = (u64)cap * 60,
                                     .usage = DE_USAGE_STORAGE | DE_USAGE_ACCEL_INPUT |
                                              DE_USAGE_COPY_SRC,
                                     .name = "RT instances"});
        de_gpu_buffer lods = de_gpu_buffer_create(
            d, &(de_gpu_buffer_desc){.size = (u64)cap * 4,
                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                     .name = "RT selected LODs"});
        de_gpu_accel tlas = de_gpu_accel_create(d, &(de_gpu_accel_desc){.is_tlas = true,
                                                                        .max_instances = cap,
                                                                        .prefer_fast_trace = true,
                                                                        .name = "RT scene TLAS"});
        if (!instances.id || !lods.id || !tlas.id) {
            de_gpu_buffer_destroy(d, instances);
            de_gpu_buffer_destroy(d, lods);
            de_gpu_accel_destroy(d, tlas);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RT TLAS capacity");
        }
        de_gpu_accel_destroy(d, v->context.tlas);
        de_gpu_buffer_destroy(d, v->context.instances);
        de_gpu_buffer_destroy(d, v->context.selected_lods);
        v->context.tlas = tlas;
        v->context.instances = instances;
        v->context.selected_lods = lods;
        v->context.capacity = cap;
    }
    if (!v->dependency.id)
        v->dependency = de_gpu_buffer_create(
            d, &(de_gpu_buffer_desc){
                   .size = 4, .usage = DE_USAGE_STORAGE, .name = "RT TLAS graph dependency"});
    u32 ring = (u32)(s->renderer->frame_index % DE__RT_RING);
    if (!v->cull[ring].id)
        v->cull[ring] =
            de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = sizeof(de__vis_cull_view),
                                                          .stride = sizeof(de__vis_cull_view),
                                                          .usage = DE_USAGE_STORAGE,
                                                          .memory = DE_MEM_UPLOAD,
                                                          .name = "RT cull view"});
    if (!v->query[ring].id)
        v->query[ring] = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = 32,
                                                                       .stride = 32,
                                                                       .usage = DE_USAGE_STORAGE,
                                                                       .memory = DE_MEM_UPLOAD,
                                                                       .name = "RT query context"});
    return v->dependency.id && v->cull[ring].id && v->query[ring].id
               ? DE_OK
               : DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RT view buffers");
}
void de__rt_view_stage(de__view_frame *vf, void *user) {
    de_renderer *r = user;
    de__rt_scene *s = r->rt_scene;
    if (!s || vf->settings->tier != DE_TIER_RT)
        return;
    const de__vis_context *vc =
        de__vis_frame_context(de__views_vis(r), vf->view, vf->graph, vf->frame_index);
    if (!vc)
        return;
    u32 slot = (vf->view.id & 255u) - 1u;
    if (slot >= 32)
        return;
    de__rt_view *v = &s->views[slot];
    de__gpu_scene_table_info inst = de__gpu_scene_table(r->gpu_scene, DE__GS_INSTANCES);
    if (reserve(s, v, inst.count ? inst.count : 1) != DE_OK) {
        vf->graph->oom = true;
        return;
    }
    u32 ring = (u32)(vf->frame_index % DE__RT_RING);
    de__vis_cull_view cull = vc->cull;
    cull.layer_mask = vf->layer_mask;
    memcpy(de_gpu_buffer_map(r->dev, v->cull[ring]), &cull, sizeof cull);
    v->context.view = vf->view;
    v->context.graph = vf->graph;
    v->context.frame = vf->frame_index;
    v->context.count = inst.count;
    v->context.tlas_index = de_gpu_accel_bindless(r->dev, v->context.tlas);
    v->context.metadata = de_gpu_buffer_bindless(r->dev, s->metadata[s->metadata_index]);
    v->context.scene_slot = vc->scene_view_slot;
    v->context.scene = de__gpu_scene_table(r->gpu_scene, DE__GS_VIEWS).buffer;
    v->context.query = v->query[ring];
    const u32 query[8] = {de_gpu_buffer_bindless(r->dev, v->context.scene),
                          vc->scene_view_slot,
                          v->context.metadata,
                          de_gpu_buffer_bindless(r->dev, v->context.selected_lods),
                          de_gpu_sampler_bindless(r->dev, s->sampler),
                          vf->layer_mask,
                          0,
                          v->context.tlas_index};
    memcpy(de_gpu_buffer_map(r->dev, v->context.query), query, sizeof query);
    v->context.dependency = de_rg_import_buffer(vf->graph, v->dependency, DE_ACCESS_SHADER_READ);
    rt_pass *p = de_arena_push_zero(vf->graph->arena, sizeof *p, 16);
    if (!p) {
        vf->graph->oom = true;
        return;
    }
    p->scene = s;
    p->context = v->context;
    const u32 pc[8] = {inst.bindless,
                       de_gpu_buffer_bindless(r->dev, v->context.instances),
                       de__gpu_scene_table(r->gpu_scene, DE__GS_MESHES).bindless,
                       v->context.metadata,
                       inst.count,
                       de_gpu_buffer_bindless(r->dev, v->cull[ring]),
                       de_gpu_buffer_bindless(r->dev, v->context.selected_lods),
                       de_gpu_buffer_bindless(r->dev, v->dependency)};
    memcpy(p->pc, pc, sizeof pc);
    de_rg_pass *pass = de_rg_add_pass(vf->graph, "RT.BuildTLAS", DE_QUEUE_GRAPHICS, execute, p);
    if (!pass) {
        vf->graph->oom = true;
        return;
    }
    de_rg_read(pass, de__gpu_scene_import(r->gpu_scene, vf->graph, DE__GS_INSTANCES),
               DE_ACCESS_SHADER_READ);
    if (vc->hidden_entities.id)
        de_rg_read(pass, vc->hidden_entities, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, de__gpu_scene_import(r->gpu_scene, vf->graph, DE__GS_MESHES),
               DE_ACCESS_SHADER_READ);
    de_rg_read(
        pass, de_rg_import_buffer(vf->graph, s->metadata[s->metadata_index], DE_ACCESS_SHADER_READ),
        DE_ACCESS_SHADER_READ);
    de_rg_write(pass, de_rg_import_buffer(vf->graph, v->context.instances, DE_ACCESS_ACCEL_READ),
                DE_ACCESS_SHADER_WRITE);
    de_rg_write(pass,
                de_rg_import_buffer(vf->graph, v->context.selected_lods, DE_ACCESS_SHADER_READ),
                DE_ACCESS_SHADER_WRITE);
    de_rg_write(pass, v->context.dependency, DE_ACCESS_SHADER_WRITE);
    de_rg_export(vf->graph, v->context.dependency, DE_ACCESS_SHADER_READ);
    de_rg_pass_side_effect(pass);
}
de__rt_context *de__rt_context_get(de_renderer *r, de_view view, de_rg *g, u64 frame) {
    if (!r || !r->rt_scene)
        return NULL;
    u32 slot = (view.id & 255u) - 1u;
    if (slot >= 32)
        return NULL;
    de__rt_context *c = &r->rt_scene->views[slot].context;
    return c->view.id == view.id && c->graph == g && c->frame == frame ? c : NULL;
}

void de__rt_trace(de_renderer *r, de_gpu_cmd *cmd, const de__rt_context *context,
                  de_gpu_buffer rays, de_gpu_buffer hits, u32 count, u32 flags) {
    if (!r || !r->rt_scene || !context || !context->tlas.id || !count)
        return;
    const u32 pc[5] = {de_gpu_buffer_bindless(r->dev, context->query),
                       de_gpu_buffer_bindless(r->dev, rays), de_gpu_buffer_bindless(r->dev, hits),
                       count, flags};
    de_gpu_cmd_set_pipeline(cmd, r->rt_scene->trace);
    de_gpu_cmd_push_constants(cmd, pc, sizeof pc);
    const u32 groups = (u32)(((u64)count + 63) / 64);
    de_gpu_cmd_dispatch(cmd, groups < 65535 ? groups : 65535, (groups + 65534) / 65535, 1);
}

u64 de__rt_sample_frame(const de_renderer *r, u64 frame) {
#if (defined(DE_DEBUG) || defined(DE_DEV)) && !defined(DE_SHIPPING)
    de_cvar cv = r && r->rt_scene ? r->rt_scene->golden_sample : de_cvar_find("r.rt.golden_sample");
    if (cv.id) {
        i64 value = de_cvar_int(cv);
        if (value >= 0)
            return (u32)value;
    }
#else
    (void)r;
#endif
    return frame;
}
