#include "render/shadows/shadows_internal.h"
#include <math.h>
#include <string.h>

void de__csm_quality(de_quality q, u32 *count, u32 *resolution, u32 *taps) {
    static const u32 n[5] = {0, 2, 3, 4, 4}, r[5] = {0, 1024, 2048, 2048, 4096},
                     t[5] = {0, 4, 8, 16, 32};
    u32 i = (u32)q;
    if (i > 4)
        i = 4;
    if (count)
        *count = n[i];
    if (resolution)
        *resolution = r[i];
    if (taps)
        *taps = t[i];
}
void de__csm_splits(f32 near_z, f32 far_z, u32 count, f32 out[5]) {
    memset(out, 0, 5 * sizeof *out);
    if (count < 1 || count > 4 || !(near_z > 0) || !(far_z > near_z))
        return;
    out[0] = near_z;
    for (u32 i = 1; i < count; ++i) {
        f32 t = (f32)i / (f32)count;
        out[i] = .75f * near_z * powf(far_z / near_z, t) + .25f * (near_z + (far_z - near_z) * t);
    }
    out[count] = far_z;
}
static f32 snap(f32 relative, f64 origin, f32 texel) {
    /* Keep the whole-world grid phase in double, then snap only small relative floats. */
    f64 phase = remainder(origin, (f64)texel);
    return (f32)(floor(((f64)relative + phase) / (f64)texel + .5) * (f64)texel - phase);
}
de_result de__csm_fit(const de__csm_camera *cam, de_vec3 direction, u32 count, u32 resolution,
                      de__csm_cascade out[4]) {
    if (!cam || !out || count < 1 || count > 4 || resolution < 4 || !isfinite(cam->distance) ||
        !(cam->near_z > 0) || !(cam->distance > cam->near_z) || !(cam->aspect > 0) ||
        !(cam->tan_half_fov > 0 || cam->ortho_half_height > 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "CSM fit parameters");
    memset(out, 0, 4 * sizeof *out);
    de_vec3 z = de_v3_normalize_safe(direction, (de_vec3){0, 0, -1});
    const de_vec3 up = fabsf(z.z) < .99f ? (de_vec3){0, 0, 1} : (de_vec3){0, 1, 0};
    de_vec3 x = de_v3_normalize(de_v3_cross(z, up)), y = de_v3_cross(x, z);
    de_vec3 back = de_v3_scale(z, -1);
    f32 splits[5];
    de__csm_splits(cam->near_z, cam->distance, count, splits);
    for (u32 i = 0; i < count; ++i) {
        f32 mid = .5f * (splits[i] + splits[i + 1]);
        f32 h =
            cam->ortho_half_height > 0 ? cam->ortho_half_height : cam->tan_half_fov * splits[i + 1];
        f32 dz = splits[i + 1] - mid;
        f32 radius = sqrtf(dz * dz + h * h * (1 + cam->aspect * cam->aspect));
        /* Expand by the 10% fade band plus the maximum centre snapping displacement. */
        radius = ceilf(radius * (1.f / .9f) / (1.f - 1.732051f / (f32)resolution) * 16.f) / 16.f;
        f32 texel = 2.f * radius / (f32)resolution;
        de_vec3 center = de_v3_add(cam->position, de_v3_scale(cam->forward, mid));
        const f64 ox = cam->origin.x * x.x + cam->origin.y * x.y + cam->origin.z * x.z;
        const f64 oy = cam->origin.x * y.x + cam->origin.y * y.y + cam->origin.z * y.z;
        const f64 oz = cam->origin.x * back.x + cam->origin.y * back.y + cam->origin.z * back.z;
        f32 cx = snap(de_v3_dot(center, x), ox, texel), cy = snap(de_v3_dot(center, y), oy, texel);
        f32 cz = snap(de_v3_dot(center, back), oz, texel);
        center =
            de_v3_add(de_v3_add(de_v3_scale(x, cx), de_v3_scale(y, cy)), de_v3_scale(back, cz));
        /* Extrude upstream by shadow distance so off-frustum directional casters are retained. */
        const f32 upstream = cam->distance + radius;
        de_mat4 view = de_m4_identity();
        view.m[0] = x.x;
        view.m[4] = x.y;
        view.m[8] = x.z;
        view.m[12] = -cx;
        view.m[1] = y.x;
        view.m[5] = y.y;
        view.m[9] = y.z;
        view.m[13] = -cy;
        view.m[2] = back.x;
        view.m[6] = back.y;
        view.m[10] = back.z;
        view.m[14] = -cz - upstream;
        /* Keep initialized payload stores independent of the aligned struct's tail padding. */
        out[i].view = view;
        out[i].proj = de_m4_ortho_rz(-radius, radius, -radius, radius, 0, upstream + radius);
        out[i].center = center;
        out[i].radius = radius;
        out[i].texel = texel;
        out[i].near_distance = splits[i];
        out[i].far_distance = splits[i + 1];
    }
    return DE_OK;
}

/* de_mat4 aligns the fit to 16 bytes; tail padding is not part of cascade coverage/cadence. */
static bool same_fit(const de__csm_cascade *a, const de__csm_cascade *b) {
    return memcmp(&a->view, &b->view, sizeof a->view) == 0 &&
           memcmp(&a->proj, &b->proj, sizeof a->proj) == 0 &&
           memcmp(&a->center, &b->center, sizeof a->center) == 0 &&
           memcmp(&a->radius, &b->radius, sizeof a->radius) == 0 &&
           memcmp(&a->texel, &b->texel, sizeof a->texel) == 0 &&
           memcmp(&a->near_distance, &b->near_distance, sizeof a->near_distance) == 0 &&
           memcmp(&a->far_distance, &b->far_distance, sizeof a->far_distance) == 0;
}

#include "render/renderer_internal.h"
#include "render/lighting/lighting.h"
#include "render/world/render_world.h"
#include "render/graph/rg_internal.h"

typedef struct csm_view {
    de_view view;
    de_gpu_texture depth, mirrors[8];
    de_gpu_buffer metadata[DE__VIS_FRAME_RING];
    de__csm_gpu *mapped[DE__VIS_FRAME_RING];
    de__csm_gpu data[9];
    de__csm_cascade fit[8];
    u64 entities[2];
    u32 cascades, resolution, lights, depth_access, mirror_access[8], draws;
    bool valid[8], ready, borrowed;
    u64 last_used;
    u64 metadata_frame[DE__VIS_FRAME_RING], pending_frame;
    bool metadata_live[DE__VIS_FRAME_RING];
    const de_rg *pending;
} csm_view;
struct de__csm {
    de_renderer *renderer;
    de_gpu_device *dev;
    const de_allocator *alloc;
    de__shadow_views *pool;
    csm_view views[DE__VIEW_MAX], capture_maps;
    de_cvar cv_lights, cv_distance, cv_far_interval, cv_pcss;
};
static void csm_release(de__csm *s, csm_view *v) {
    if (!v->borrowed) {
        if (v->depth.id)
            de_gpu_texture_destroy(s->dev, v->depth);
        for (u32 i = 0; i < 8; ++i)
            if (v->mirrors[i].id)
                de_gpu_texture_destroy(s->dev, v->mirrors[i]);
    }
    for (u32 i = 0; i < DE__VIS_FRAME_RING; ++i)
        if (v->metadata[i].id)
            de_gpu_buffer_destroy(s->dev, v->metadata[i]);
    memset(v, 0, sizeof *v);
}
static void csm_stage(de__view_frame *vf, void *user) {
    de__csm *s = user;
    const de__vis_context *vc =
        de__vis_frame_context(de__views_vis(s->renderer), vf->view, vf->graph, vf->frame_index);
    if (!vc)
        return;
    de_result result = de__csm_build(s, vf, vc);
    if (result != DE_OK) {
        /* A partially built producer cannot shade with clear/stale maps. Make the receiver
         * graph fail rather than silently publish incorrect unshadowed direct lighting. */
        ++vf->graph->error_count;
        DE_ERROR(s->renderer->log, "CSM: %s", de_result_str(result));
    }
}
de_result de__csm_create_ex(de_gpu_device *dev, const de_allocator *alloc, de__csm **out) {
    if (!dev || !out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "CSM create");
    *out = NULL;
    if (!alloc)
        alloc = de_heap_allocator();
    de__csm *s = DE_NEW(alloc, de__csm, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "CSM state");
    memset(s, 0, sizeof *s);
    s->dev = dev;
    s->alloc = alloc;
    de_result result = de__shadow_views_create(dev, alloc, &s->pool);
    if (result != DE_OK) {
        DE_FREE(alloc, s, sizeof *s);
        return result;
    }
    s->cv_pcss = de__shadow_pcss_register();
    s->cv_lights =
        de_cvar_register(&(de_cvar_desc){.name = "r.csm_lights",
                                         .type = DE_CVAR_INT,
                                         .default_number = 1,
                                         .min = 1,
                                         .max = 2,
                                         .help = "Shadowed directional lights, at most two"});
    s->cv_distance =
        de_cvar_register(&(de_cvar_desc){.name = "r.shadow_distance",
                                         .type = DE_CVAR_FLOAT,
                                         .default_number = 200,
                                         .min = 1,
                                         .max = 100000,
                                         .help = "Directional shadow distance in metres"});
    s->cv_far_interval = de_cvar_register(&(de_cvar_desc){
        .name = "r.csm_far_update_interval",
        .type = DE_CVAR_INT,
        .default_number = 1,
        .min = 1,
        .max = 16,
        .help = "Far cascade update cadence; changed coverage/casters force an update"});
    *out = s;
    return DE_OK;
}
de_result de__csm_create(de_renderer *r, de__csm **out) {
    DE_TRY(de__csm_create_ex(r->dev, r->alloc, out));
    (*out)->renderer = r;
    de_result result =
        de__view_register_stage(r, DE__VIEW_SLOT_LIGHTING, -40, "shadow.csm", csm_stage, *out);
    if (result != DE_OK) {
        de__csm_destroy(*out);
        *out = NULL;
    }
    return result;
}
void de__csm_destroy(de__csm *s) {
    if (!s)
        return;
    if (s->renderer)
        de__view_unregister_stage(s->renderer, csm_stage, s);
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        csm_release(s, &s->views[i]);
    csm_release(s, &s->capture_maps);
    de__shadow_views_destroy(s->pool);
    DE_FREE(s->alloc, s, sizeof *s);
}
static de_result csm_storage(de__csm *s, csm_view *v, u32 count, u32 resolution, u32 lights,
                             de_view view) {
    if (v->view.id != view.id || v->cascades != count || v->resolution != resolution ||
        v->lights != lights) {
        /* Allocate a complete candidate before dropping the old state. */
        csm_view candidate = {
            .view = view, .cascades = count, .resolution = resolution, .lights = lights};
        candidate.depth = de_gpu_texture_create(
            s->dev, &(de_gpu_texture_desc){.dim = DE_TEX_2D_ARRAY,
                                           .format = DE_FMT_D32_FLOAT,
                                           .width = resolution,
                                           .height = resolution,
                                           .depth_or_layers = count * lights,
                                           .usage = DE_USAGE_DEPTH | DE_USAGE_COPY_SRC,
                                           .name = "csm.depth_array"});
        for (u32 i = 0; i < count * lights && candidate.depth.id; ++i) {
            candidate.mirrors[i] = de_gpu_texture_create(
                s->dev, &(de_gpu_texture_desc){.format = DE_FMT_R32_FLOAT,
                                               .width = resolution,
                                               .height = resolution,
                                               .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST |
                                                        DE_USAGE_COPY_SRC,
                                               .name = "csm.sampled_layer"});
            if (!candidate.mirrors[i].id) {
                csm_release(s, &candidate);
                return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "CSM layer mirror");
            }
        }
        if (!candidate.depth.id)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "CSM depth array");
        csm_release(s, v);
        *v = candidate;
    }
    return DE_OK;
}
de_result de__csm_build(de__csm *s, const de__view_frame *vf, const de__vis_context *vc) {
    if (!s || !vf || !vc || !vf->settings)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "CSM build");
    u32 slot = (vf->view.id & 255u) - 1u;
    if (slot >= DE__VIEW_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "CSM receiver slot");
    csm_view *v = &s->views[slot];
    if (v->pending)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "CSM receiver already reserved");
    v->draws = 0;
    if (vf->settings->shadows == DE_QUALITY_OFF || !vf->world || !vf->camera)
        return DE_OK;
    u32 count, resolution, taps;
    de__csm_quality(vf->settings->shadows, &count, &resolution, &taps);
    const de__rw_lights *world = &vf->world->lights;
    u32 selected[2], lights = 0, limit = (u32)de_cvar_int(s->cv_lights);
    if (limit < 1)
        limit = 1;
    if (limit > 2)
        limit = 2;
    for (u32 i = 0; i < world->count && lights < limit; ++i)
        if (world->entity[i] && world->light[i].kind == DE_LIGHT_DIRECTIONAL &&
            world->light[i].cast_shadows && world->light[i].intensity > 0 &&
            isfinite(world->light[i].intensity))
            selected[lights++] = i;
    if (!lights)
        return DE_OK;
    u32 ring = (u32)(vf->frame_index % DE__VIS_FRAME_RING);
    if (v->metadata_live[ring] && v->metadata_frame[ring] >= vc->frames_completed)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "CSM metadata still in flight");
    if (vf->probe_capture) {
        /* Capture graphs submit sequentially on graphics. They share one map set, while each
         * receiver keeps distinct immutable metadata/ring entries until its frame fence. */
        if (s->capture_maps.pending)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "capture shadow maps reserved");
        DE_TRY(csm_storage(s, &s->capture_maps, count, resolution, lights, (de_view){.id = 1}));
        if (v->view.id != vf->view.id || !v->borrowed || v->cascades != count ||
            v->resolution != resolution || v->lights != lights)
            csm_release(s, v);
        v->view = vf->view;
        v->borrowed = true;
        v->cascades = count;
        v->resolution = resolution;
        v->lights = lights;
        v->depth = s->capture_maps.depth;
        v->depth_access = s->capture_maps.depth_access;
        memcpy(v->mirrors, s->capture_maps.mirrors, sizeof v->mirrors);
        memcpy(v->mirror_access, s->capture_maps.mirror_access, sizeof v->mirror_access);
    } else {
        if (v->borrowed)
            csm_release(s, v);
        DE_TRY(csm_storage(s, v, count, resolution, lights, vf->view));
    }
    v->last_used = vf->frame_index;
    /* Bound retained ordinary-view image caches; a single required view may exceed this
     * retention target (two-light Ultra is 1 GiB). Eviction never drops current work, and
     * deferred retirement/atomic replacement can temporarily exceed the target. */
    for (; !v->borrowed;) {
        u64 bytes = 0;
        u32 oldest = DE__VIEW_MAX;
        for (u32 i = 0; i < DE__VIEW_MAX; ++i) {
            csm_view *other = &s->views[i];
            if (!other->depth.id || other->borrowed)
                continue;
            bytes +=
                (u64)other->resolution * other->resolution * other->cascades * other->lights * 8u;
            if (other != v && !other->pending &&
                (oldest == DE__VIEW_MAX || other->last_used < s->views[oldest].last_used))
                oldest = i;
        }
        if (bytes <= 512ull * 1024 * 1024 || oldest == DE__VIEW_MAX)
            break;
        csm_release(s, &s->views[oldest]);
    }
    v->pending = vf->graph;
    v->pending_frame = vf->frame_index;
    if (v->borrowed)
        s->capture_maps.pending = vf->graph;
    if (!v->metadata[ring].id) {
        v->metadata[ring] =
            de_gpu_buffer_create(s->dev, &(de_gpu_buffer_desc){.size = 9 * sizeof(de__csm_gpu),
                                                               .stride = sizeof(de__csm_gpu),
                                                               .usage = DE_USAGE_STORAGE,
                                                               .memory = DE_MEM_UPLOAD,
                                                               .name = "csm.metadata"});
        if (!v->metadata[ring].id ||
            !(v->mapped[ring] = de_gpu_buffer_map(s->dev, v->metadata[ring]))) {
            if (v->metadata[ring].id)
                de_gpu_buffer_destroy(s->dev, v->metadata[ring]);
            v->metadata[ring] = (de_gpu_buffer){0};
            v->mapped[ring] = NULL;
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "CSM metadata");
        }
    }
    f32 distance = (f32)de_cvar_float(s->cv_distance);
    const DeCamera *camera = &vf->camera->camera;
    de__csm_camera cam = {
        .position = vf->camera->offset,
        .forward = {-vf->view_matrix.m[2], -vf->view_matrix.m[6], -vf->view_matrix.m[10]},
        .origin = vf->world->origin,
        .near_z = camera->near_z > 0 ? camera->near_z : .1f,
        .distance = distance,
        .aspect = fabsf(vf->proj.m[5] / vf->proj.m[0]),
        .tan_half_fov = 1.f / fabsf(vf->proj.m[5]),
        .ortho_half_height =
            camera->projection == DE_PROJ_ORTHOGRAPHIC ? 1.f / fabsf(vf->proj.m[5]) : 0};
    if (cam.distance <= cam.near_z)
        cam.distance = cam.near_z + 1;
    de__csm_gpu data[9] = {0};
    data[0].sphere[0] = cam.position.x;
    data[0].sphere[1] = cam.position.y;
    data[0].sphere[2] = cam.position.z;
    data[0].sphere[3] = cam.distance;
    data[0].light[0] = cam.forward.x;
    data[0].light[1] = cam.forward.y;
    data[0].light[2] = cam.forward.z;
    data[0].identity[0] = lights;
    data[0].identity[1] = count;
    de_rg_resource depth = de_rg_import_texture(vf->graph, v->depth, v->depth_access);
    de_rg_resource mirrors[8] = {0};
    bool changed = vf->world->dirty_instance_count || vf->world->removed_instance_count ||
                   vf->world->origin_changed;
    u32 cadence = (u32)de_cvar_int(s->cv_far_interval);
    if (cadence < 1)
        cadence = 1;
    for (u32 l = 0; l < lights; ++l) {
        u32 index = selected[l];
        const de_affine *xf = &world->transform[index];
        de_vec3 direction =
            de_v3_normalize_safe((de_vec3){-xf->m[6], -xf->m[7], -xf->m[8]}, (de_vec3){0, 0, -1});
        de__csm_cascade fitted[4];
        DE_TRY(de__csm_fit(&cam, direction, count, resolution, fitted));
        for (u32 c = 0; c < count; ++c) {
            u32 layer = l * count + c;
            mirrors[layer] =
                de_rg_import_texture(vf->graph, v->mirrors[layer], v->mirror_access[layer]);
            bool update = vf->probe_capture || c < 2 || vf->frame_index % cadence == 0 || changed ||
                          !v->valid[layer] || v->entities[l] != world->entity[index] ||
                          !same_fit(&v->fit[layer], &fitted[c]);
            if (update) {
                DE_TRY(de__shadow_view_build(
                    s->pool, vf->graph,
                    &(de__shadow_view_desc){.scene = vc->gpu_scene,
                                            .depth = depth,
                                            .context = slot * 8 + layer,
                                            .layer = layer,
                                            .width = resolution,
                                            .height = resolution,
                                            .layer_mask = vc->layer_mask,
                                            .view = fitted[c].view,
                                            .proj = fitted[c].proj,
                                            .camera = cam.position,
                                            .origin = cam.origin,
                                            .frame = vf->frame_index,
                                            .frames_completed = vc->frames_completed,
                                            .clear_depth = true},
                    NULL));
                DE_TRY(de__shadow_copy_layer(s->pool, vf->graph, depth, layer, mirrors[layer],
                                             resolution, resolution));
                ++v->draws;
                v->fit[layer] = fitted[c];
            }
            de__csm_gpu *entry = &data[1 + layer];
            de_mat4 matrix = de_m4_mul(&v->fit[layer].proj, &v->fit[layer].view);
            for (u32 r = 0; r < 4; ++r)
                for (u32 col = 0; col < 4; ++col)
                    entry->matrix[r][col] = matrix.m[col * 4 + r];
            entry->sphere[0] = v->fit[layer].center.x;
            entry->sphere[1] = v->fit[layer].center.y;
            entry->sphere[2] = v->fit[layer].center.z;
            entry->sphere[3] = v->fit[layer].radius;
            entry->light[0] = direction.x;
            entry->light[1] = direction.y;
            entry->light[2] = direction.z;
            entry->light[3] = v->fit[layer].texel;
            entry->params[0] = 1.f / (cam.distance + 2.f * v->fit[layer].radius);
            entry->params[1] = world->light[index].source_radius;
            entry->params[2] = .1f;
            entry->identity[0] = (u32)world->entity[index];
            entry->identity[1] = (u32)(world->entity[index] >> 32);
            entry->map = (de__shadow_map_ref){
                .texture = de_gpu_texture_bindless(s->dev, v->mirrors[layer], 0, false),
                .layer = layer,
                .taps = taps,
                .resolution = resolution,
                .uv_clamp = {.5f / resolution, .5f / resolution, 1.f - .5f / resolution,
                             1.f - .5f / resolution},
                .depth_projection = {0.f, cam.distance + 2.f * v->fit[layer].radius, 0.f,
                                     world->light[index].source_radius},
                .uv_projection = {1.f / (2.f * v->fit[layer].radius),
                                  1.f / (2.f * v->fit[layer].radius),
                                  (f32)de_cvar_float(s->cv_pcss), 0.f},
                .filter_state = {1u, (u32)vf->settings->shadows, (u32)vf->frame_index,
                                 taps < 16u ? 16u : taps}};
        }
        v->entities[l] = world->entity[index];
    }
    memcpy(v->mapped[ring], data, sizeof data);
    memcpy(v->data, data, sizeof data);
    v->pending = vf->graph;
    de_rg_export(vf->graph, depth, DE_ACCESS_COPY_SRC);
    v->depth_access = DE_ACCESS_COPY_SRC;
    for (u32 i = 0; i < count * lights; ++i) {
        de_rg_export(vf->graph, mirrors[i], DE_ACCESS_SHADER_READ);
        v->mirror_access[i] = DE_ACCESS_SHADER_READ;
    }
    if (vf->renderer)
        de__csm_publish(s, vf->view, vf->graph, vf->frame_index,
                        de__lighting_view_inputs(de__views_lighting(vf->renderer), slot));
    return DE_OK;
}
void de__csm_complete(de__csm *s, de_view view, const de_rg *graph, bool attempted, bool success) {
    if (!s)
        return;
    de__shadow_views_end(s->pool, graph, attempted);
    u32 slot = (view.id & 255u) - 1u;
    if (slot >= DE__VIEW_MAX)
        return;
    csm_view *v = &s->views[slot];
    if (v->pending == graph) {
        v->pending = NULL;
        v->ready = success;
        if (attempted) {
            u32 ring = (u32)(v->pending_frame % DE__VIS_FRAME_RING);
            v->metadata_live[ring] = true;
            v->metadata_frame[ring] = v->pending_frame;
        }
        for (u32 i = 0; i < 8; ++i)
            v->valid[i] = success;
        if (!success) {
            v->depth_access = DE_ACCESS_NONE;
            memset(v->mirror_access, 0, sizeof v->mirror_access);
        }
        if (v->borrowed) {
            s->capture_maps.pending = NULL;
            s->capture_maps.depth_access = v->depth_access;
            memcpy(s->capture_maps.mirror_access, v->mirror_access, sizeof v->mirror_access);
        }
    }
}
bool de__csm_get(const de__csm *s, de_view view, de__csm_info *out) {
    u32 slot = (view.id & 255u) - 1u;
    if (!s || !out || slot >= DE__VIEW_MAX)
        return false;
    const csm_view *v = &s->views[slot];
    *out = (de__csm_info){.cascades = v->cascades,
                          .resolution = v->resolution,
                          .lights = v->lights,
                          .draws = v->draws,
                          .capacity = de__shadow_views_capacity(s->pool),
                          .depth = v->depth,
                          .ready = v->ready};
    memcpy(out->mirrors, v->mirrors, sizeof out->mirrors);
    memcpy(out->data, v->data, sizeof out->data);
    return true;
}

void de__csm_publish(de__csm *s, de_view view, de_rg *g, u64 frame, de__lighting_inputs *in) {
    u32 slot = (view.id & 255u) - 1u;
    if (!s || !in || slot >= DE__VIEW_MAX)
        return;
    csm_view *v = &s->views[slot];
    if (v->view.id != view.id || v->pending != g)
        return;
    in->shadow_records =
        de_rg_import_buffer(g, v->metadata[frame % DE__VIS_FRAME_RING], DE_ACCESS_SHADER_READ);
    in->shadow_texture_count = v->lights * v->cascades;
    for (u32 i = 0; i < in->shadow_texture_count; ++i)
        in->shadow_textures[i] = de_rg_import_texture(g, v->mirrors[i], v->mirror_access[i]);
}
