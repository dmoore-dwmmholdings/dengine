/* DENG-46: renderer-owned direct-light reservoirs and source distribution. */
#include "render/rt/restir_di.h"
#include "render/graph/rg_internal.h"
#include "render/passes/vis/gbuffer.h"
#include "render/shaders/engine_shaders.h"
#include "render/world/render_world.h"
#include <math.h>
#include <string.h>

void de__di_update(de__di_reservoir *r, u32 source, de_vec2 sample, f32 target, f32 weight, u32 m,
                   f32 random) {
    r->m += m;
    if (!(weight > 0) || !isfinite(weight))
        return;
    r->sum += weight;
    if (random * r->sum < weight) {
        r->source = source;
        r->u = sample.x;
        r->v = sample.y;
        r->target = target;
    }
}
void de__di_merge(de__di_reservoir *r, const de__di_reservoir *q, f32 target, u32 cap, f32 random) {
    u32 m = q->m < cap ? q->m : cap;
    if (q->source == UINT32_MAX) {
        r->m += m;
        return;
    }
    de__di_update(r, q->source, (de_vec2){q->u, q->v}, target, target * q->weight * m, m, random);
}
void de__di_finalize(de__di_reservoir *r, u32 normalization) {
    r->weight = r->target > 0 && normalization ? r->sum / (r->target * normalization) : 0;
    if (!isfinite(r->weight))
        r->weight = 0;
}
de_result de__di_alias_build(const f32 *power, u32 count, de__di_alias *out, u32 *scratch) {
    if (!power || !count || !out || !scratch)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "DI alias arguments");
    f64 total = 0;
    for (u32 i = 0; i < count; ++i)
        if (isfinite(power[i]) && power[i] > 0)
            total += power[i];
    u32 small = 0, large = 0;
    for (u32 i = 0; i < count; ++i) {
        f32 w = isfinite(power[i]) && power[i] > 0 ? power[i] : 0;
        out[i] = (de__di_alias){.pdf = total > 0 ? (f32)(w / total) : 1.f / count, .alias = i};
        out[i].probability = out[i].pdf * count;
        if (out[i].probability < 1)
            scratch[small++] = i;
        else
            scratch[count + large++] = i;
    }
    while (small && large) {
        u32 a = scratch[--small], b = scratch[count + --large];
        out[a].alias = b;
        out[b].probability += out[a].probability - 1;
        if (out[b].probability < 1)
            scratch[small++] = b;
        else
            scratch[count + large++] = b;
    }
    while (small)
        out[scratch[--small]].probability = 1;
    while (large)
        out[scratch[count + --large]].probability = 1;
    return DE_OK;
}
typedef struct di_descriptor {
    u32 kind, index, first, submesh;
} di_descriptor;
typedef struct di_params {
    u32 scene, scene_slot, view, entry;
    u32 depth, gb0, gb1, gb2;
    u32 motion, ids, visible, ids_pitch;
    u32 visible_count, sources, source_count, descriptors;
    u32 alias_work, initial, previous, temporal;
    u32 spatial, history_previous, history_current, rays;
    u32 hits, diffuse, specular, distance;
    u32 width, height, frame, candidates;
    u32 unbiased, initial_visibility, history_valid, metadata;
    u32 sampler, layers, padding[26];
} di_params;
DE_STATIC_ASSERT(sizeof(di_params) == 256, "DI parameters mirror 64 words");
#define DI_RING         8u
#define DI_STAGES       16u
#define DI_SOURCE_BYTES 192u
enum {
    K_SOURCES,
    K_ALIAS,
    K_INITIAL,
    K_INITIAL_VIS,
    K_TEMPORAL,
    K_SPATIAL,
    K_SPATIAL_VIS,
    K_FINAL_RAY,
    K_SHADE,
    K_REFERENCE,
    K_COUNT
};
typedef struct di_view {
    de_view view;
    de_gpu_buffer scratch, reservoir[2], surface[2], rays, hits, params;
    de_gpu_texture diffuse, specular, distance;
    u32 width, height, ring, mode;
    u64 epoch, last_frame;
    bool completed;
    de_vec3 camera;
    de_mat4 matrix, projection;
    de_rg *pending;
    u64 pending_frame;
    bool pending_alias;
    de__di_info info;
} di_view;
struct de__restir_di {
    de_renderer *r;
    de_gpu_pipeline kernels[K_COUNT];
    de_cvar enable, unbiased, initial_visibility, candidates;
    di_descriptor *descriptors;
    u32 count, capacity, emissive, descriptor_ring;
    de_gpu_buffer descriptor_upload[DI_RING], sources, alias_work;
    u64 signature, epoch, alias_builds;
    bool needs_alias;
    de__di_debug debug;
    di_view views[32];
    di_view source_view; /* PT shares the exact source/alias kernels without DI reservoirs. */
};
static bool enabled(de__restir_di *s, const de_render_settings *settings) {
    if (!s || settings->tier != DE_TIER_RT || !s->r->caps.ray_query)
        return false;
    i64 override = de_cvar_int(s->enable);
    return override >= 0 ? override != 0
                         : settings->rt_shadows && settings->shadows >= DE_QUALITY_HIGH;
}
static de_gpu_buffer make_buffer(de__restir_di *s, u64 size, u32 stride, de_gpu_memory memory,
                                 const char *name) {
    return de_gpu_buffer_create(s->r->dev,
                                &(de_gpu_buffer_desc){.size = size ? size : 16,
                                                      .stride = stride,
                                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                      .memory = memory,
                                                      .name = name});
}
static void release_view(de__restir_di *s, di_view *v) {
    de_gpu_device *d = s->r->dev;
    de_gpu_buffer_destroy(d, v->scratch);
    de_gpu_buffer_destroy(d, v->rays);
    de_gpu_buffer_destroy(d, v->hits);
    de_gpu_buffer_destroy(d, v->params);
    for (u32 i = 0; i < 2; ++i) {
        de_gpu_buffer_destroy(d, v->reservoir[i]);
        de_gpu_buffer_destroy(d, v->surface[i]);
    }
    de_gpu_texture_destroy(d, v->diffuse);
    de_gpu_texture_destroy(d, v->specular);
    de_gpu_texture_destroy(d, v->distance);
    memset(v, 0, sizeof *v);
}
void de__restir_di_destroy(de__restir_di *s) {
    if (!s)
        return;
    for (u32 i = 0; i < 32; ++i)
        release_view(s, &s->views[i]);
    release_view(s, &s->source_view);
    for (u32 i = 0; i < K_COUNT; ++i)
        de_gpu_pipeline_destroy(s->r->dev, s->kernels[i]);
    for (u32 i = 0; i < DI_RING; ++i)
        de_gpu_buffer_destroy(s->r->dev, s->descriptor_upload[i]);
    de_gpu_buffer_destroy(s->r->dev, s->sources);
    de_gpu_buffer_destroy(s->r->dev, s->alias_work);
    if (s->descriptors)
        DE_FREE(s->r->alloc, s->descriptors, (size_t)s->capacity * sizeof *s->descriptors);
    DE_FREE(s->r->alloc, s, sizeof *s);
}
de_result de__restir_di_create(de_renderer *r, de__restir_di **out) {
    *out = NULL;
    de__restir_di *s = DE_ALLOC(r->alloc, sizeof *s, 16, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "DI owner");
    memset(s, 0, sizeof *s);
    s->r = r;
    s->enable =
        de_cvar_register(&(de_cvar_desc){.name = "r.rt.restir_di",
                                         .help = "ReSTIR local direct light: -1 auto, 0 off, 1 on",
                                         .type = DE_CVAR_INT,
                                         .default_number = -1,
                                         .min = -1,
                                         .max = 1});
    s->unbiased = de_cvar_register(&(de_cvar_desc){.name = "r.rt.restir_di_unbiased",
                                                   .help = "Visibility-corrected spatial reuse",
                                                   .type = DE_CVAR_BOOL,
                                                   .default_number = 0});
    s->initial_visibility =
        de_cvar_register(&(de_cvar_desc){.name = "r.rt.restir_di_initial_visibility",
                                         .help = "Test initial selected sample visibility",
                                         .type = DE_CVAR_BOOL,
                                         .default_number = 0});
    s->candidates = de_cvar_register(&(de_cvar_desc){.name = "r.rt.restir_di_candidates",
                                                     .help = "Initial RIS candidates",
                                                     .type = DE_CVAR_INT,
                                                     .default_number = 32,
                                                     .min = 1,
                                                     .max = 128});
    if (!s->enable.id || !s->unbiased.id || !s->initial_visibility.id || !s->candidates.id) {
        de__restir_di_destroy(s);
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "DI cvar registration");
    }
    if (r->caps.ray_query && r->caps.max_bindless_accels) {
        const char *files[] = {"restir_di_initial", "restir_di_temporal", "restir_di_spatial",
                               "restir_di_shade"};
        const char *entries[4][4] = {
            {"cs_sources", "cs_alias", "cs_initial", "cs_initial_visibility"},
            {"cs_temporal", NULL, NULL, NULL},
            {"cs_spatial", "cs_spatial_visibility", NULL, NULL},
            {"cs_final_ray", "cs_shade", "cs_reference", NULL}};
        const u32 counts[] = {4, 1, 2, 3}, indices[] = {0, 4, 5, 7};
        char why[512] = {0};
        for (u32 f = 0; f < 4; ++f) {
            de__engine_shader_code code;
            de_result result = de__engine_shader_load(r->dev, r->alloc, files[f], 0, entries[f],
                                                      counts[f], &code, why, sizeof why);
            if (result != DE_OK) {
                de__restir_di_destroy(s);
                return DE_FAIL(result, "DI shaders: %s", why);
            }
            for (u32 j = 0; j < counts[f]; ++j) {
                const u32 k = indices[f] + j;
                s->kernels[k] = de_gpu_pipeline_create(
                    r->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                                    .cs = code.entries[j].blob,
                                                    .push_constant_bytes = 16,
                                                    .threadgroup_size = {k == K_ALIAS     ? 1
                                                                         : k == K_SOURCES ? 64
                                                                                          : 8,
                                                                         k <= K_ALIAS ? 1 : 8, 1},
                                                    .name = entries[f][j]});
            }
            de__engine_shader_code_free(&code);
        }
        for (u32 i = 0; i < K_COUNT; ++i)
            if (!s->kernels[i].id) {
                de__restir_di_destroy(s);
                return DE_FAIL(DE_ERR_UNSUPPORTED, "DI pipeline");
            }
    }
    *out = s;
    return DE_OK;
}
static de_result source_capacity(de__restir_di *s, u32 count) {
    if (count <= s->capacity)
        return DE_OK;
    u32 cap = s->capacity ? s->capacity : 64;
    while (cap < count) {
        if (cap > UINT32_MAX / 2)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "DI source count");
        cap *= 2;
    }
    di_descriptor *next = DE_ALLOC(s->r->alloc, (size_t)cap * sizeof *next, 16, DE_MEM_RENDER);
    if (!next)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "DI descriptor capacity");
    if (s->descriptors)
        DE_FREE(s->r->alloc, s->descriptors, (size_t)s->capacity * sizeof *next);
    s->descriptors = next;
    s->capacity = cap;
    de_gpu_buffer_destroy(s->r->dev, s->sources);
    de_gpu_buffer_destroy(s->r->dev, s->alias_work);
    s->sources = make_buffer(s, (u64)cap * DI_SOURCE_BYTES, DI_SOURCE_BYTES, DE_MEM_GPU_ONLY,
                             "DI light sources");
    s->alias_work = make_buffer(s, (u64)cap * 8, 0, DE_MEM_GPU_ONLY, "DI alias stacks");
    for (u32 i = 0; i < DI_RING; ++i) {
        de_gpu_buffer_destroy(s->r->dev, s->descriptor_upload[i]);
        s->descriptor_upload[i] =
            make_buffer(s, (u64)cap * 16, 16, DE_MEM_UPLOAD, "DI source descriptors");
        if (!s->descriptor_upload[i].id)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "DI descriptors");
    }
    return s->sources.id && s->alias_work.id ? DE_OK : DE_FAIL(DE_ERR_OUT_OF_MEMORY, "DI sources");
}
static u32 describe(de__restir_di *s, const de__render_world_frame *world, di_descriptor *out) {
    if (!world)
        return 0;
    u32 count = 0, emissive = 0;
    for (u32 i = 0; i < world->lights.count; ++i) {
        const DeLight *l = &world->lights.light[i];
        if (world->lights.entity[i] && l->kind > DE_LIGHT_DIRECTIONAL && l->kind <= DE_LIGHT_DISK &&
            l->intensity > 0) {
            if (out)
                out[count] = (di_descriptor){0, i, 0, 0};
            ++count;
        }
    }
    for (u32 i = 0; i < world->instances.count; ++i) {
        if (!world->instances.entity[i] || !(world->instances.flags[i] & DE__RW_INST_VISIBLE_IN_RT))
            continue;
        u32 mesh_slot;
        const u32 *materials;
        if (!de__gpu_scene_instance_slots(s->r->gpu_scene, i, &mesh_slot, &materials))
            continue;
        const de__gs_mesh *mesh = de__gpu_scene_entry(s->r->gpu_scene, DE__GS_MESHES, mesh_slot);
        if (!mesh || !(mesh->flags & DE__GS_MESH_READY))
            continue;
        const de_render_mesh *source = NULL;
        de__rt_scene *rt = s->r->rt_scene;
        for (u32 m = 0; rt && m < rt->mesh_count; ++m)
            if (rt->meshes[m].packed.vertices == mesh->vertices) {
                source = rt->meshes[m].source;
                break;
            }
        if (!source || !source->lod_submeshes)
            continue;
        for (u32 sub = 0; sub < source->submesh_count && sub < 8; ++sub) {
            const de__gs_material *material =
                de__gpu_scene_entry(s->r->gpu_scene, DE__GS_MATERIALS, materials[sub]);
            if (!material || !(material->emissive.v[3] > 0) ||
                !(material->emissive.v[0] > 0 || material->emissive.v[1] > 0 ||
                  material->emissive.v[2] > 0))
                continue;
            const de_cooked_mesh_range *range = &source->lod_submeshes[sub];
            for (u32 triangle = 0; triangle < range->index_count / 3; ++triangle) {
                if (out)
                    out[count] = (di_descriptor){1, i, range->index_first + triangle * 3, sub};
                ++count;
                ++emissive;
            }
        }
    }
    s->emissive = emissive;
    return count;
}
de_result de__restir_di_prepare(de_renderer *r) {
    de__restir_di *s = r ? r->restir_di : NULL;
    if (!s)
        return DE_OK;
    de_render_settings settings = de_renderer_get_settings(r);
    if (settings.tier != DE_TIER_RT || !s->r->caps.ray_query) {
        for (u32 i = 0; i < 32; ++i)
            s->views[i].completed = false;
        return DE_OK;
    }
    const u64 signature = de__gpu_scene_source_signature(r->gpu_scene);
    if (signature == s->signature)
        return DE_OK;
    const de__render_world_frame *world = de__render_world_last(r);
    u32 count = describe(s, world, NULL);
    DE_TRY(source_capacity(s, count));
    if (count) {
        describe(s, world, s->descriptors);
        s->descriptor_ring = (u32)(r->frame_index % DI_RING);
        memcpy(de_gpu_buffer_map(r->dev, s->descriptor_upload[s->descriptor_ring]), s->descriptors,
               (size_t)count * 16);
    }
    s->count = count;
    s->signature = signature;
    ++s->epoch;
    s->needs_alias = count != 0;
    return DE_OK;
}
static bool view_capacity(de__restir_di *s, di_view *v, u32 width, u32 height) {
    if (v->width == width && v->height == height)
        return true;
    release_view(s, v);
    const u64 count = (u64)width * height;
    if (count > UINT32_MAX / 6)
        return false;
    v->width = width;
    v->height = height;
    // Initial and spatial scratch lifetimes do not overlap: the same 32-byte/pixel allocation
    // aliases them.
    v->scratch = make_buffer(s, count * 32, 32, DE_MEM_GPU_ONLY, "DI initial/spatial scratch");
    for (u32 i = 0; i < 2; ++i) {
        v->reservoir[i] = make_buffer(s, count * 32, 32, DE_MEM_GPU_ONLY, "DI reservoir history");
        v->surface[i] = make_buffer(s, count * 32, 32, DE_MEM_GPU_ONLY, "DI surface history");
    }
    v->rays = make_buffer(s, count * 6 * 32, 32, DE_MEM_GPU_ONLY, "DI visibility rays");
    v->hits = make_buffer(s, count * 6 * 32, 32, DE_MEM_GPU_ONLY, "DI visibility hits");
    v->params = make_buffer(s, DI_RING * DI_STAGES * sizeof(di_params), sizeof(di_params),
                            DE_MEM_UPLOAD, "DI immutable parameters");
    de_gpu_texture_desc desc = {.width = width,
                                .height = height,
                                .format = DE_FMT_RGBA32_FLOAT,
                                .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                                .name = "DI noisy direct"};
    v->diffuse = de_gpu_texture_create(s->r->dev, &desc);
    v->specular = de_gpu_texture_create(s->r->dev, &desc);
    desc.format = DE_FMT_R16_FLOAT;
    desc.name = "DI hit distance";
    v->distance = de_gpu_texture_create(s->r->dev, &desc);
    return v->scratch.id && v->reservoir[0].id && v->reservoir[1].id && v->surface[0].id &&
           v->surface[1].id && v->rays.id && v->hits.id && v->params.id && v->diffuse.id &&
           v->specular.id && v->distance.id;
}
typedef struct di_resources {
    de_rg_resource depth, gb[3], motion, ids, visible, scene;
    de_rg_resource sources, work, descriptors, scratch, previous, temporal, history_previous,
        history_current, rays, hits, diffuse, specular, distance;
} di_resources;
typedef struct di_pass {
    de__restir_di *s;
    di_view *view;
    de__rt_context rt;
    di_resources resources;
    di_params params;
    u32 kernel, stage, phase;
    bool trace;
} di_pass;
static void barrier(de_gpu_cmd *c, de_gpu_buffer b, u32 before, u32 after) {
    de_gpu_barrier v = {.buffer = b, .before = before, .after = after};
    de_gpu_cmd_barriers(c, &v, 1);
}
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    di_pass *x = user;
    de__restir_di *s = x->s;
    di_view *v = x->view;
    de_gpu_device *d = s->r->dev;
    di_params p = x->params;
    if (x->kernel >= K_INITIAL) {
        p.depth = de_gpu_texture_bindless(d, de_rg_texture(pass, x->resources.depth), 0, false);
        p.gb0 = de_gpu_texture_bindless(d, de_rg_texture(pass, x->resources.gb[0]), 0, false);
        p.gb1 = de_gpu_texture_bindless(d, de_rg_texture(pass, x->resources.gb[1]), 0, false);
        p.gb2 = de_gpu_texture_bindless(d, de_rg_texture(pass, x->resources.gb[2]), 0, false);
        p.motion = de_gpu_texture_bindless(d, de_rg_texture(pass, x->resources.motion), 0, false);
        p.ids = de_gpu_buffer_bindless(d, de_rg_buffer(pass, x->resources.ids));
        p.visible = de_gpu_buffer_bindless(d, de_rg_buffer(pass, x->resources.visible));
    }
    u32 entry = (u32)(s->r->frame_index % DI_RING) * DI_STAGES + x->stage;
    memcpy((u8 *)de_gpu_buffer_map(d, v->params) + (size_t)entry * sizeof p, &p, sizeof p);
    u32 pc[4] = {de_gpu_buffer_bindless(d, v->params), entry, x->phase, 0};
    de_gpu_cmd_set_pipeline(cmd, s->kernels[x->kernel]);
    de_gpu_cmd_push_constants(cmd, pc, sizeof pc);
    if (x->kernel == K_REFERENCE) {
        const u32 gx = (p.width + 7) / 8, gy = (p.height + 7) / 8;
        de_gpu_cmd_dispatch(cmd, gx, gy, 1);
        for (u32 source = 0; source < s->count; ++source) {
            pc[2] = 1;
            pc[3] = source;
            de_gpu_cmd_push_constants(cmd, pc, sizeof pc);
            de_gpu_cmd_dispatch(cmd, gx, gy, 1);
            barrier(cmd, v->rays, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
            de__rt_trace(s->r, cmd, &x->rt, v->rays, v->hits, p.width * p.height,
                         DE__RT_CAST_SHADOWS | DE__RT_ACCEPT_FIRST_HIT);
            barrier(cmd, v->hits, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
            de_gpu_cmd_set_pipeline(cmd, s->kernels[K_REFERENCE]);
            pc[2] = 2;
            de_gpu_cmd_push_constants(cmd, pc, sizeof pc);
            de_gpu_cmd_dispatch(cmd, gx, gy, 1);
            de_gpu_barrier b[2] = {{.texture = v->diffuse,
                                    .before = DE_ACCESS_SHADER_WRITE,
                                    .after = DE_ACCESS_SHADER_WRITE},
                                   {.texture = v->specular,
                                    .before = DE_ACCESS_SHADER_WRITE,
                                    .after = DE_ACCESS_SHADER_WRITE}};
            de_gpu_cmd_barriers(cmd, b, 2);
        }
        return;
    }
    de_gpu_cmd_dispatch(cmd,
                        x->kernel == K_ALIAS     ? 1
                        : x->kernel == K_SOURCES ? (s->count + 63) / 64
                                                 : (p.width + 7) / 8,
                        x->kernel <= K_ALIAS ? 1 : (p.height + 7) / 8, 1);
    if (x->kernel == K_ALIAS)
        s->needs_alias = false;
    if (x->trace) {
        barrier(cmd, v->rays, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
        const u32 multiplier = x->kernel == K_SPATIAL_VIS ? 6 : 1;
        de__rt_trace(s->r, cmd, &x->rt, v->rays, v->hits, p.width * p.height * multiplier,
                     DE__RT_CAST_SHADOWS | DE__RT_ACCEPT_FIRST_HIT);
        barrier(cmd, v->hits, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
    }
}
static de_result add_pass(di_pass *base, de_rg *g, u32 kernel, u32 phase, bool trace, u32 *stage,
                          const char *name) {
    if (*stage >= DI_STAGES)
        return DE_FAIL(DE_ERR_INTERNAL, "DI pass parameter capacity");
    di_pass *p = de_arena_push_zero(g->arena, sizeof *p, 16);
    if (!p)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "DI graph parameters");
    *p = *base;
    p->kernel = kernel;
    p->stage = (*stage)++;
    p->phase = phase;
    p->trace = trace;
    de_rg_pass *pass = de_rg_add_pass(g, name, DE_QUEUE_GRAPHICS, execute, p);
    if (!pass)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "DI graph pass");
    di_resources *r = &p->resources;
    de_rg_pass_side_effect(pass);
    de_rg_read(pass, r->scene, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, p->rt.dependency, DE_ACCESS_SHADER_READ);
    if (kernel == K_SOURCES) {
        de_rg_read(pass, r->descriptors, DE_ACCESS_SHADER_READ);
        de_rg_write(pass, r->sources, DE_ACCESS_SHADER_WRITE);
    } else if (kernel == K_ALIAS) {
        de_rg_read(pass, r->sources, DE_ACCESS_SHADER_READ);
        de_rg_write(pass, r->sources, DE_ACCESS_SHADER_WRITE);
        de_rg_write(pass, r->work, DE_ACCESS_SHADER_WRITE);
    } else {
        de_rg_read(pass, r->sources, DE_ACCESS_SHADER_READ);
        de_rg_read(pass, r->depth, DE_ACCESS_SHADER_READ);
        for (u32 i = 0; i < 3; ++i)
            de_rg_read(pass, r->gb[i], DE_ACCESS_SHADER_READ);
        de_rg_read(pass, r->motion, DE_ACCESS_SHADER_READ);
        de_rg_read(pass, r->ids, DE_ACCESS_SHADER_READ);
        de_rg_read(pass, r->visible, DE_ACCESS_SHADER_READ);
        switch (kernel) {
        case K_INITIAL:
            de_rg_write(pass, r->scratch, DE_ACCESS_SHADER_WRITE);
            de_rg_write(pass, r->history_current, DE_ACCESS_SHADER_WRITE);
            break;
        case K_INITIAL_VIS:
            de_rg_read(pass, r->scratch, DE_ACCESS_SHADER_READ);
            if (phase)
                de_rg_write(pass, r->scratch, DE_ACCESS_SHADER_WRITE);
            break;
        case K_TEMPORAL:
            de_rg_read(pass, r->scratch, DE_ACCESS_SHADER_READ);
            de_rg_read(pass, r->previous, DE_ACCESS_SHADER_READ);
            de_rg_read(pass, r->history_previous, DE_ACCESS_SHADER_READ);
            de_rg_write(pass, r->temporal, DE_ACCESS_SHADER_WRITE);
            break;
        case K_SPATIAL:
            de_rg_read(pass, r->temporal, DE_ACCESS_SHADER_READ);
            de_rg_read(pass, r->history_current, DE_ACCESS_SHADER_READ);
            de_rg_write(pass, r->scratch, DE_ACCESS_SHADER_WRITE);
            break;
        case K_SPATIAL_VIS:
            de_rg_read(pass, r->scratch, DE_ACCESS_SHADER_READ);
            de_rg_read(pass, r->temporal, DE_ACCESS_SHADER_READ);
            de_rg_read(pass, r->history_current, DE_ACCESS_SHADER_READ);
            if (phase)
                de_rg_write(pass, r->scratch, DE_ACCESS_SHADER_WRITE);
            break;
        case K_FINAL_RAY:
            de_rg_read(pass, r->scratch, DE_ACCESS_SHADER_READ);
            break;
        case K_SHADE:
            de_rg_read(pass, r->scratch, DE_ACCESS_SHADER_READ);
            de_rg_write(pass, r->diffuse, DE_ACCESS_SHADER_WRITE);
            de_rg_write(pass, r->specular, DE_ACCESS_SHADER_WRITE);
            de_rg_write(pass, r->distance, DE_ACCESS_SHADER_WRITE);
            break;
        case K_REFERENCE:
            de_rg_write(pass, r->diffuse, DE_ACCESS_SHADER_WRITE);
            de_rg_write(pass, r->specular, DE_ACCESS_SHADER_WRITE);
            de_rg_write(pass, r->distance, DE_ACCESS_SHADER_WRITE);
            break;
        default:
            break;
        }
        if (trace || kernel == K_REFERENCE) {
            de_rg_write(pass, r->rays, DE_ACCESS_SHADER_WRITE);
            de_rg_write(pass, r->hits, DE_ACCESS_SHADER_WRITE);
        } else if (kernel == K_SHADE ||
                   ((kernel == K_INITIAL_VIS || kernel == K_SPATIAL_VIS) && phase))
            de_rg_read(pass, r->hits, DE_ACCESS_SHADER_READ);
    }
    return DE_OK;
}
de_result de__restir_di_build(de__restir_di *s, de_rg *g, de__vis_context *vc,
                              de__lighting_context *lc, de__lighting_inputs *in) {
    if (!s || !vc || !vc->settings)
        return DE_OK;
    u32 slot = (vc->view.id & 255u) - 1u;
    if (slot >= 32)
        return DE_OK;
    di_view *v = &s->views[slot];
    v->info = (de__di_info){0};
    if (!enabled(s, vc->settings) || !s->count) {
        v->completed = false;
        return DE_OK;
    }
    de__rt_context *rt = de__rt_context_get(s->r, vc->view, g, vc->frame_index);
    if (!rt)
        return DE_OK;
    if (!view_capacity(s, v, vc->width, vc->height))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "DI view capacity");
    const bool unbiased = de_cvar_bool(s->unbiased),
               initial = unbiased || de_cvar_bool(s->initial_visibility);
    u32 candidates = (u32)de_cvar_int(s->candidates);
    if (!candidates)
        candidates = 32;
    const u32 mode = (unbiased ? 1 : 0) | (initial ? 2 : 0) | (s->debug.reference ? 4 : 0) |
                     (candidates << 8) | ((u32)vc->settings->shadows << 3) |
                     (vc->settings->rt_shadows ? 64u : 0u) | ((u32)vc->settings->tier << 16);
    bool history = v->completed && v->view.id == vc->view.id && v->epoch == s->epoch &&
                   v->last_frame + 1 == vc->frame_index && v->mode == mode && !vc->view_new;
    de_vec3 delta = de_v3_sub(v->camera, vc->camera_position);
    if (de_v3_dot(delta, delta) > 1)
        history = false;
    for (u32 i = 0; i < 16; ++i) {
        if (i % 4 != 3 && fabsf(v->matrix.m[i] - vc->view_matrix.m[i]) > .5f)
            history = false;
        if (fabsf(v->projection.m[i] - vc->proj.m[i]) > .01f)
            history = false;
    }
    v->view = vc->view;
    v->mode = mode;
    v->camera = vc->camera_position;
    v->matrix = vc->view_matrix;
    v->projection = vc->proj;
    u32 current = (u32)(vc->frame_index & 1), previous = current ^ 1;
    de_gpu_device *d = s->r->dev;
    di_resources r = {
        .depth = vc->depth,
        .gb = {vc->gbuffer[0], vc->gbuffer[1], vc->gbuffer[2]},
        .motion = vc->gbuffer[DE__GB_MOTION],
        .ids = vc->vis_ids_buffer,
        .visible = vc->visible_meshlets,
        .scene = vc->scene_views,
        .sources = de_rg_import_buffer(g, s->sources, DE_ACCESS_SHADER_READ),
        .work = de_rg_import_buffer(g, s->alias_work, DE_ACCESS_SHADER_READ),
        .descriptors =
            de_rg_import_buffer(g, s->descriptor_upload[s->descriptor_ring], DE_ACCESS_SHADER_READ),
        .scratch = de_rg_import_buffer(g, v->scratch, DE_ACCESS_SHADER_READ),
        .previous = de_rg_import_buffer(g, v->reservoir[previous], DE_ACCESS_SHADER_READ),
        .temporal = de_rg_import_buffer(g, v->reservoir[current], DE_ACCESS_SHADER_READ),
        .history_previous = de_rg_import_buffer(g, v->surface[previous], DE_ACCESS_SHADER_READ),
        .history_current = de_rg_import_buffer(g, v->surface[current], DE_ACCESS_SHADER_READ),
        .rays = de_rg_import_buffer(g, v->rays, DE_ACCESS_SHADER_READ),
        .hits = de_rg_import_buffer(g, v->hits, DE_ACCESS_SHADER_READ),
        .diffuse = de_rg_import_texture(g, v->diffuse, DE_ACCESS_SHADER_READ),
        .specular = de_rg_import_texture(g, v->specular, DE_ACCESS_SHADER_READ),
        .distance = de_rg_import_texture(g, v->distance, DE_ACCESS_SHADER_READ)};
    di_pass base = {.s = s,
                    .view = v,
                    .rt = *rt,
                    .resources = r,
                    .params = {.scene = vc->scene_views_bindless,
                               .scene_slot = vc->scene_view_slot,
                               .view = lc->constants_bindless,
                               .entry = lc->constants_entry,
                               .ids_pitch = vc->vis_ids_pitch,
                               .visible_count = vc->visible_capacity + vc->visible_alpha_capacity,
                               .sources = de_gpu_buffer_bindless(d, s->sources),
                               .source_count = s->count,
                               .descriptors = de_gpu_buffer_bindless(
                                   d, s->descriptor_upload[s->descriptor_ring]),
                               .alias_work = de_gpu_buffer_bindless(d, s->alias_work),
                               .initial = de_gpu_buffer_bindless(d, v->scratch),
                               .previous = de_gpu_buffer_bindless(d, v->reservoir[previous]),
                               .temporal = de_gpu_buffer_bindless(d, v->reservoir[current]),
                               .spatial = de_gpu_buffer_bindless(d, v->scratch),
                               .history_previous = de_gpu_buffer_bindless(d, v->surface[previous]),
                               .history_current = de_gpu_buffer_bindless(d, v->surface[current]),
                               .rays = de_gpu_buffer_bindless(d, v->rays),
                               .hits = de_gpu_buffer_bindless(d, v->hits),
                               .diffuse = de_gpu_texture_bindless(d, v->diffuse, 0, true),
                               .specular = de_gpu_texture_bindless(d, v->specular, 0, true),
                               .distance = de_gpu_texture_bindless(d, v->distance, 0, true),
                               .width = vc->width,
                               .height = vc->height,
                               .frame = (u32)vc->frame_index,
                               .candidates = candidates,
                               .unbiased = unbiased,
                               .initial_visibility = initial,
                               .history_valid = history,
                               .metadata = rt->metadata,
                               .layers = vc->layer_mask,
                               .sampler = de_gpu_sampler_bindless(d, s->r->rt_scene->sampler)}};
    u32 stage = 0;
    if (s->needs_alias) {
        DE_TRY(add_pass(&base, g, K_SOURCES, 0, false, &stage, "DI.SourcePower"));
        DE_TRY(add_pass(&base, g, K_ALIAS, 0, false, &stage, "DI.Alias"));
    }
    if (s->debug.reference)
        DE_TRY(add_pass(&base, g, K_REFERENCE, 0, false, &stage, "DI.Reference.AllShadowed"));
    else {
        DE_TRY(add_pass(&base, g, K_INITIAL, 0, false, &stage, "DI.Initial32"));
        if (initial) {
            DE_TRY(add_pass(&base, g, K_INITIAL_VIS, 0, true, &stage, "DI.InitialVisibility"));
            DE_TRY(add_pass(&base, g, K_INITIAL_VIS, 1, false, &stage, "DI.InitialVisibleResolve"));
        }
        DE_TRY(add_pass(&base, g, K_TEMPORAL, 0, false, &stage, "DI.Temporal"));
        DE_TRY(add_pass(&base, g, K_SPATIAL, 0, false, &stage, "DI.Spatial5"));
        if (unbiased) {
            DE_TRY(add_pass(&base, g, K_SPATIAL_VIS, 0, true, &stage, "DI.SpatialVisibility"));
            DE_TRY(
                add_pass(&base, g, K_SPATIAL_VIS, 1, false, &stage, "DI.SpatialUnbiasedResolve"));
        }
        DE_TRY(add_pass(&base, g, K_FINAL_RAY, 0, true, &stage, "DI.FinalVisibility"));
        DE_TRY(add_pass(&base, g, K_SHADE, 0, false, &stage, "DI.Shade"));
    }
    const de_rg_resource buffers[] = {r.sources,         r.work,     r.scratch,
                                      r.previous,        r.temporal, r.history_previous,
                                      r.history_current, r.rays,     r.hits};
    for (u32 i = 0; i < sizeof buffers / sizeof buffers[0]; ++i)
        de_rg_export(g, buffers[i], DE_ACCESS_SHADER_READ);
    de_rg_export(g, r.diffuse, DE_ACCESS_SHADER_READ);
    de_rg_export(g, r.specular, DE_ACCESS_SHADER_READ);
    de_rg_export(g, r.distance, DE_ACCESS_SHADER_READ);
    in->restir_diffuse = r.diffuse;
    in->restir_specular = r.specular;
    in->restir_distance = r.distance;
    lc->restir_diffuse = r.diffuse;
    lc->restir_specular = r.specular;
    lc->restir_distance = r.distance;
    const u64 pixels = (u64)vc->width * vc->height;
    s->r->stats.rays_traced +=
        pixels * (s->debug.reference ? s->count : 1u + (initial ? 1u : 0u) + (unbiased ? 6u : 0u));
    v->pending = g;
    v->pending_frame = vc->frame_index;
    v->pending_alias = s->needs_alias;
    v->info = (de__di_info){v->diffuse,  v->specular,
                            v->distance, v->reservoir[current],
                            s->sources,  vc->width,
                            vc->height,  s->count,
                            s->emissive, s->alias_builds + (s->needs_alias ? 1 : 0),
                            s->epoch,    history};
    return DE_OK;
}
void de__restir_di_complete(de__restir_di *s, de_view view, const de_rg *g, u64 frame,
                            bool success) {
    if (!s)
        return;
    u32 slot = (view.id & 255u) - 1u;
    if (slot >= 32)
        return;
    di_view *v = &s->views[slot];
    if (v->pending != g || v->pending_frame != frame)
        return;
    v->pending = NULL;
    v->completed = success && !s->debug.reference;
    if (success) {
        v->epoch = s->epoch;
        v->last_frame = frame;
        if (v->pending_alias) {
            s->needs_alias = false;
            ++s->alias_builds;
        }
    }
}
void de__restir_di_debug(de__restir_di *s, const de__di_debug *debug) {
    if (s)
        s->debug = debug ? *debug : (de__di_debug){0};
}
bool de__restir_di_get(de__restir_di *s, de_view view, de__di_info *out) {
    if (!s || !out)
        return false;
    u32 slot = (view.id & 255u) - 1u;
    if (slot >= 32 || s->views[slot].view.id != view.id || !s->views[slot].info.width)
        return false;
    *out = s->views[slot].info;
    return true;
}

/* DENG-72: reuse the production analytic/emissive descriptors and GPU power alias builder.
 * No G-buffer or reservoir is required. Graph ordering is shared across both consumers. */
de_result de__restir_di_sources(de__restir_di *s, de__view_frame *vf, const de__rt_context *rt,
                                de_gpu_buffer *buffer, u32 *count, de_rg_resource *dependency) {
    *buffer = s->sources;
    *count = s->count;
    *dependency = (de_rg_resource){0};
    if (!s->count)
        return DE_OK;
    de_rg *g = vf->graph;
    *dependency = de_rg_import_buffer(g, s->sources, DE_ACCESS_SHADER_READ);
    if (!s->needs_alias)
        return DE_OK;
    if (!s->source_view.params.id) {
        s->source_view.params =
            make_buffer(s, DI_RING * DI_STAGES * sizeof(di_params), sizeof(di_params),
                        DE_MEM_UPLOAD, "PT shared source parameters");
        if (!s->source_view.params.id)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "PT source parameters");
    }
    di_pass p = {
        .s = s,
        .view = &s->source_view,
        .rt = *rt,
        .resources = {.scene = de_rg_import_buffer(g, rt->scene, DE_ACCESS_SHADER_READ),
                      .sources = *dependency,
                      .work = de_rg_import_buffer(g, s->alias_work, DE_ACCESS_SHADER_READ),
                      .descriptors = de_rg_import_buffer(
                          g, s->descriptor_upload[s->descriptor_ring], DE_ACCESS_SHADER_READ)},
        .params = {.scene = de_gpu_buffer_bindless(s->r->dev, rt->scene),
                   .scene_slot = rt->scene_slot,
                   .sources = de_gpu_buffer_bindless(s->r->dev, s->sources),
                   .source_count = s->count,
                   .descriptors =
                       de_gpu_buffer_bindless(s->r->dev, s->descriptor_upload[s->descriptor_ring]),
                   .alias_work = de_gpu_buffer_bindless(s->r->dev, s->alias_work)}};
    u32 stage = 0;
    DE_TRY(add_pass(&p, g, K_SOURCES, 0, false, &stage, "PT.SourcePower"));
    DE_TRY(add_pass(&p, g, K_ALIAS, 0, false, &stage, "PT.SourceAlias"));
    return DE_OK;
}
