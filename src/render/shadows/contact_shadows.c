#include "render/shadows/contact_internal.h"
#include "render/renderer_internal.h"
#include "render/graph/rg_internal.h"
#include "render/passes/vis/gbuffer.h"
#include <math.h>
#include <string.h>

static f32 brightness(const DeLight *l) {
    f32 intensity = l->intensity;
    if (l->kind == DE_LIGHT_POINT)
        intensity = de__light_point_candela(intensity);
    else if (l->kind == DE_LIGHT_SPOT)
        intensity = de__light_spot_candela(intensity, l->spot_outer);
    else if (l->kind == DE_LIGHT_RECT)
        intensity *= l->area_size.x * l->area_size.y;
    else if (l->kind == DE_LIGHT_DISK)
        intensity *= 3.14159265358979323846f * l->area_size.x * l->area_size.x * .25f;
    return intensity * fmaxf(l->color.x, fmaxf(l->color.y, l->color.z));
}
static de__contact_light record(const de__rw_lights *w, u32 i, u32 channel, f32 cutoff) {
    const DeLight *l = &w->light[i];
    const de_affine *t = &w->transform[i];
    de_vec3 d = de_v3_normalize_safe((de_vec3){-t->m[6], -t->m[7], -t->m[8]}, (de_vec3){0, 0, -1});
    return (de__contact_light){
        .entity = {(u32)w->entity[i], (u32)(w->entity[i] >> 32)},
        .kind = (u32)l->kind,
        .channel = channel,
        .position_range = {t->m[9], t->m[10], t->m[11], de__atlas_light_range(l, cutoff)},
        .direction_radius = {d.x, d.y, d.z, l->source_radius}};
}
void de__contact_select(const de__rw_lights *w, u64 directional, u32 max_locals, f32 cutoff,
                        de__contact_metadata *out) {
    memset(out, 0, sizeof *out);
    if (!w)
        return;
    if (max_locals > 4u)
        max_locals = 4u;
    u32 selected[4] = {0}, count = 0;
    f32 scores[4] = {0};
    for (u32 i = 0; i < w->count; ++i) {
        const DeLight *l = &w->light[i];
        if (!w->entity[i] || !l->cast_shadows || !(l->intensity > 0.f) || !isfinite(l->intensity))
            continue;
        if (l->kind == DE_LIGHT_DIRECTIONAL) {
            if (directional && w->entity[i] == directional) {
                out->light[0] = record(w, i, 0, cutoff);
                out->header[3] = 1;
            }
            continue;
        }
        if (l->kind > DE_LIGHT_DISK || !max_locals)
            continue;
        const f32 score = brightness(l);
        if (!(score > 0.f) || !isfinite(score))
            continue;
        u32 at = 0;
        while (at < count && (scores[at] > score ||
                              (scores[at] == score && w->entity[selected[at]] < w->entity[i])))
            ++at;
        if (at >= max_locals)
            continue;
        if (count < max_locals)
            ++count;
        for (u32 j = count - 1u; j > at; --j) {
            selected[j] = selected[j - 1u];
            scores[j] = scores[j - 1u];
        }
        selected[at] = i;
        scores[at] = score;
    }
    out->header[2] = count;
    for (u32 i = 0; i < count; ++i)
        out->light[i + 1u] = record(w, selected[i], i, cutoff);
}

typedef struct contact_view {
    de_view view;
    de_gpu_buffer buffer[DE__VIS_FRAME_RING];
    de__contact_metadata *mapped[DE__VIS_FRAME_RING];
    u64 frame[DE__VIS_FRAME_RING], pending_frame;
    bool live[DE__VIS_FRAME_RING];
    const de_rg *pending;
    de__contact_info info;
} contact_view;
struct de__contact {
    de_gpu_device *dev;
    const de_allocator *alloc;
    de__vis_shared *vis;
    de_renderer *renderer;
    de__vis_kernel kernel;
    de_cvar length, steps, locals, thickness;
    contact_view views[DE__VIEW_MAX];
};
typedef struct contact_pass {
    de__contact *s;
    contact_view *v;
    de__contact_params pc;
    de_rg_resource depth, normal, records, packed, directional, local;
    u32 ring;
    bool publish;
} contact_pass;
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    contact_pass *p = user;
    de__contact_params pc = p->pc;
    if (p->publish) {
        de_gpu_texture d = de_rg_texture(pass, p->directional), l = de_rg_texture(pass, p->local);
        de_gpu_buffer b = de_rg_buffer(pass, p->packed);
        de_gpu_cmd_copy_buffer_to_texture(cmd, d, 0, 0, b, 0, pc.pitch);
        de_gpu_cmd_copy_buffer_to_texture(cmd, l, 0, 0, b, pc.local_offset, pc.pitch);
        // IDs were fixed before cs_light_prep was built. Only these resource indices resolve
        // during recording; every recorded list completes before GPU submission begins.
        p->v->mapped[p->ring]->header[0] = de_gpu_texture_bindless(p->s->dev, d, 0, false);
        p->v->mapped[p->ring]->header[1] = de_gpu_texture_bindless(p->s->dev, l, 0, false);
        p->v->info.metadata.header[0] = p->v->mapped[p->ring]->header[0];
        p->v->info.metadata.header[1] = p->v->mapped[p->ring]->header[1];
        p->v->info.directional = d;
        p->v->info.local = l;
        return;
    }
    pc.depth = de_gpu_texture_bindless(p->s->dev, de_rg_texture(pass, p->depth), 0, false);
    pc.normal = de_gpu_texture_bindless(p->s->dev, de_rg_texture(pass, p->normal), 0, false);
    pc.records = de_gpu_buffer_bindless(p->s->dev, de_rg_buffer(pass, p->records));
    pc.packed = de_gpu_buffer_bindless(p->s->dev, de_rg_buffer(pass, p->packed));
    de_gpu_cmd_set_pipeline(cmd, p->s->kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, (pc.width + 7u) / 8u, (pc.height + 7u) / 8u, 1);
}
static void release(de__contact *s, contact_view *v) {
    for (u32 i = 0; i < DE__VIS_FRAME_RING; ++i)
        de_gpu_buffer_destroy(s->dev, v->buffer[i]);
    memset(v, 0, sizeof *v);
}
de_result de__contact_build(de__contact *s, const de__view_frame *vf, const de__vis_context *vc,
                            const de__contact_config *config, u64 directional,
                            de__lighting_inputs *inputs) {
    if (!s || !vf || !vf->view.id || !vf->graph || !vf->settings || !inputs)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "contact build arguments");
    u32 slot = (vf->view.id & 255u) - 1u;
    if (slot >= DE__VIEW_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "contact view slot");
    contact_view *v = &s->views[slot];
    if (v->pending)
        return DE_FAIL(DE_ERR_BUSY, "contact graph pending");
    if (v->view.id && v->view.id != vf->view.id)
        release(s, v);
    v->view = vf->view;
    memset(&v->info, 0, sizeof v->info);
    de__contact_config cfg = config ? *config
                                    : (de__contact_config){.length = .3f,
                                                           .thickness = .025f,
                                                           .normal_bias = .001f,
                                                           .steps = 16u,
                                                           .max_locals = 4u};
    if (vf->settings->shadows == DE_QUALITY_OFF || vf->probe_capture || cfg.length == 0.f)
        return DE_OK;
    if (!isfinite(cfg.length) || cfg.length < 0.f || !isfinite(cfg.thickness) ||
        cfg.thickness <= 0.f || !isfinite(cfg.normal_bias) || cfg.normal_bias < 0.f ||
        cfg.steps < 1u || cfg.steps > 64u || cfg.max_locals > 4u)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "contact configuration");
    if (!vc || !vc->depth.id || !vc->gbuffer[DE__GB1].id || !vf->world || !vf->width || !vf->height)
        return DE_OK;
    if (!s->kernel.pipeline.id)
        return DE_FAIL(DE_ERR_INTERNAL, "contact kernel unavailable");
    de__contact_metadata data;
    de_cvar cutoff = de_cvar_find("r.light_cutoff_lux");
    de__contact_select(&vf->world->lights, directional, cfg.max_locals,
                       cutoff.id ? (f32)de_cvar_float(cutoff) : .05f, &data);
    if (!data.header[2] && !data.header[3])
        return DE_OK;
    const u32 ring = (u32)(vf->frame_index % DE__VIS_FRAME_RING);
    if (v->live[ring] && v->frame[ring] >= vc->frames_completed)
        return DE_FAIL(DE_ERR_BUSY, "contact metadata in flight");
    if (!v->buffer[ring].id) {
        v->buffer[ring] =
            de_gpu_buffer_create(s->dev, &(de_gpu_buffer_desc){.size = sizeof data,
                                                               .stride = 16,
                                                               .memory = DE_MEM_UPLOAD,
                                                               .usage = DE_USAGE_STORAGE,
                                                               .name = "contact.metadata"});
        if (!v->buffer[ring].id ||
            !(v->mapped[ring] = de_gpu_buffer_map(s->dev, v->buffer[ring]))) {
            de_gpu_buffer_destroy(s->dev, v->buffer[ring]);
            v->buffer[ring] = (de_gpu_buffer){0};
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "contact metadata");
        }
    }
    memcpy(v->mapped[ring], &data, sizeof data);
    v->info.metadata = data;
    v->info.steps = cfg.steps;
    v->pending = vf->graph;
    v->pending_frame = vf->frame_index;
    de_rg *g = vf->graph;
    const u64 pitch = ((u64)vf->width * 8u + 255u) & ~(u64)255u;
    const u64 offset = (pitch * vf->height + 511u) & ~(u64)511u;
    if (pitch > UINT32_MAX || offset > UINT32_MAX || offset + pitch * vf->height > UINT32_MAX)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "contact output extent");
    contact_pass *p = de_arena_push_zero(g->arena, 2 * sizeof *p, 16);
    if (!p)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "contact graph parameters");
    de_gpu_texture_desc td = {.width = vf->width,
                              .height = vf->height,
                              .format = DE_FMT_RGBA16_FLOAT,
                              .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC,
                              .name = "contact.directional"};
    de_rg_resource d = de_rg_create_texture(g, &td);
    td.name = "contact.local";
    de_rg_resource l = de_rg_create_texture(g, &td);
    de_rg_resource packed =
        de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = offset + pitch * vf->height,
                                                     .stride = 4,
                                                     .memory = DE_MEM_GPU_ONLY,
                                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                     .name = "contact.packed"});
    de_rg_resource records = de_rg_import_buffer(g, v->buffer[ring], DE_ACCESS_SHADER_READ);
    if (!d.id || !l.id || !packed.id || !records.id)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "contact graph resources");
    p[0] = (contact_pass){.s = s,
                          .v = v,
                          .ring = ring,
                          .depth = vc->depth,
                          .normal = vc->gbuffer[DE__GB1],
                          .records = records,
                          .packed = packed,
                          .directional = d,
                          .local = l,
                          .pc = {.scene = vc->scene_views_bindless,
                                 .slot = vc->scene_view_slot,
                                 .width = vf->width,
                                 .height = vf->height,
                                 .pitch = (u32)pitch,
                                 .local_offset = (u32)offset,
                                 .steps = cfg.steps,
                                 .frame = (u32)vf->frame_index,
                                 .length = cfg.length,
                                 .thickness = cfg.thickness,
                                 .normal_bias = cfg.normal_bias}};
    p[1] = p[0];
    p[1].publish = true;
    de_rg_pass *pass = de_rg_add_pass(g, "contact.trace", DE_QUEUE_GRAPHICS, execute, &p[0]);
    if (!pass)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "contact trace pass");
    de_rg_read(pass, vc->scene_views, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, vc->depth, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, vc->gbuffer[DE__GB1], DE_ACCESS_SHADER_READ);
    de_rg_read(pass, records, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, packed, DE_ACCESS_SHADER_WRITE);
    pass = de_rg_add_pass(g, "contact.publish", DE_QUEUE_GRAPHICS, execute, &p[1]);
    if (!pass)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "contact publish pass");
    de_rg_read(pass, packed, DE_ACCESS_COPY_SRC);
    de_rg_write(pass, d, DE_ACCESS_COPY_DST);
    de_rg_write(pass, l, DE_ACCESS_COPY_DST);
    inputs->contact_records = records;
    inputs->contact_shadows = d;
    inputs->contact_local = l;
    return DE_OK;
}
static void stage(de__view_frame *vf, void *user) {
    de__contact *s = user;
    if (!s->vis->enabled)
        return;
    u32 slot = (vf->view.id & 255u) - 1u;
    de__lighting_inputs *inputs = de__lighting_view_inputs(de__views_lighting(s->renderer), slot);
    u64 directional = 0;
    de__csm_info csm;
    if (inputs && inputs->shadow_records.id && de__csm_get(s->renderer->csm, vf->view, &csm) &&
        csm.lights)
        directional = (u64)csm.data[1].identity[0] | ((u64)csm.data[1].identity[1] << 32);
    de__contact_config cfg = {.length = (f32)de_cvar_float(s->length),
                              .thickness = (f32)de_cvar_float(s->thickness),
                              .normal_bias = .001f,
                              .steps = (u32)de_cvar_int(s->steps),
                              .max_locals = (u32)de_cvar_int(s->locals)};
    const de__vis_context *vc = de__vis_frame_context(s->vis, vf->view, vf->graph, vf->frame_index);
    if (de__contact_build(s, vf, vc, &cfg, directional, inputs) != DE_OK)
        ++vf->graph->error_count;
}
de_result de__contact_create_ex(de_gpu_device *dev, const de_allocator *alloc, de__vis_shared *vis,
                                de__contact **out) {
    if (!dev || !alloc || !vis || !out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "contact create arguments");
    *out = NULL;
    de__contact *s = DE_NEW(alloc, de__contact, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "contact state");
    memset(s, 0, sizeof *s);
    s->dev = dev;
    s->alloc = alloc;
    s->vis = vis;
    if (vis->enabled) {
        const char *entry = "cs_contact_shadows";
        const u32 group[3] = {8, 8, 1};
        de__engine_shader_code code;
        de_result result = de__vis_shader_load(vis, "contact_shadows", &entry, 1, &code);
        if (result == DE_OK) {
            result = de__vis_kernel_create(vis, &code, 0, entry, group, sizeof(de__contact_params),
                                           &s->kernel);
            de__engine_shader_code_free(&code);
        }
        if (result != DE_OK) {
            de__contact_destroy(s);
            return result;
        }
    }
    *out = s;
    return DE_OK;
}
de_result de__contact_create(de_renderer *r, de__contact **out) {
    DE_TRY(de__contact_create_ex(r->dev, r->alloc, de__views_vis(r), out));
    de__contact *s = *out;
    s->renderer = r;
    s->length = de_cvar_register(&(de_cvar_desc){.name = "r.contact_shadow_length",
                                                 .type = DE_CVAR_FLOAT,
                                                 .default_number = .3,
                                                 .min = 0,
                                                 .max = 10});
    s->steps = de_cvar_register(&(de_cvar_desc){.name = "r.contact_shadow_steps",
                                                .type = DE_CVAR_INT,
                                                .default_number = 16,
                                                .min = 1,
                                                .max = 64});
    s->locals = de_cvar_register(&(de_cvar_desc){.name = "r.contact_shadow_local_lights",
                                                 .type = DE_CVAR_INT,
                                                 .default_number = 4,
                                                 .min = 0,
                                                 .max = 4});
    s->thickness = de_cvar_register(&(de_cvar_desc){.name = "r.contact_shadow_thickness",
                                                    .type = DE_CVAR_FLOAT,
                                                    .default_number = .025,
                                                    .min = .0001,
                                                    .max = 1});
    de_result result = de__view_register_stage(r, DE__VIEW_SLOT_LIGHTING, -31, "contact", stage, s);
    if (result != DE_OK) {
        de__contact_destroy(s);
        *out = NULL;
    }
    return result;
}
void de__contact_complete(de__contact *s, de_view view, const de_rg *graph, bool attempted) {
    u32 slot = (view.id & 255u) - 1u;
    if (!s || slot >= DE__VIEW_MAX)
        return;
    contact_view *v = &s->views[slot];
    if (v->view.id != view.id || v->pending != graph)
        return;
    if (attempted) {
        u32 ring = (u32)(v->pending_frame % DE__VIS_FRAME_RING);
        v->live[ring] = true;
        v->frame[ring] = v->pending_frame;
    }
    v->pending = NULL;
}
bool de__contact_get(const de__contact *s, de_view view, de__contact_info *out) {
    u32 slot = (view.id & 255u) - 1u;
    if (!s || !out || slot >= DE__VIEW_MAX || s->views[slot].view.id != view.id)
        return false;
    *out = s->views[slot].info;
    return true;
}
void de__contact_destroy(de__contact *s) {
    if (!s)
        return;
    if (s->renderer)
        de__view_unregister_stage(s->renderer, stage, s);
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        release(s, &s->views[i]);
    de__vis_kernel_destroy(s->vis, &s->kernel);
    DE_FREE(s->alloc, s, sizeof *s);
}
