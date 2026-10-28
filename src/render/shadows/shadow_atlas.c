#include "render/shadows/shadows_internal.h"
#include "render/renderer_internal.h"
#include "render/world/render_world.h"
#include "render/lighting/lighting.h"
#include "render/materials/material_instances.h"
#include "render/graph/rg_internal.h"
#include <math.h>
#include <string.h>

typedef struct atlas_light {
    u64 entity, signature, casters, dirty_since, last_served;
    de__atlas_tile tiles[6];
    u32 faces, completed, scheduled, requested_size;
    bool dynamic;
} atlas_light;
typedef struct atlas_view {
    de_view view;
    de__atlas_alloc allocator;
    de_gpu_texture depth, mirror;
    u32 depth_access, mirror_access, size, lights, rendered;
    atlas_light light[DE__ATLAS_LIGHTS];
    de_gpu_buffer metadata[DE__VIS_FRAME_RING];
    de__atlas_gpu *mapped[DE__VIS_FRAME_RING];
    de__atlas_gpu data[DE__ATLAS_RECORDS];
    u64 ring_frame[DE__VIS_FRAME_RING], frame, last_used;
    bool ring_live[DE__VIS_FRAME_RING], ready, active, initialized;
    u32 group;
    const de_rg *pending;
} atlas_view;
typedef struct atlas_group {
    atlas_view cache;
    const de__view_hidden *hidden;
    u32 mask, spent;
    de_quality quality;
    bool keyed, requested, built;
    u64 built_frame;
} atlas_group;
struct de__shadow_atlas {
    de_renderer *renderer;
    de_gpu_device *dev;
    const de_allocator *alloc;
    de__shadow_views *pool;
    de__engine_shader_code clear_code;
    de_gpu_pipeline clear_pipeline;
    de_cvar cv_size, cv_max, cv_updates, cv_pcss;
    u64 context_frame;
    u32 next_context;
    atlas_view views[DE__VIEW_MAX];
    atlas_group groups[DE__VIEW_MAX];
    u64 config_frame;
    bool configured;
    de__rw_view ranking_camera;
    bool has_ranking_camera;
};
static void release_receiver(de__shadow_atlas *s, atlas_view *v) {
    for (u32 i = 0; i < DE__VIS_FRAME_RING; ++i)
        if (v->metadata[i].id)
            de_gpu_buffer_destroy(s->dev, v->metadata[i]);
    memset(v, 0, sizeof *v);
}
static void release(de__shadow_atlas *s, atlas_view *v) {
    if (v->depth.id)
        de_gpu_texture_destroy(s->dev, v->depth);
    if (v->mirror.id)
        de_gpu_texture_destroy(s->dev, v->mirror);
    for (u32 i = 0; i < DE__VIS_FRAME_RING; ++i)
        if (v->metadata[i].id)
            de_gpu_buffer_destroy(s->dev, v->metadata[i]);
    de__atlas_alloc_destroy(&v->allocator);
    memset(v, 0, sizeof *v);
}
static void stage(de__view_frame *vf, void *user) {
    de__shadow_atlas *s = user;
    const de__vis_context *vc =
        de__vis_frame_context(de__views_vis(s->renderer), vf->view, vf->graph, vf->frame_index);
    if (!vc)
        return;
    de_result result = de__shadow_atlas_build(s, vf, vc);
    if (result != DE_OK) {
        ++vf->graph->error_count;
        DE_ERROR(s->renderer->log, "shadow atlas: %s", de_result_str(result));
    }
}
de_result de__shadow_atlas_create_ex(de_gpu_device *dev, const de_allocator *alloc,
                                     de__shadow_atlas **out) {
    if (!dev || !out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "shadow atlas create");
    *out = NULL;
    if (!alloc)
        alloc = de_heap_allocator();
    de__shadow_atlas *s = DE_NEW(alloc, de__shadow_atlas, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "shadow atlas owner");
    memset(s, 0, sizeof *s);
    s->dev = dev;
    s->alloc = alloc;
    de_result result = de__shadow_views_create(dev, alloc, &s->pool);
    if (result != DE_OK) {
        DE_FREE(alloc, s, sizeof *s);
        return result;
    }
    s->cv_pcss = de__shadow_pcss_register();
    s->cv_size = de_cvar_register(
        &(de_cvar_desc){.name = "r.shadow_atlas_size",
                        .type = DE_CVAR_INT,
                        .default_number = 4096,
                        .min = 4096,
                        .max = 8192,
                        .help = "Local shadow atlas edge; Ultra defaults to 8192"});
    s->cv_max = de_cvar_register(&(de_cvar_desc){.name = "r.shadow_local_max",
                                                 .type = DE_CVAR_INT,
                                                 .default_number = 32,
                                                 .min = 1,
                                                 .max = 32,
                                                 .help = "Maximum cached shadowed local lights"});
    s->cv_updates = de_cvar_register(&(de_cvar_desc){
        .name = "r.shadow_atlas_updates_per_frame",
        .type = DE_CVAR_INT,
        .default_number = 8,
        .min = 1,
        .max = 192,
        .help =
            "Local shadow tile renders per renderer frame; points publish only after six faces"});
    *out = s;
    return DE_OK;
}
de_result de__shadow_atlas_create(de_renderer *r, de__shadow_atlas **out) {
    DE_TRY(de__shadow_atlas_create_ex(r->dev, r->alloc, out));
    (*out)->renderer = r;
    de_result result =
        de__view_register_stage(r, DE__VIEW_SLOT_LIGHTING, -39, "shadow.atlas", stage, *out);
    if (result != DE_OK) {
        de__shadow_atlas_destroy(*out);
        *out = NULL;
    }
    return result;
}
void de__shadow_atlas_destroy(de__shadow_atlas *s) {
    if (!s)
        return;
    if (s->renderer)
        de__view_unregister_stage(s->renderer, stage, s);
    for (u32 i = 0; i < DE__VIEW_MAX; ++i) {
        release_receiver(s, &s->views[i]);
        release(s, &s->groups[i].cache);
        de__view_hidden_release(s->groups[i].hidden);
    }
    if (s->clear_pipeline.id)
        de_gpu_pipeline_destroy(s->dev, s->clear_pipeline);
    de__engine_shader_code_free(&s->clear_code);
    de__shadow_views_destroy(s->pool);
    DE_FREE(s->alloc, s, sizeof *s);
}
void de__shadow_atlas_frame_begin(de__shadow_atlas *s, u64 frame, const de__rw_view *camera) {
    if (!s)
        return;
    s->configured = true;
    s->config_frame = frame;
    s->has_ranking_camera = camera != NULL;
    if (camera)
        s->ranking_camera = *camera;
    for (u32 i = 0; i < DE__VIEW_MAX; ++i) {
        s->groups[i].requested = false;
        s->groups[i].quality = DE_QUALITY_OFF;
        s->groups[i].spent = 0;
    }
    if (s->renderer) {
        s->renderer->stats.shadow_atlas_occupied_texels = 0;
        s->renderer->stats.shadow_atlas_tiles_rendered = 0;
    }
}
de_result de__shadow_atlas_frame_view(de__shadow_atlas *s, de_view view, u32 mask,
                                      de_quality quality) {
    return de__shadow_atlas_frame_view_hidden(s, view, mask, quality, NULL);
}
de_result de__shadow_atlas_frame_view_hidden(de__shadow_atlas *s, de_view view, u32 mask,
                                             de_quality quality, const de__view_hidden *hidden) {
    u32 slot = (view.id & 255u) - 1u;
    if (!s || slot >= DE__VIEW_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "atlas receiver registration");
    atlas_view *receiver = &s->views[slot];
    if (receiver->pending)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "atlas receiver still pending");
    if (receiver->view.id != view.id) {
        release_receiver(s, receiver);
        receiver->view = view;
    }
    receiver->active = quality != DE_QUALITY_OFF;
    if (!receiver->active)
        return DE_OK;
    if (!mask)
        mask = ~0u;
    u32 found = DE__VIEW_MAX;
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        if (s->groups[i].keyed && s->groups[i].mask == mask &&
            de__view_hidden_equal(s->groups[i].hidden, hidden)) {
            found = i;
            break;
        }
    if (found == DE__VIEW_MAX) {
        for (u32 i = 0; i < DE__VIEW_MAX; ++i) {
            atlas_group *g = &s->groups[i];
            if (!g->requested && !g->cache.pending &&
                (found == DE__VIEW_MAX || !g->keyed ||
                 g->cache.last_used < s->groups[found].cache.last_used)) {
                found = i;
                if (!g->keyed)
                    break;
            }
        }
        if (found == DE__VIEW_MAX)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "atlas groups reserved by unsubmitted graphs");
        release(s, &s->groups[found].cache);
        de__view_hidden_release(s->groups[found].hidden);
        s->groups[found] =
            (atlas_group){.keyed = true, .mask = mask, .hidden = de__view_hidden_retain(hidden)};
    }
    atlas_group *group = &s->groups[found];
    group->requested = true;
    if (quality > group->quality)
        group->quality = quality;
    receiver->group = found;
    return DE_OK;
}
static u32 group_quota(const de__shadow_atlas *s, u32 group) {
    u32 count = 0, ordinal = 0;
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        if (s->groups[i].requested) {
            if (i == group)
                ordinal = count;
            ++count;
        }
    u32 budget = (u32)de_cvar_int(s->cv_updates);
    if (budget > DE__ATLAS_FACES)
        budget = DE__ATLAS_FACES;
    if (!count)
        return 0;
    u32 first = (u32)(s->config_frame % count), relative = (ordinal + count - first) % count;
    return budget / count + (relative < budget % count ? 1u : 0u);
}
static de_result clear_init(de__shadow_atlas *s) {
    if (s->clear_pipeline.id)
        return DE_OK;
    if (!s->clear_code.count) {
        const char *entries[] = {"vs_atlas_clear", "ps_atlas_clear"};
        char why[768];
        DE_TRY(de__engine_shader_load(s->dev, s->alloc, "shadow_atlas", 0, entries, 2,
                                      &s->clear_code, why, sizeof why));
    }
    s->clear_pipeline =
        de_gpu_pipeline_create(s->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_GRAPHICS,
                                                               .vs = s->clear_code.entries[0].blob,
                                                               .ps = s->clear_code.entries[1].blob,
                                                               .depth_format = DE_FMT_D32_FLOAT,
                                                               .depth_write = true,
                                                               .depth_test = false,
                                                               .cull = DE_CULL_NONE,
                                                               .name = "shadow.atlas_tile_clear"});
    return s->clear_pipeline.id ? DE_OK : DE_FAIL(DE_ERR_INTERNAL, "shadow atlas clear pipeline");
}
typedef struct atlas_clear {
    de_rg_resource depth;
    de_gpu_pipeline pipeline;
    de__atlas_tile tile;
} atlas_clear;
/* First allocation (or an uncertain failed graph) has no confirmed cached pixels. Define the
 * whole image before eager fallback sampling, including groups with no light-tile quota yet. */
static void initialize_depth(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const atlas_clear *p = user;
    const de_gpu_attachment attachment = {
        .texture = de_rg_texture(pass, p->depth), .clear = true, .clear_depth = 0, .store = true};
    de_gpu_cmd_begin_render(cmd, NULL, 0, &attachment);
    de_gpu_cmd_end_render(cmd);
}
static void clear_tile(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const atlas_clear *p = user;
    const de_gpu_attachment attachment = {.texture = de_rg_texture(pass, p->depth), .store = true};
    de_gpu_cmd_begin_render(cmd, NULL, 0, &attachment);
    de_gpu_cmd_set_pipeline(cmd, p->pipeline);
    de_gpu_cmd_scissor(cmd, p->tile.x, p->tile.y, p->tile.size, p->tile.size);
    de_gpu_cmd_draw(cmd, 3, 1, 0, 0);
    de_gpu_cmd_end_render(cmd);
}
static de_result storage(de__shadow_atlas *s, atlas_view *v, de_view view, u32 size) {
    if (v->view.id == view.id && v->size == size && v->depth.id && v->mirror.id)
        return DE_OK;
    release(s, v);
    v->view = view;
    v->size = size;
    DE_TRY(de__atlas_alloc_init(&v->allocator, s->alloc, size));
    v->depth = de_gpu_texture_create(
        s->dev, &(de_gpu_texture_desc){.format = DE_FMT_D32_FLOAT,
                                       .width = size,
                                       .height = size,
                                       .usage = DE_USAGE_DEPTH | DE_USAGE_COPY_SRC,
                                       .name = "shadow.atlas_depth"});
    v->mirror = de_gpu_texture_create(
        s->dev,
        &(de_gpu_texture_desc){.format = DE_FMT_R32_FLOAT,
                               .width = size,
                               .height = size,
                               .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC,
                               .name = "shadow.atlas_mirror"});
    if (!v->depth.id || !v->mirror.id) {
        release(s, v);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "shadow atlas images");
    }
    return DE_OK;
}
static u64 hash(u64 h, const void *data, size_t size) {
    const u8 *p = data;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}
f32 de__atlas_light_range(const DeLight *l, f32 cutoff) {
    if (!l)
        return 0;
    if (l->range > 0)
        return l->range;
    f32 c = fmaxf(l->color.x, fmaxf(l->color.y, l->color.z)) * l->intensity;
    if (l->kind == DE_LIGHT_POINT)
        c = de__light_point_candela(c);
    else if (l->kind == DE_LIGHT_SPOT)
        c = de__light_spot_candela(c, l->spot_outer);
    else
        c *= l->kind == DE_LIGHT_DISK ? l->area_size.x * l->area_size.x * .785398163f
                                      : l->area_size.x * l->area_size.y;
    return de__light_derived_range(c, cutoff);
}
static f32 range_of(const DeLight *l) {
    de_cvar cv = de_cvar_find("r.light_cutoff_lux");
    f32 cutoff = DE_HANDLE_VALID(cv) ? (f32)de_cvar_float(cv) : .05f;
    return de__atlas_light_range(l, cutoff);
}
static bool touches(de_aabb bounds, de_vec3 p, f32 range) {
    f32 d = 0;
    const f32 c[3] = {p.x, p.y, p.z}, lo[3] = {bounds.min.x, bounds.min.y, bounds.min.z},
              hi[3] = {bounds.max.x, bounds.max.y, bounds.max.z};
    for (u32 i = 0; i < 3; ++i) {
        f32 q = fmaxf(lo[i] - c[i], fmaxf(c[i] - hi[i], 0));
        d += q * q;
    }
    return d <= range * range;
}
/* Recomputed only on render-world change lists or GPU-scene mesh/material changes. Comparing
 * previous/current influence fingerprints retains removed casters' old influence implicitly. */
static u64 caster_signature(const de__view_frame *vf, const de__vis_context *vc, de_vec3 p,
                            f32 range) {
    u64 h = 1469598103934665603ull;
    const de__rw_instances *w = &vf->world->instances;
    for (u32 i = 0; i < w->count; ++i) {
        if (!w->entity[i] || de__view_hidden_contains(vf->hidden, w->entity[i]) ||
            !(w->flags[i] & DE__RW_INST_CAST_SHADOWS) ||
            !(w->layer_mask[i] & (vc->layer_mask ? vc->layer_mask : ~0u)) ||
            !touches(w->bounds[i], p, range))
            continue;
        h = hash(h, &w->entity[i], sizeof w->entity[i]);
        h = hash(h, &w->transform[i], sizeof w->transform[i]);
        h = hash(h, &w->bounds[i], sizeof w->bounds[i]);
        h = hash(h, &w->flags[i], sizeof w->flags[i]);
        h = hash(h, &w->layer_mask[i], sizeof w->layer_mask[i]);
        /* Alpha texture content can change without changing its material's bindless number.
         * Conservatively invalidate intersecting caster influences on the pool reload epoch. */
        if (vf->renderer) {
            u64 epoch = de__material_instances_epoch(vf->renderer->material_instances);
            h = hash(h, &epoch, sizeof epoch);
        }
        u32 mesh;
        const u32 *materials;
        if (de__gpu_scene_instance_slots(vc->gpu_scene, i, &mesh, &materials)) {
            const void *entry = de__gpu_scene_entry(vc->gpu_scene, DE__GS_MESHES, mesh);
            if (entry)
                h = hash(h, entry, sizeof(de__gs_mesh));
            for (u32 j = 0; j < DE__RW_MATERIALS; ++j) {
                entry = de__gpu_scene_entry(vc->gpu_scene, DE__GS_MATERIALS, materials[j]);
                if (entry)
                    h = hash(h, entry, sizeof(de__gs_material));
            }
        }
    }
    return h;
}
static de_mat4 light_view(de_vec3 p, de_vec3 forward, de_vec3 up) {
    de_vec3 z = de_v3_scale(forward, -1), x = de_v3_normalize(de_v3_cross(forward, up)),
            y = de_v3_cross(z, x);
    de_mat4 m = de_m4_identity();
    m.m[0] = x.x;
    m.m[4] = x.y;
    m.m[8] = x.z;
    m.m[12] = -de_v3_dot(x, p);
    m.m[1] = y.x;
    m.m[5] = y.y;
    m.m[9] = y.z;
    m.m[13] = -de_v3_dot(y, p);
    m.m[2] = z.x;
    m.m[6] = z.y;
    m.m[10] = z.z;
    m.m[14] = -de_v3_dot(z, p);
    return m;
}
typedef struct ranked_light {
    u32 slot;
    u64 entity;
    f64 importance;
} ranked_light;
static bool earlier(ranked_light a, ranked_light b) {
    return a.importance > b.importance || (a.importance == b.importance && a.entity < b.entity);
}

/* All receiver geometry in the mask group is a conservative superset of every member
 * camera's visible receivers (including captures and cameras outside a light's sphere).
 * Do not apply this test to casters: off-camera occluders must still render into the map.
 * Unknown bounds/custom forward geometry retain the light. No snapshot writes or allocation.
 * Spot/area lights deliberately use their containing range sphere, never a narrow cone test. */
typedef struct receiver_bounds {
    de_aabb union_bounds;
    bool any, unknown;
} receiver_bounds;
static receiver_bounds receiver_union(const de__rw_instances *w, u32 mask,
                                      const de__view_hidden *hidden) {
    receiver_bounds result = {0};
    if (w->count && (!w->entity || !w->layer_mask || !w->flags || !w->bounds)) {
        result.unknown = true;
        return result;
    }
    for (u32 i = 0; i < w->count; ++i) {
        if (!w->entity[i] || de__view_hidden_contains(hidden, w->entity[i]) ||
            !(w->layer_mask[i] & mask))
            continue;
        if (!(w->flags[i] & DE__RW_INST_HAS_BOUNDS)) {
            result.unknown = true;
            return result;
        }
        const de_aabb b = w->bounds[i];
        if (!isfinite(b.min.x) || !isfinite(b.min.y) || !isfinite(b.min.z) || !isfinite(b.max.x) ||
            !isfinite(b.max.y) || !isfinite(b.max.z) || b.min.x > b.max.x || b.min.y > b.max.y ||
            b.min.z > b.max.z) {
            result.unknown = true;
            return result;
        }
        if (!result.any) {
            result.union_bounds = b;
            result.any = true;
        } else {
            de_aabb *u = &result.union_bounds;
            u->min.x = fminf(u->min.x, b.min.x);
            u->min.y = fminf(u->min.y, b.min.y);
            u->min.z = fminf(u->min.z, b.min.z);
            u->max.x = fmaxf(u->max.x, b.max.x);
            u->max.y = fmaxf(u->max.y, b.max.y);
            u->max.z = fmaxf(u->max.z, b.max.z);
        }
    }
    return result;
}
/* Match light_cull.slang: area influence includes the emitter's containing radius.
 * Invalid dimensions cannot justify rejection; retaining is conservative. */
static f32 receiver_sphere_range(const DeLight *l, f32 range) {
    f32 extent = 0;
    if (l->kind == DE_LIGHT_RECT) {
        if (!isfinite(l->area_size.x) || !isfinite(l->area_size.y))
            return INFINITY;
        extent = hypotf(.5f * l->area_size.x, .5f * l->area_size.y);
    } else if (l->kind == DE_LIGHT_DISK) {
        if (!isfinite(l->area_size.x) || l->area_size.x < 0)
            return INFINITY;
        extent = .5f * l->area_size.x;
    }
    return range + extent;
}
static bool receiver_touches(de_aabb b, de_vec3 p, f32 range) {
    const f32 lo[3] = {b.min.x, b.min.y, b.min.z}, hi[3] = {b.max.x, b.max.y, b.max.z},
              c[3] = {p.x, p.y, p.z};
    f64 distance2 = 0;
    for (u32 axis = 0; axis < 3; ++axis) {
        const f64 d = fmax((f64)lo[axis] - c[axis], fmax((f64)c[axis] - hi[axis], 0));
        distance2 += d * d;
    }
    /* Inclusive with the same relative guard for broad and exact receiver bounds. */
    return distance2 <= (f64)range * range * (1.0 + 1e-6);
}
static bool receiver_influence(const de__rw_instances *w, u32 mask, de_vec3 p, f32 range,
                               const de__view_hidden *hidden) {
    for (u32 i = 0; i < w->count; ++i)
        if (w->entity[i] && !de__view_hidden_contains(hidden, w->entity[i]) &&
            (w->layer_mask[i] & mask) && receiver_touches(w->bounds[i], p, range))
            return true;
    return false;
}
/* Pure selection is shared by the production atlas and CPU regression fixtures. */
static u32 select_lights(const de__render_world_frame *frame, const de__rw_view *camera, u32 mask,
                         bool unknown_receivers, u32 limit, const de__view_hidden *hidden,
                         u32 out[DE__ATLAS_LIGHTS]) {
    if (!frame || !camera || !out || !limit)
        return 0;
    mask = mask ? mask : ~0u;
    receiver_bounds receivers = {0};
    bool receivers_checked = unknown_receivers;
    ranked_light selected[DE__ATLAS_LIGHTS];
    u32 count = 0;
    if (limit > DE__ATLAS_LIGHTS)
        limit = DE__ATLAS_LIGHTS;
    const de__rw_lights *world = &frame->lights;
    for (u32 i = 0; i < world->count; ++i) {
        const DeLight *l = &world->light[i];
        if (!world->entity[i] || de__view_hidden_contains(hidden, world->entity[i]) ||
            l->kind == DE_LIGHT_DIRECTIONAL || !l->cast_shadows || !(l->intensity > 0) ||
            !isfinite(l->intensity))
            continue;
        de_vec3 p = {world->transform[i].m[9], world->transform[i].m[10],
                     world->transform[i].m[11]};
        f32 range = range_of(l), d = de_v3_length(de_v3_sub(p, camera->offset));
        if (!isfinite(range) || !(range > 0))
            continue;
        f64 projected = (f64)range * range;
        if (camera->camera.projection != DE_PROJ_ORTHOGRAPHIC)
            projected /= fmaxf(d * d, .01f);
        ranked_light item = {i, world->entity[i], l->intensity * projected};
        u32 insert = count;
        while (insert && earlier(item, selected[insert - 1]))
            --insert;
        if (insert >= limit)
            continue;
        const f32 receiver_range = receiver_sphere_range(l, range);
        /* No receiver walk when the view has no eligible local shadow lights. */
        if (!receivers_checked) {
            receivers = receiver_union(&frame->instances, mask, hidden);
            unknown_receivers = receivers.unknown;
            receivers_checked = true;
            if (!unknown_receivers && !receivers.any)
                return 0;
        }
        if (!unknown_receivers &&
            (!receiver_touches(receivers.union_bounds, p, receiver_range) ||
             !receiver_influence(&frame->instances, mask, p, receiver_range, hidden)))
            continue;
        if (count < limit)
            ++count;
        for (u32 j = count - 1; j > insert; --j)
            selected[j] = selected[j - 1];
        selected[insert] = item;
    }
    for (u32 i = 0; i < count; ++i)
        out[i] = selected[i].slot;
    return count;
}

u32 de__atlas_select_lights(const de__render_world_frame *frame, const de__rw_view *camera,
                            u32 mask, bool unknown_receivers, u32 limit,
                            u32 out[DE__ATLAS_LIGHTS]) {
    return select_lights(frame, camera, mask, unknown_receivers, limit, NULL, out);
}

static de_result cache_build(de__shadow_atlas *s, const de__view_frame *vf,
                             const de__vis_context *vc, atlas_group *group) {
    atlas_view *v = &group->cache;
    v->rendered = 0;
    u32 selected[DE__ATLAS_LIGHTS];
    const u32 count = select_lights(vf->world, vf->camera, group->mask,
                                    vf->renderer && vf->renderer->forward_participant_count != 0,
                                    (u32)de_cvar_int(s->cv_max), vf->hidden, selected);
    const de__rw_lights *world = &vf->world->lights;
    if (!count) {
        for (u32 i = 0; i < v->lights; ++i)
            for (u32 f = 0; f < v->light[i].faces; ++f)
                de__atlas_alloc_free(&v->allocator, v->light[i].tiles[f]);
        memset(v->light, 0, sizeof v->light);
        memset(v->data, 0, sizeof v->data);
        v->lights = 0;
        v->ready = false;
        return DE_OK;
    }
    u32 size = (u32)de_cvar_int(s->cv_size);
    size = size >= 8192 || vf->settings->shadows == DE_QUALITY_ULTRA ? 8192 : 4096;
    DE_TRY(storage(s, v, (de_view){.id = (u32)(group - s->groups) + 1u}, size));
    DE_TRY(clear_init(s));
    const bool gap = v->last_used + 1u != vf->frame_index;
    // Ordinary caches are lazy and retained within 512MiB; retirement/scratch are independent.
    v->last_used = vf->frame_index;
    for (;;) {
        u64 bytes = 0;
        u32 oldest = DE__VIEW_MAX;
        for (u32 i = 0; i < DE__VIEW_MAX; ++i) {
            atlas_group *other = &s->groups[i];
            atlas_view *o = &other->cache;
            bytes += (u64)o->size * o->size * 8u;
            if (!other->requested && o->size && !o->pending &&
                o->last_used + 60u < vf->frame_index &&
                (oldest == DE__VIEW_MAX || o->last_used < s->groups[oldest].cache.last_used))
                oldest = i;
        }
        if (bytes <= 512ull * 1024 * 1024 || oldest == DE__VIEW_MAX)
            break;
        release(s, &s->groups[oldest].cache);
        s->groups[oldest].built = false;
    }
    v->pending = vf->graph;
    v->frame = vf->frame_index;
    if (s->context_frame != vf->frame_index) {
        s->context_frame = vf->frame_index;
        s->next_context = 0;
    }
    u32 faces = 0;
    for (u32 i = 0; i < count; ++i)
        faces += world->light[selected[i]].kind == DE_LIGHT_POINT ? 6 : 1;
    u32 base = 16;
    while (base < 2048 && (u64)(base * 2) * (base * 2) * faces * 2 <= ((u64)size * size))
        base *= 2;
    u32 cap = vf->settings->shadows == DE_QUALITY_LOW      ? 128
              : vf->settings->shadows == DE_QUALITY_MEDIUM ? 256
              : vf->settings->shadows == DE_QUALITY_HIGH   ? 512
                                                           : 2048;
    atlas_light old[DE__ATLAS_LIGHTS];
    memcpy(old, v->light, sizeof old);
    memset(v->light, 0, sizeof v->light);
    bool used[DE__ATLAS_LIGHTS] = {0};
    for (u32 i = 0; i < count; ++i)
        for (u32 j = 0; j < v->lights; ++j)
            if (old[j].entity == world->entity[selected[i]]) {
                v->light[i] = old[j];
                used[j] = true;
                break;
            }
    for (u32 j = 0; j < v->lights; ++j)
        if (!used[j])
            for (u32 f = 0; f < old[j].faces; ++f)
                de__atlas_alloc_free(&v->allocator, old[j].tiles[f]);
    const de__gpu_scene_stats *stats = de__gpu_scene_stats_get(vc->gpu_scene);
    bool changed = gap || vf->world->dirty_instance_count || vf->world->removed_instance_count ||
                   vf->world->origin_changed ||
                   (stats && (stats->material_packs || stats->frame_entries[DE__GS_MESHES]));
    memset(v->data, 0, sizeof v->data);
    v->data[0].identity[0] = count;
    de_rg_resource depth = de_rg_import_texture(vf->graph, v->depth, v->depth_access),
                   mirror = de_rg_import_texture(vf->graph, v->mirror, v->mirror_access);
    if (!v->initialized) {
        atlas_clear *initialize = DE_ARENA_PUSH(vf->graph->arena, atlas_clear, 1);
        if (!initialize)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "shadow atlas initialization");
        *initialize = (atlas_clear){.depth = depth};
        de_rg_pass *pass = de_rg_add_pass(vf->graph, "shadow.atlas_initialize", DE_QUEUE_GRAPHICS,
                                          initialize_depth, initialize);
        de_rg_write(pass, depth, DE_ACCESS_DEPTH_WRITE);
    }
    de_mat4 views[DE__ATLAS_FACES], projs[DE__ATLAS_FACES];
    for (u32 i = 0; i < count; ++i) {
        atlas_light *cache = &v->light[i];
        u32 index = selected[i];
        const DeLight *l = &world->light[index];
        const de_affine *xf = &world->transform[index];
        de_vec3 p = {xf->m[9], xf->m[10], xf->m[11]};
        f32 range = fmaxf(range_of(l), .02f), near_z = fminf(.1f, range * .01f);
        u32 n = l->kind == DE_LIGHT_POINT ? 6 : 1, tile_size = base * (i < count / 4 ? 2u : 1u);
        if (tile_size > cap)
            tile_size = cap;
        bool moved =
            cache->faces != n || !cache->tiles[0].size || cache->requested_size != tile_size;
        if (moved) {
            for (u32 f = 0; f < cache->faces; ++f)
                de__atlas_alloc_free(&v->allocator, cache->tiles[f]);
            memset(cache->tiles, 0, sizeof cache->tiles);
            cache->faces = n;
            cache->requested_size = tile_size;
            for (;;) {
                u32 allocated = 0;
                for (; allocated < n; ++allocated)
                    if (!de__atlas_alloc_take(&v->allocator, tile_size, &cache->tiles[allocated]))
                        break;
                if (allocated == n)
                    break;
                for (u32 f = 0; f < allocated; ++f)
                    de__atlas_alloc_free(&v->allocator, cache->tiles[f]);
                if (tile_size == 16)
                    return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "shadow atlas tiles");
                tile_size /= 2;
            }
        }
        u64 signature = hash(1469598103934665603ull, xf, sizeof *xf);
        signature = hash(signature, &vf->world->origin, sizeof vf->world->origin);
        signature = hash(signature, &l->kind, sizeof l->kind);
        signature = hash(signature, &range, sizeof range);
        signature = hash(signature, &l->spot_outer, sizeof l->spot_outer);
        signature = hash(signature, &group->mask, sizeof group->mask);
        u64 casters = changed || moved || !cache->entity || cache->signature != signature
                          ? caster_signature(vf, vc, p, range)
                          : cache->casters;
        if (moved || cache->signature != signature || cache->casters != casters ||
            cache->entity != world->entity[selected[i]] || cache->dynamic) {
            if (!cache->entity || cache->completed == ((1u << cache->faces) - 1u))
                cache->dirty_since = vf->frame_index + 1u;
            cache->completed = 0;
        }
        cache->entity = world->entity[selected[i]];
        cache->signature = signature;
        cache->casters = casters;
        cache->scheduled = 0;
        const de_vec3 dirs[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                 {0, -1, 0}, {0, 0, 1},  {0, 0, -1}},
                      ups[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1},
                                {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
        for (u32 f = 0; f < n; ++f) {
            de_vec3 direction =
                n == 6 ? dirs[f]
                       : de_v3_normalize_safe((de_vec3){-xf->m[6], -xf->m[7], -xf->m[8]},
                                              (de_vec3){0, 0, -1});
            de_vec3 up =
                n == 6 ? ups[f]
                       : (fabsf(direction.z) < .99f ? (de_vec3){0, 0, 1} : (de_vec3){0, 1, 0});
            f32 angle = n == 6 ? 1.570796327f
                        : l->kind == DE_LIGHT_SPOT
                            ? fminf(fmaxf(l->spot_outer * 2, .01f), 3.131592654f)
                            : 2.617993878f;
            tile_size = cache->tiles[f].size;
            f32 tangent = tanf(angle * .5f) * (f32)tile_size / (f32)(tile_size - 4u);
            views[i * 6 + f] = light_view(p, direction, up);
            projs[i * 6 + f] = de_m4_perspective_rz(2 * atanf(tangent), 1, near_z);
            projs[i * 6 + f].m[10] = near_z / (range - near_z);
            projs[i * 6 + f].m[14] = near_z * range / (range - near_z);
            de_mat4 matrix = de_m4_mul(&projs[i * 6 + f], &views[i * 6 + f]);
            de__atlas_gpu *entry = &v->data[1 + i * 6 + f];
            for (u32 r = 0; r < 4; ++r)
                for (u32 c = 0; c < 4; ++c)
                    entry->matrix[r][c] = matrix.m[c * 4 + r];
            memcpy(entry->position, &p, sizeof p);
            entry->position[3] = range;
            entry->projection[0] = near_z;
            entry->projection[1] = range;
            entry->projection[2] = tangent;
            entry->projection[3] = l->source_radius;
            de__atlas_tile tile = cache->tiles[f];
            entry->tile[0] = (f32)tile.x / size;
            entry->tile[1] = (f32)tile.y / size;
            entry->tile[2] = entry->tile[3] = (f32)tile.size / size;
            entry->identity[0] = (u32)cache->entity;
            entry->identity[1] = (u32)(cache->entity >> 32);
            entry->identity[2] = n;
            u32 taps = vf->settings->shadows == DE_QUALITY_LOW      ? 4
                       : vf->settings->shadows == DE_QUALITY_MEDIUM ? 8
                       : vf->settings->shadows == DE_QUALITY_HIGH   ? 16
                                                                    : 32;
            entry->map = (de__shadow_map_ref){
                .texture = de_gpu_texture_bindless(s->dev, v->mirror, 0, false),
                .taps = taps,
                .resolution = size,
                .uv_clamp = {((f32)tile.x + 2.5f) / size, ((f32)tile.y + 2.5f) / size,
                             ((f32)(tile.x + tile.size) - 2.5f) / size,
                             ((f32)(tile.y + tile.size) - 2.5f) / size},
                .depth_projection = {near_z, range, tangent, l->source_radius},
                .uv_projection = {entry->tile[2] / (2.f * tangent),
                                  entry->tile[3] / (2.f * tangent), (f32)de_cvar_float(s->cv_pcss),
                                  0.f},
                .filter_state = {2u, (u32)vf->settings->shadows, (u32)vf->frame_index,
                                 taps < 16u ? 16u : taps}};
        }
    }
    v->lights = count;
    u32 quota = group_quota(s, (u32)(group - s->groups));
    u32 budget = group->spent < quota ? quota - group->spent : 0;
    while (v->rendered < budget) {
        u32 choice = count;
        for (u32 i = 0; i < count; ++i) {
            atlas_light *a = &v->light[i];
            if ((a->completed | a->scheduled) == ((1u << a->faces) - 1u))
                continue;
            if (choice == count || a->dirty_since < v->light[choice].dirty_since ||
                (a->dirty_since == v->light[choice].dirty_since &&
                 a->last_served < v->light[choice].last_served))
                choice = i;
        }
        if (choice == count)
            break;
        atlas_light *a = &v->light[choice];
        u32 face = 0;
        while ((a->completed | a->scheduled) & (1u << face))
            ++face;
        atlas_clear *clear = DE_ARENA_PUSH(vf->graph->arena, atlas_clear, 1);
        if (!clear)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "shadow atlas tile clear");
        *clear = (atlas_clear){depth, s->clear_pipeline, a->tiles[face]};
        de_rg_pass *pass =
            de_rg_add_pass(vf->graph, "shadow.atlas_clear", DE_QUEUE_GRAPHICS, clear_tile, clear);
        de_rg_read(pass, depth, DE_ACCESS_DEPTH_READ);
        de_rg_write(pass, depth, DE_ACCESS_DEPTH_READ | DE_ACCESS_DEPTH_WRITE);
        de__atlas_tile tile = a->tiles[face];
        DE_TRY(de__shadow_view_build(
            s->pool, vf->graph,
            &(de__shadow_view_desc){.scene = vc->gpu_scene,
                                    .hidden = vf->hidden,
                                    .hidden_buffer = vf->hidden_buffer,
                                    .depth = depth,
                                    .context = s->next_context++,
                                    .rect = {tile.x, tile.y, tile.size, tile.size},
                                    .width = size,
                                    .height = size,
                                    .layer_mask = vc->layer_mask,
                                    .view = views[choice * 6 + face],
                                    .proj = projs[choice * 6 + face],
                                    .camera = {v->data[1 + choice * 6].position[0],
                                               v->data[1 + choice * 6].position[1],
                                               v->data[1 + choice * 6].position[2]},
                                    .origin = vf->world->origin,
                                    .frame = vf->frame_index,
                                    .frames_completed = vc->frames_completed},
            NULL));
        a->scheduled |= 1u << face;
        a->last_served = vf->frame_index + 1u;
        ++v->rendered;
    }
    if (v->rendered || !v->initialized)
        DE_TRY(de__shadow_copy_layer(s->pool, vf->graph, depth, 0, mirror, size, size));
    for (u32 i = 0; i < count; ++i)
        for (u32 f = 0; f < v->light[i].faces; ++f)
            v->data[1 + i * 6 + f].identity[3] =
                (v->light[i].completed | v->light[i].scheduled) == ((1u << v->light[i].faces) - 1u);
    de_rg_export(vf->graph, depth, DE_ACCESS_COPY_SRC);
    de_rg_export(vf->graph, mirror, DE_ACCESS_SHADER_READ);
    return DE_OK;
}
de_result de__shadow_atlas_build(de__shadow_atlas *s, const de__view_frame *vf,
                                 const de__vis_context *vc) {
    if (!s || !vf || !vc || !vf->settings)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "shadow atlas build");
    u32 slot = (vf->view.id & 255u) - 1u;
    if (slot >= DE__VIEW_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "shadow atlas receiver");
    atlas_view *receiver = &s->views[slot];
    if (receiver->pending)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "shadow atlas receiver pending");
    receiver->rendered = 0;
    if (!s->configured || s->config_frame != vf->frame_index)
        de__shadow_atlas_frame_begin(s, vf->frame_index, vf->camera);
    DE_TRY(de__shadow_atlas_frame_view_hidden(s, vf->view, vc->layer_mask, vf->settings->shadows,
                                              vf->hidden));
    if (!receiver->active || !vf->world || !vf->camera) {
        receiver->active = false;
        return DE_OK;
    }
    atlas_group *group = &s->groups[receiver->group];
    atlas_view *cache = &group->cache;
    if (cache->pending)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "shadow atlas group pending");
    const u32 ring = (u32)(vf->frame_index % DE__VIS_FRAME_RING);
    if (receiver->ring_live[ring] && receiver->ring_frame[ring] >= vc->frames_completed)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "shadow atlas metadata in flight");
    if (!receiver->metadata[ring].id) {
        receiver->metadata[ring] =
            de_gpu_buffer_create(s->dev, &(de_gpu_buffer_desc){.size = sizeof receiver->data,
                                                               .stride = sizeof(de__atlas_gpu),
                                                               .usage = DE_USAGE_STORAGE,
                                                               .memory = DE_MEM_UPLOAD,
                                                               .name = "shadow.atlas_metadata"});
        if (!receiver->metadata[ring].id ||
            !(receiver->mapped[ring] = de_gpu_buffer_map(s->dev, receiver->metadata[ring]))) {
            if (receiver->metadata[ring].id)
                de_gpu_buffer_destroy(s->dev, receiver->metadata[ring]);
            receiver->metadata[ring] = (de_gpu_buffer){0};
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "shadow atlas metadata");
        }
    }
    receiver->pending = vf->graph;
    receiver->frame = vf->frame_index;
    receiver->ready = false;
    const bool reuse = group->built && group->built_frame == vf->frame_index;
    if (!reuse) {
        de_render_settings settings = *vf->settings;
        settings.shadows = group->quality;
        de__view_frame producer = *vf;
        producer.settings = &settings;
        if (s->has_ranking_camera)
            producer.camera = &s->ranking_camera;
        DE_TRY(cache_build(s, &producer, vc, group));
    }
    receiver->size = cache->size;
    receiver->lights = cache->lights;
    receiver->depth = cache->depth;
    receiver->mirror = cache->mirror;
    receiver->depth_access = cache->depth_access;
    receiver->mirror_access = cache->mirror_access;
    receiver->allocator.occupied = cache->allocator.occupied;
    receiver->rendered = reuse ? 0 : cache->rendered;
    memcpy(receiver->data, cache->data, sizeof receiver->data);
    u32 taps = vf->settings->shadows == DE_QUALITY_LOW      ? 4
               : vf->settings->shadows == DE_QUALITY_MEDIUM ? 8
               : vf->settings->shadows == DE_QUALITY_HIGH   ? 16
                                                            : 32;
    for (u32 i = 1; i < DE__ATLAS_RECORDS; ++i) {
        receiver->data[i].map.taps = taps;
        receiver->data[i].map.filter_state[1] = (u32)vf->settings->shadows;
        receiver->data[i].map.filter_state[2] = (u32)vf->frame_index;
        receiver->data[i].map.filter_state[3] = taps < 16u ? 16u : taps;
        receiver->data[i].map.uv_projection[2] = (f32)de_cvar_float(s->cv_pcss);
    }
    memcpy(receiver->mapped[ring], receiver->data, sizeof receiver->data);
    if (!cache->lights) {
        receiver->active = false;
        group->built = true;
        group->built_frame = vf->frame_index;
    }
    if (vf->renderer && receiver->active)
        de__shadow_atlas_publish(s, vf->view, vf->graph, vf->frame_index,
                                 de__lighting_view_inputs(de__views_lighting(vf->renderer), slot));
    return DE_OK;
}
void de__shadow_atlas_complete(de__shadow_atlas *s, de_view view, const de_rg *graph,
                               bool attempted, bool success) {
    if (!s)
        return;
    de__shadow_views_end(s->pool, graph, attempted);
    u32 slot = (view.id & 255u) - 1u;
    if (slot >= DE__VIEW_MAX)
        return;
    atlas_view *v = &s->views[slot];
    if (v->pending != graph)
        return;
    v->pending = NULL;
    v->ready = success;
    if (attempted) {
        u32 ring = (u32)(v->frame % DE__VIS_FRAME_RING);
        v->ring_live[ring] = true;
        v->ring_frame[ring] = v->frame;
    }
    atlas_group *group = &s->groups[v->group];
    atlas_view *cache = &group->cache;
    if (cache->pending != graph)
        return;
    cache->pending = NULL;
    if (attempted)
        group->spent += cache->rendered;
    for (u32 i = 0; i < cache->lights; ++i) {
        cache->light[i].completed =
            success ? cache->light[i].completed | cache->light[i].scheduled : 0;
        cache->light[i].scheduled = 0;
    }
    cache->initialized = success;
    if (!success) {
        /* A failed build may have allocated new entries beyond the previous light count.
         * Discard the complete candidate allocator/cache rather than leak or publish a subset. */
        if (cache->allocator.nodes)
            memset(cache->allocator.nodes, 0, cache->allocator.node_count);
        cache->allocator.occupied = 0;
        memset(cache->light, 0, sizeof cache->light);
        cache->lights = 0;
        memset(cache->data, 0, sizeof cache->data);
        v->allocator.occupied = 0;
        v->lights = 0;
        v->rendered = 0;
        memset(v->data, 0, sizeof v->data);
        group->built = false;
    } else {
        group->built = true;
        group->built_frame = v->frame;
        if (s->renderer)
            s->renderer->stats.shadow_atlas_tiles_rendered += cache->rendered;
    }
    cache->depth_access = success ? DE_ACCESS_COPY_SRC : DE_ACCESS_NONE;
    cache->mirror_access = success ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE;
    if (s->renderer) {
        u32 occupied = 0;
        for (u32 i = 0; i < DE__VIEW_MAX; ++i)
            if (s->groups[i].requested)
                occupied += s->groups[i].cache.allocator.occupied;
        s->renderer->stats.shadow_atlas_occupied_texels = occupied;
    }
}
void de__shadow_atlas_publish(de__shadow_atlas *s, de_view view, de_rg *g, u64 frame,
                              de__lighting_inputs *in) {
    u32 slot = (view.id & 255u) - 1u;
    if (!s || !in || slot >= DE__VIEW_MAX)
        return;
    atlas_view *v = &s->views[slot];
    if (v->pending != g || v->view.id != view.id || !v->active)
        return;
    in->atlas_records =
        de_rg_import_buffer(g, v->metadata[frame % DE__VIS_FRAME_RING], DE_ACCESS_SHADER_READ);
    in->atlas_texture = de_rg_import_texture(g, v->mirror, v->mirror_access);
}
bool de__shadow_atlas_get(const de__shadow_atlas *s, de_view view, de__atlas_info *out) {
    u32 slot = (view.id & 255u) - 1u;
    if (!s || !out || slot >= DE__VIEW_MAX)
        return false;
    const atlas_view *v = &s->views[slot];
    *out = (de__atlas_info){.size = v->size,
                            .lights = v->lights,
                            .rendered = v->rendered,
                            .occupied = v->allocator.occupied,
                            .capacity = de__shadow_views_capacity(s->pool),
                            .ready = v->ready,
                            .depth = v->depth,
                            .mirror = v->mirror,
                            .records = v->metadata[v->frame % DE__VIS_FRAME_RING]};
    memcpy(out->data, v->data, sizeof out->data);
    return true;
}
bool de__shadow_atlas_dynamic(de__shadow_atlas *s, de_view view, u64 entity, bool dynamic) {
    u32 slot = (view.id & 255u) - 1u;
    if (!s || slot >= DE__VIEW_MAX)
        return false;
    atlas_view *receiver = &s->views[slot];
    if (receiver->view.id != view.id || receiver->pending)
        return false;
    atlas_view *v = &s->groups[receiver->group].cache;
    if (v->pending)
        return false;
    for (u32 i = 0; i < v->lights; ++i)
        if (v->light[i].entity == entity) {
            v->light[i].dynamic = dynamic;
            return true;
        }
    return false;
}
bool de__shadow_atlas_capture_ready(const de__shadow_atlas *s, de_view view) {
    u32 slot = (view.id & 255u) - 1u;
    if (!s || slot >= DE__VIEW_MAX)
        return true;
    const atlas_view *v = &s->views[slot];
    if (v->view.id != view.id || !v->active)
        return true;
    if (v->pending || !v->ready || v->frame != s->config_frame)
        return false;
    for (u32 i = 0; i < v->lights; ++i)
        if (!v->data[1 + i * 6].identity[3])
            return false;
    return true;
}
