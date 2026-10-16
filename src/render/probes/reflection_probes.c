#include "render/probes/probes_internal.h"
#include "render/renderer_internal.h"
#include "render/lighting/lighting.h"
#include "render/graph/rg_internal.h"
#include <dengine/de_debug.h>
#include "asset/slots_internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef struct probe_record {
    de__probe_snapshot snapshot;
    u64 hash, last_capture;
    de__probe_handle handle;
    u32 pool, faces, capture_faces;
    bool queued, loaded, live, wanted;
} probe_record;
typedef struct probe_gpu {
    f32 position[4], extent[4], rotation[4], blend[4];
    u32 info[4];
} probe_gpu;
DE_STATIC_ASSERT(sizeof(probe_gpu) == 80, "ProbeGpu is 80 bytes");
typedef de__probe_bounds_ring probe_blend_view;
typedef struct probe_sample_params {
    u32 probes, entry, list, scene, slot, depth, gb0, gb1, gb2, output, width, height;
    u32 tile, tiles_x, tiles_y, slices;
    f32 near_z, log_scale, log_bias;
    u32 sampler, lut, ao, reserved0, reserved1;
} probe_sample_params;
typedef struct probe_cull_params {
    u32 bounds, probes, list, count, clusters, scene, slot, entry, bounds_offset, reserved0,
        reserved1, reserved2;
} probe_cull_params;
DE_STATIC_ASSERT(sizeof(probe_cull_params) == 48, "ProbeCullParams is 48 bytes");
static void probe_blend_stage(de__view_frame *vf, void *user);
struct de__probes {
    de_renderer *renderer;
    de__probe_store pools[1];
    de__probe_capture captures[3];
    de__probe_capture *capture;
    probe_record records[DE__PROBE_MAX];
    u32 count, active, pool_size;
    de_cvar cv_faces, cv_pool;
    bool command_owner;
    de__vis_kernel cull;
    de_gpu_buffer gpu;
    probe_gpu *gpu_map;
    u64 gpu_frame;
    u32 gpu_count;
    bool gpu_valid;
    probe_blend_view blend_views[DE__VIEW_MAX];
};

static de_result bake_command(int argc, const char *const *argv, void *user) {
    (void)argv;
    if (argc != 1)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "usage: r.probes_bake");
    struct de__probes *p = (struct de__probes *)user;
    de_renderer *r = p->renderer;
    de__render_world_wait_idle(r);
    for (u32 i = 0; i < p->count; ++i) {
        probe_record *rec = &p->records[i];
        de__probe_store *s = &p->pools[rec->pool];
        if (!de__probe_store_valid(s, rec->handle) || !s->slots[rec->handle.slot].ready)
            return DE_FAIL(DE_ERR_BUSY, "probe bake: capture has not completed");
        u64 size =
            de__probe_payload_size(s->slots[rec->handle.slot].resolution) + DE__PROBE_BLOB_HEADER;
        u8 *blob = (u8 *)DE_ALLOC(r->alloc, (size_t)size, 16, DE_MEM_RENDER);
        if (!blob)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "probe bake: blob");
        de_result result =
            de__probe_store_readback(p->capture, s, rec->handle, blob + DE__PROBE_BLOB_HEADER);
        if (result == DE_OK)
            result =
                de__probe_blob_write(blob, size, rec->hash, s->slots[rec->handle.slot].resolution,
                                     blob + DE__PROBE_BLOB_HEADER);
        char path[128];
        (void)snprintf(path, sizeof path, "dev:/probe-%016llx.deprobe",
                       (unsigned long long)rec->snapshot.entity);
        if (result == DE_OK)
            result = de_file_write_all_atomic(path, blob, size);
        DE_FREE(r->alloc, blob, (size_t)size);
        if (result != DE_OK)
            return result;
    }
    return DE_OK;
}
de_result de__probes_create(de_renderer *r, struct de__probes **out) {
    struct de__probes *p = DE_NEW(r->alloc, struct de__probes, DE_MEM_RENDER);
    if (!p)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "probes: state");
    memset(p, 0, sizeof *p);
    p->renderer = r;
    p->capture = &p->captures[0];
    p->active = DE__PROBE_NONE;
    p->cv_faces = de_cvar_register(&(de_cvar_desc){.name = "r.probe_faces_per_frame",
                                                   .type = DE_CVAR_INT,
                                                   .default_number = 6,
                                                   .min = 1,
                                                   .max = 6});
    p->cv_pool = de_cvar_register(&(de_cvar_desc){.name = "r.probe_pool_size",
                                                  .type = DE_CVAR_INT,
                                                  .default_number = 16,
                                                  .min = 1,
                                                  .max = 64});
    de_result result = de_command_register(
        "r.probes_bake", "Write ready probes to dev:/ as .deprobe blobs", bake_command, p);
    p->command_owner = result == DE_OK;
    if (result != DE_OK)
        de_error_clear();
    result = de__view_register_stage(r, DE__VIEW_SLOT_LIGHTING, -10, "probe.specular",
                                     probe_blend_stage, p);
    if (result != DE_OK) {
        de__probes_destroy(r, p);
        return result;
    }
    *out = p;
    return DE_OK;
}
void de__probes_destroy(de_renderer *r, struct de__probes *p) {
    if (!p)
        return;
    if (p->command_owner)
        de_command_unregister("r.probes_bake");
    de__view_unregister_stage(r, probe_blend_stage, p);
    de__vis_shared *vis = de__views_vis(r);
    de__vis_kernel_destroy(vis, &p->cull);
    de_gpu_buffer_destroy(r->dev, p->gpu);
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        de_gpu_buffer_destroy(r->dev, p->blend_views[i].bounds);
    for (u32 i = 0; i < 3; ++i)
        de__probe_capture_destroy(&p->captures[i]);
    for (u32 i = 0; i < 1; ++i)
        de__probe_store_destroy(&p->pools[i]);
    DE_FREE(r->alloc, p, sizeof *p);
}
de_result de_render_probe_request_update(de_renderer *r, de_entity entity) {
    if (!r || !r->probes || !entity.bits)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "probe request: invalid renderer/entity");
    /* Main-thread callers may arrive before Extract waits for a pipelined prior frame. */
    de__render_world_wait_idle(r);
    struct de__probes *p = r->probes;
    for (u32 i = 0; i < p->count; ++i)
        if (p->records[i].snapshot.entity == entity.bits) {
            p->records[i].queued = true;
            return DE_OK;
        }
    return DE_FAIL(DE_ERR_NOT_FOUND, "probe request: entity was not extracted");
}
void de__probes_prepare(de_renderer *r, const de__render_world_frame *world) {
    struct de__probes *p = r->probes;
    if (!p)
        return;
    for (u32 i = 0; i < 3; ++i)
        if (p->captures[i].initialized)
            de__probe_capture_at_position(&p->captures[i], (de_dvec3){0}, (de_dvec3){0}, 0);
    u32 capacity = (u32)de_cvar_int(p->cv_pool);
    if (!capacity)
        capacity = 16;

    for (u32 i = 0; i < p->count; ++i)
        p->records[i].live = false;
    if (world)
        for (u32 n = 0; n < world->probe_count; ++n) {
            const de__probe_snapshot *snap = &world->probes[n];
            u32 at = 0;
            for (; at < p->count; ++at)
                if (p->records[at].snapshot.entity == snap->entity)
                    break;
            if (at == p->count) {
                if (p->count == DE__PROBE_MAX)
                    continue;
                memset(&p->records[at], 0, sizeof p->records[at]);
                p->records[at].handle.slot = DE__PROBE_NONE;
                ++p->count;
            }
            probe_record *rec = &p->records[at];
            u64 hash = de__probe_settings_hash(snap);
            u32 new_pool = 0;
            if (hash != rec->hash || rec->snapshot.probe.bake.id != snap->probe.bake.id) {
                if (de__probe_store_valid(&p->pools[rec->pool], rec->handle))
                    de__probe_store_free(&p->pools[rec->pool], rec->handle.slot);
                rec->handle = (de__probe_handle){DE__PROBE_NONE, 0};
                rec->queued = true;
                rec->faces = 0;
                rec->loaded = false;
                rec->hash = hash;
            }
            rec->snapshot = *snap;
            rec->live = true;
            rec->pool = new_pool;
            rec->wanted = true;
            if (!de__probe_store_valid(&p->pools[0], rec->handle) && world) {
                for (u32 v = 0; v < world->view_count; ++v) {
                    de_vec3 delta = de_dv3_relative(snap->position, world->origin);
                    delta = de_v3_sub(world->views[v].offset, delta);
                    de_quat q = de_quat_normalize(snap->rotation);
                    q.x = -q.x;
                    q.y = -q.y;
                    q.z = -q.z;
                    if (de__probe_weight(&snap->probe, de_quat_rotate(q, delta)) > 0)
                        rec->queued = true;
                }
            }
            if (snap->probe.update == DE_PROBE_EVERY_N_FRAMES &&
                r->frame_index - rec->last_capture >=
                    (snap->probe.update_interval ? snap->probe.update_interval : 60))
                rec->queued = true;
        }
    for (u32 i = 0; i < p->count;) {
        if (p->records[i].live) {
            ++i;
            continue;
        }
        if (p->active == i)
            p->active = DE__PROBE_NONE;
        probe_record *rec = &p->records[i];
        if (de__probe_store_valid(&p->pools[rec->pool], rec->handle))
            de__probe_store_free(&p->pools[rec->pool], rec->handle.slot);
        if (p->active == p->count - 1)
            p->active = i;
        p->records[i] = p->records[--p->count];
    }
    if (!p->count)
        return;
    bool resize = p->pool_size && capacity != p->pool_size;

    if (resize) {
        de_gpu_wait_idle(r->dev);
        for (u32 i = 0; i < 1; ++i)
            de__probe_store_destroy(&p->pools[i]);
        for (u32 i = 0; i < p->count; ++i) {
            p->records[i].handle.slot = DE__PROBE_NONE;
            p->records[i].queued = true;
            p->records[i].faces = 0;
        }
        p->active = DE__PROBE_NONE;
    }
    p->pool_size = capacity;
    if (p->active == DE__PROBE_NONE) {
        for (u32 i = 0; i < p->count; ++i) {
            probe_record *rec = &p->records[i];
            if (!rec->queued || !rec->wanted)
                continue;
            if (!rec->loaded && DE_HANDLE_VALID(rec->snapshot.probe.bake) && r->assets) {
                de__slot *slot = de__slot_get(r->assets, rec->snapshot.probe.bake.id);
                if (slot && (slot->state == DE_LOAD_PENDING || slot->state == DE_LOAD_LOADING))
                    continue;
            }
            if (p->active == DE__PROBE_NONE ||
                rec->snapshot.probe.priority > p->records[p->active].snapshot.probe.priority ||
                (rec->snapshot.probe.priority == p->records[p->active].snapshot.probe.priority &&
                 rec->snapshot.entity < p->records[p->active].snapshot.entity))
                p->active = i;
        }
    }
    if (p->active == DE__PROBE_NONE)
        return;
    probe_record *rec = &p->records[p->active];
    de__probe_store *s = &p->pools[rec->pool];
    u32 resolution = rec->snapshot.probe.resolution ? rec->snapshot.probe.resolution : 128;
    if (resolution != 64 && resolution != 128 && resolution != 256)
        resolution = 128;
    if (!s->capacity && de__probe_store_init(s, r->dev, r->alloc, 256, capacity) != DE_OK)
        return;
    /* Keep one lazy capture per supported resolution: switching probes must not churn
     * persistent view graphs or reinterpret retired bindless descriptor indices. */
    de__probe_capture *candidate = &p->captures[resolution == 64 ? 0 : resolution == 128 ? 1 : 2];
    if (!candidate->initialized) {
        if (de__probe_capture_init(candidate, r, resolution) != DE_OK)
            return;
    }
    p->capture = candidate;
    if (!de__probe_store_valid(s, rec->handle))
        rec->handle =
            de__probe_store_alloc_resolution(s, rec->snapshot.entity, r->frame_index, resolution);
    if (!de__probe_store_valid(s, rec->handle))
        return;
    if (!rec->loaded && DE_HANDLE_VALID(rec->snapshot.probe.bake) && r->assets) {
        u64 size = 0;
        const void *blob = de_blob_data(r->assets, rec->snapshot.probe.bake, &size), *data = NULL;
        u32 baked_resolution;
        if (blob && de__probe_blob_read(blob, size, rec->hash, &baked_resolution, &data) == DE_OK &&
            baked_resolution == resolution &&
            de__probe_store_upload(p->capture, s, rec->handle, data) == DE_OK) {
            rec->loaded = true;
            rec->queued = false;
            rec->last_capture = r->frame_index;
            p->active = DE__PROBE_NONE;
            return;
        }
        /* A missing final/stale asset falls back to capture, without starving other probes. */
        de_error_clear();
        rec->loaded = true;
    }
    u32 budget = (u32)de_cvar_int(p->cv_faces);
    if (!budget)
        budget = 6;
    u32 mask = 0;
    for (u32 f = 0; f < 6 && budget; ++f)
        if (!(rec->faces & (1u << f))) {
            mask |= 1u << f;
            --budget;
        }
    de__probe_capture_at_position(p->capture, rec->snapshot.position,
                                  world ? world->origin : (de_dvec3){0}, mask);
}
void de__probes_after_views(de_renderer *r, de_arena *arena, u64 frame) {
    (void)arena;
    struct de__probes *p = r->probes;
    if (!p || p->active == DE__PROBE_NONE || !p->capture->scheduled)
        return;
    probe_record *rec = &p->records[p->active];
    u32 faces = rec->faces | p->capture->scheduled;
    if (de__probe_capture_complete(p->capture, &p->pools[rec->pool], rec->handle, faces == 63) !=
        DE_OK)
        return;
    rec->faces = faces;
    for (u32 f = 0; f < 6; ++f)
        if (p->capture->scheduled & (1u << f))
            ++rec->capture_faces;
    if (faces == 63) {
        rec->queued = false;
        rec->last_capture = frame;
        rec->faces = 0;
        p->active = DE__PROBE_NONE;
    }
}
bool de__probes_capture_view(const de_renderer *r, de_view view) {
    if (!r || !r->probes)
        return false;
    for (u32 c = 0; c < 3; ++c)
        for (u32 i = 0; i < 6; ++i)
            if (r->probes->captures[c].views[i].id == view.id)
                return true;
    return false;
}

/* Cluster list producer. It shares S14's grid and bounds, but publishes the existing RGBA
 * indirect-specular contract before lighting, so SSR can composite over it later. */
void de__probe_cluster_bounds(const de__cluster_grid *grid, const de_mat4 *proj, f32 *bounds);
static de_result blend_init(struct de__probes *p) {
    if (p->gpu_map && DE_HANDLE_VALID(p->cull.pipeline))
        return DE_OK;
    de_renderer *r = p->renderer;
    de__vis_shared *vis = de__views_vis(r);
    de__vis_kernel_destroy(vis, &p->cull);
    de_gpu_buffer_destroy(r->dev, p->gpu);
    p->gpu = (de_gpu_buffer){0};
    p->gpu_map = NULL;
    const char *cull[] = {"cs_probe_cluster_list"};
    de__engine_shader_code code;
    const u32 g64[] = {64, 1, 1};
    DE_TRY(de__vis_shader_load(vis, "lighting_cull", cull, 1, &code));
    de_result result = de__vis_kernel_create(vis, &code, 0, "probe.cluster_lists", g64,
                                             sizeof(probe_cull_params), &p->cull);
    de__engine_shader_code_free(&code);
    if (result != DE_OK)
        return result;
    p->gpu = de_gpu_buffer_create(
        r->dev,
        &(de_gpu_buffer_desc){.size = sizeof(probe_gpu) * DE__PROBE_MAX * DE__VIS_FRAME_RING,
                              .stride = sizeof(probe_gpu),
                              .memory = DE_MEM_UPLOAD,
                              .usage = DE_USAGE_STORAGE,
                              .name = "probe.records"});
    p->gpu_map = (probe_gpu *)de_gpu_buffer_map(r->dev, p->gpu);
    return p->gpu_map ? DE_OK : DE_FAIL(DE_ERR_OUT_OF_MEMORY, "probe records: upload ring");
}
static void pack_probes(struct de__probes *p, const de__view_frame *vf) {
    if (p->gpu_valid && p->gpu_frame == vf->frame_index)
        return;
    p->gpu_valid = true;
    p->gpu_frame = vf->frame_index;
    p->gpu_count = 0;
    u32 order[DE__PROBE_MAX], count = 0;
    for (u32 i = 0; i < p->count; ++i) {
        probe_record *rec = &p->records[i];
        de__probe_store *s = &p->pools[rec->pool];
        if (!rec->wanted || !de__probe_store_valid(s, rec->handle) ||
            !s->slots[rec->handle.slot].ready)
            continue;
        u32 at = count;
        while (
            at > 0 &&
            (p->records[order[at - 1]].snapshot.probe.priority < rec->snapshot.probe.priority ||
             (p->records[order[at - 1]].snapshot.probe.priority == rec->snapshot.probe.priority &&
              p->records[order[at - 1]].snapshot.entity > rec->snapshot.entity))) {
            order[at] = order[at - 1];
            --at;
        }
        order[at] = i;
        ++count;
    }
    probe_gpu *dst = p->gpu_map + (vf->frame_index % DE__VIS_FRAME_RING) * DE__PROBE_MAX;
    for (u32 k = 0; k < count; ++k) {
        probe_record *rec = &p->records[order[k]];
        de__probe_snapshot *snap = &rec->snapshot;
        de__probe_store *s = &p->pools[rec->pool];
        DeReflectionProbe *settings = &snap->probe;
        probe_gpu g;
        memset(&g, 0, sizeof g);
        de_vec3 pos =
            de_dv3_relative(snap->position, vf->world ? vf->world->origin : (de_dvec3){0});
        g.position[0] = pos.x;
        g.position[1] = pos.y;
        g.position[2] = pos.z;
        g.position[3] = settings->shape == DE_PROBE_SPHERE
                            ? settings->extent.x
                            : sqrtf(settings->extent.x * settings->extent.x +
                                    settings->extent.y * settings->extent.y +
                                    settings->extent.z * settings->extent.z);
        g.extent[0] = settings->extent.x;
        g.extent[1] = settings->extent.y;
        g.extent[2] = settings->extent.z;
        g.extent[3] = settings->intensity;
        de_quat q = de_quat_normalize(snap->rotation);
        g.rotation[0] = q.x;
        g.rotation[1] = q.y;
        g.rotation[2] = q.z;
        g.rotation[3] = q.w;
        g.blend[0] = settings->blend_distance;
        g.blend[1] = (f32)s->slots[rec->handle.slot].resolution;
        g.info[0] = (u32)settings->shape;
        g.info[1] = settings->box_projection ? 1 : 0;
        g.info[2] =
            de_gpu_texture_bindless(p->renderer->dev, s->slots[rec->handle.slot].strip, 0, false);
        g.info[3] = s->slots[rec->handle.slot].mips;
        dst[k] = g;
        de_vec3 delta = de_v3_sub(vf->camera->offset, pos);
        de_quat inverse = {-q.x, -q.y, -q.z, q.w};
        if (de__probe_weight(settings, de_quat_rotate(inverse, delta)) > 0)
            s->slots[rec->handle.slot].used = vf->frame_index;
    }
    p->gpu_count = count;
}
typedef struct probe_blend_pass {
    struct de__probes *p;
    probe_cull_params cull;
    de_rg_resource bounds, list;
} probe_blend_pass;
static void probe_blend_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const probe_blend_pass *p = (const probe_blend_pass *)user;
    de_gpu_device *dev = p->p->renderer->dev;
    probe_cull_params pc = p->cull;
    pc.list = de_gpu_buffer_bindless(dev, de_rg_buffer(pass, p->list));
    de_gpu_cmd_set_pipeline(cmd, p->p->cull.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, (pc.clusters + 63) / 64, 1, 1);
}
static void probe_blend_stage(de__view_frame *vf, void *user) {
    struct de__probes *p = (struct de__probes *)user;
    de_renderer *r = p->renderer;
    if (vf->probe_capture || !p->count || !p->capture->initialized ||
        vf->settings->reflections == DE_QUALITY_OFF)
        return;
    de__vis_context *vc =
        de__vis_frame_context(de__views_vis(r), vf->view, vf->graph, vf->frame_index);
    if (!vc)
        return;
    if (blend_init(p) != DE_OK)
        return;
    pack_probes(p, vf);
    if (!p->gpu_count)
        return;
    de__lighting *lt = de__views_lighting(r);
    de__lighting_inputs *inputs = de__lighting_view_inputs(lt, vc->view_slot);
    if (!inputs)
        return;
    u32 tile = (u32)de_cvar_int(lt->cv_tile), slices = (u32)de_cvar_int(lt->cv_slices);
    de__cluster_grid grid =
        de__cluster_grid_make(vf->width, vf->height, tile ? tile : 64, slices ? slices : 32,
                              vf->camera->camera.near_z, (f32)de_cvar_float(lt->cv_far));
    probe_blend_view *bv = &p->blend_views[vc->view_slot];
    if (de__probe_bounds_reserve(r->dev, bv, grid.count) != DE_OK)
        return;
    u32 entry = (u32)(vf->frame_index % DE__VIS_FRAME_RING);
    de__probe_cluster_bounds(&grid, &vf->proj, bv->map + (size_t)entry * bv->capacity * 8);
    de_rg *g = vf->graph;
    probe_blend_pass *c = (probe_blend_pass *)de_arena_push(g->arena, sizeof *c, 16);
    if (!c)
        return;
    memset(c, 0, sizeof *c);
    c->p = p;
    c->bounds = de_rg_import_buffer(g, bv->bounds, DE_ACCESS_SHADER_READ);
    c->list = de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = (u64)grid.count * 65 * 4,
                                                           .usage = DE_USAGE_STORAGE,
                                                           .name = "probe.cluster_lists"});
    c->cull = (probe_cull_params){.bounds = de_gpu_buffer_bindless(r->dev, bv->bounds),
                                  .probes = de_gpu_buffer_bindless(r->dev, p->gpu),
                                  .count = p->gpu_count,
                                  .clusters = grid.count,
                                  .scene = vc->scene_views_bindless,
                                  .slot = vc->view_slot,
                                  .entry = entry,
                                  .bounds_offset = entry * bv->capacity * 32u};
    de_rg_resource records = de_rg_import_buffer(g, p->gpu, DE_ACCESS_SHADER_READ);
    de_rg_pass *pass =
        de_rg_add_pass(g, "probe.cluster_lists", DE_QUEUE_GRAPHICS, probe_blend_execute, c);
    de_rg_read(pass, c->bounds, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, records, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, vc->scene_views, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, c->list, DE_ACCESS_SHADER_WRITE);
    inputs->probe_records = records;
    inputs->probe_list = c->list;
    inputs->probe_entry = entry;
    inputs->probe_sampler = de_gpu_sampler_bindless(r->dev, p->capture->sampler);
    for (u32 k = 0; k < 4; ++k)
        inputs->probe_view_z[k] = vf->view_matrix.m[k * 4 + 2];
    inputs->probe_texture_count = 0;
    for (u32 i = 0; i < p->count; ++i) {
        probe_record *rec = &p->records[i];
        de__probe_store *store = &p->pools[rec->pool];
        if (rec->wanted && de__probe_store_valid(store, rec->handle) &&
            store->slots[rec->handle.slot].ready)
            inputs->probe_textures[inputs->probe_texture_count++] = de_rg_import_texture(
                g, store->slots[rec->handle.slot].strip, DE_ACCESS_SHADER_READ);
    }
}

bool de__probes_info(const de_renderer *r, u64 entity, de__probe_info *out) {
    if (!r || !r->probes || !out)
        return false;
    const struct de__probes *p = r->probes;
    for (u32 i = 0; i < p->count; ++i)
        if (p->records[i].snapshot.entity == entity) {
            const probe_record *rec = &p->records[i];
            const de__probe_store *s = &p->pools[rec->pool];
            *out = (de__probe_info){
                .resolution = rec->snapshot.probe.resolution ? rec->snapshot.probe.resolution : 128,
                .faces = rec->faces,
                .capture_faces = rec->capture_faces,
                .ready = de__probe_store_valid(s, rec->handle) && s->slots[rec->handle.slot].ready,
                .baked = rec->loaded};
            return true;
        }
    return false;
}
de_result de__probes_readback(de_renderer *r, u64 entity, void *payload, u64 size) {
    if (!r || !r->probes)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "probe readback: no renderer");
    struct de__probes *p = r->probes;
    for (u32 i = 0; i < p->count; ++i)
        if (p->records[i].snapshot.entity == entity) {
            probe_record *rec = &p->records[i];
            de__probe_store *s = &p->pools[rec->pool];
            if (!de__probe_store_valid(s, rec->handle))
                return DE_FAIL(DE_ERR_NOT_FOUND, "probe readback: nonresident");
            if (size != de__probe_payload_size(s->slots[rec->handle.slot].resolution))
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "probe readback: size");
            return de__probe_store_readback(p->capture, s, rec->handle, payload);
        }
    return DE_FAIL(DE_ERR_NOT_FOUND, "probe readback: entity");
}

de_result de__probe_bounds_reserve(de_gpu_device *dev, de__probe_bounds_ring *ring, u32 count) {
    if (!dev || !ring || !count)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "probe bounds: invalid descriptor");
    if (ring->capacity >= count && ring->map)
        return DE_OK;
    de_gpu_buffer buffer = de_gpu_buffer_create(
        dev, &(de_gpu_buffer_desc){.size = (u64)count * 32 * DE__VIS_FRAME_RING,
                                   .memory = DE_MEM_UPLOAD,
                                   .usage = DE_USAGE_STORAGE,
                                   .name = "probe.cluster_bounds"});
    if (!DE_HANDLE_VALID(buffer))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "probe bounds: allocation");
    f32 *map = (f32 *)de_gpu_buffer_map(dev, buffer);
    if (!map) {
        de_gpu_buffer_destroy(dev, buffer);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "probe bounds: map");
    }
    de_gpu_buffer_destroy(dev, ring->bounds);
    *ring = (de__probe_bounds_ring){buffer, map, count};
    return DE_OK;
}
