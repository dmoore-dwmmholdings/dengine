/*
 * gpu_scene.c: the GPU scene (SPEC §12.4; DENG-177): the tables, the entry packing, the upload
 * plan and its render-graph passes. gpu_scene.h describes the design; gpu_table.c holds one table.
 */
#include "render/scene/gpu_scene.h"

#include <dengine/de_debug.h>

#include "core/de_bitset.h"
#include "render/graph/rg_internal.h"
#include "render/loaders/render_assets.h"
#include "render/loaders/upload_ring.h"
#include "render/materials/material_instances.h"
#include "render/world/render_world.h"

#include "render/shaders/engine_shaders.h" /* DENG-421: the scatter kernels */

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/** A run of at least this many consecutive changed entries is copied straight from the ring;
 * shorter runs of instances and lights are scattered (when the scene has the scatter kernels). */
#define DE__GS_COPY_MIN_RUN 4u
/** Threads per scatter group (scatter.slang) and the most groups of one dispatch. */
#define DE__GS_SCATTER_GROUP      64u
#define DE__GS_SCATTER_MAX_GROUPS 65535u
/** The scene's own ring, when the renderer has no uploader. */
#define DE__GS_OWN_RING_DEFAULT ((u64)64 << 20)
/** How long a frame waits for an earlier frame's stage to retire when the ring is full. */
#define DE__GS_RING_WAIT_NS (1000ull * 1000000ull)

static const u32 k_default_capacity[DE__GS_TABLE_COUNT] = {1024u, 256u, 256u, 256u, 8u};
static const char *const k_table_names[DE__GS_TABLE_COUNT] = {
    "gpu scene instances", "gpu scene meshes", "gpu scene materials", "gpu scene lights",
    "gpu scene views"};
static const u32 k_strides[DE__GS_TABLE_COUNT] = {
    (u32)sizeof(de__gs_instance), (u32)sizeof(de__gs_mesh), (u32)sizeof(de__gs_material),
    (u32)sizeof(de__gs_light), (u32)sizeof(de__gs_view)};

/* ---------- Layout tables (the test layout_match) ---------- */

#define DE__GS_ROW(T, ctype, stype, name, dims)                                                    \
    {#name, #stype, (u32)offsetof(T, name), (u32)sizeof(((T *)0)->name)},

#define ROW_LOD(c, s, n, d)   DE__GS_ROW(de__gs_mesh_lod, c, s, n, d)
#define ROW_INST(c, s, n, d)  DE__GS_ROW(de__gs_instance, c, s, n, d)
#define ROW_MESH(c, s, n, d)  DE__GS_ROW(de__gs_mesh, c, s, n, d)
#define ROW_LIGHT(c, s, n, d) DE__GS_ROW(de__gs_light, c, s, n, d)
#define ROW_VIEW(c, s, n, d)  DE__GS_ROW(de__gs_view, c, s, n, d)

static const de__gs_layout_field k_lod_fields[] = {DE__GS_MESH_LOD_FIELDS(ROW_LOD)};
static const de__gs_layout_field k_inst_fields[] = {DE__GS_INSTANCE_FIELDS(ROW_INST)};
static const de__gs_layout_field k_mesh_fields[] = {DE__GS_MESH_FIELDS(ROW_MESH)};
static const de__gs_layout_field k_light_fields[] = {DE__GS_LIGHT_FIELDS(ROW_LIGHT)};
static const de__gs_layout_field k_view_fields[] = {DE__GS_VIEW_FIELDS(ROW_VIEW)};

#define LAYOUT(name, T, fields)                                                                    \
    {name, (u32)sizeof(T), fields, (u32)(sizeof fields / sizeof fields[0])}

static const de__gs_layout k_layouts[] = {
    LAYOUT("DeGpuMeshLod", de__gs_mesh_lod, k_lod_fields),
    LAYOUT("DeGpuInstance", de__gs_instance, k_inst_fields),
    LAYOUT("DeGpuMesh", de__gs_mesh, k_mesh_fields),
    LAYOUT("DeGpuLight", de__gs_light, k_light_fields),
    LAYOUT("DeSceneConstants", de__gs_view, k_view_fields),
};

const de__gs_layout *de__gs_layouts(u32 *count) {
    *count = (u32)(sizeof k_layouts / sizeof k_layouts[0]);
    return k_layouts;
}

static const de__gs_layout_const k_consts[] = {
    {"DE_GPU_NONE", DE__GS_NONE},
    {"DE_GPU_INSTANCE_LIVE", DE__GS_INSTANCE_LIVE},
    {"DE_GPU_INSTANCE_CAST_SHADOWS", DE__GS_INSTANCE_CAST_SHADOWS},
    {"DE_GPU_INSTANCE_VISIBLE_IN_RT", DE__GS_INSTANCE_VISIBLE_IN_RT},
    {"DE_GPU_INSTANCE_HAS_BOUNDS", DE__GS_INSTANCE_HAS_BOUNDS},
    {"DE_GPU_MESH_READY", DE__GS_MESH_READY},
    {"DE_GPU_MESH_MESHLETS", DE__GS_MESH_MESHLETS},
    {"DE_GPU_MATERIAL_MODEL_STANDARD", DE__GS_MATERIAL_MODEL_STANDARD},
    {"DE_GPU_MATERIAL_MODEL_UNLIT", DE__GS_MATERIAL_MODEL_UNLIT},
    {"DE_GPU_MATERIAL_MODEL_SPRITE", DE__GS_MATERIAL_MODEL_SPRITE},
    {"DE_GPU_MATERIAL_MODEL_TERRAIN", DE__GS_MATERIAL_MODEL_TERRAIN},
    {"DE_GPU_MATERIAL_MODEL_WATER", DE__GS_MATERIAL_MODEL_WATER},
    {"DE_GPU_MATERIAL_MODELS", DE__GS_MATERIAL_MODELS},
    {"DE_GPU_LIGHT_LIVE", DE__GS_LIGHT_LIVE},
    {"DE_GPU_LIGHT_CAST_SHADOWS", DE__GS_LIGHT_CAST_SHADOWS},
    {"DE_GPU_INSTANCE_MATERIALS", DE__GS_INSTANCE_MATERIALS},
    {"DE_GPU_MESH_LODS", DE__GS_MESH_LODS},
};

const de__gs_layout_const *de__gs_layout_consts(u32 *count) {
    *count = (u32)(sizeof k_consts / sizeof k_consts[0]);
    return k_consts;
}

DE_STATIC_ASSERT(DE__RW_MATERIALS == DE__GS_INSTANCE_MATERIALS, "materials per instance");
DE_STATIC_ASSERT(DE_COOKED_MESH_MAX_LODS == DE__GS_MESH_LODS, "LODs per mesh");
DE_STATIC_ASSERT(DE_COOKED_TEX_COUNT == DE__GS_MATERIAL_TEXTURES, "texture slots per material");
DE_STATIC_ASSERT(sizeof(de__gs_material) == DE__MATERIAL_PARAMS_SIZE, "material block stride");
DE_STATIC_ASSERT(DE_COOKED_STREAM_COUNT == 8, "stream_offset / stream_format hold 8 streams");

/* ---------- The scene ---------- */

/** Scratch buffers of the scatter path. */
enum { SCR_INSTANCES = 0, SCR_LIGHTS = 1, SCR_INDICES = 2, SCR_COUNT = 3 };
/** Copy targets: a table (its id) or a scratch buffer (DE__GS_TABLE_COUNT + its id). */
#define TARGET_SCRATCH(i) ((u32)DE__GS_TABLE_COUNT + (u32)(i))

typedef struct gs_scratch {
    de_gpu_buffer buf;
    u64 bytes;
    u32 stride;
    u32 state;
    const char *name;
} gs_scratch;

/** One copy_buffer from the ring. */
typedef struct gs_copy {
    u32 target;
    de_gpu_buffer dst;
    u64 src, dst_off, size;
} gs_copy;

/** One scatter dispatch. */
typedef struct gs_scatter {
    u32 table; /* DE__GS_INSTANCES or DE__GS_LIGHTS */
    u32 src_first, index_first, count;
    u64 payload_src, index_src; /* ring offsets of its first entry and slot (debug_apply) */
} gs_scatter;

/** A run of consecutive changed slots, and how much of it this frame takes. */
typedef struct gs_run {
    u32 table;
    u32 first, len;
    u32 take;
    bool scatter;
} gs_run;

/** A table that grew: the GPU copy of its old entries. */
typedef struct gs_grow {
    de_gpu_buffer src, dst;
    u64 bytes;
} gs_grow;

/** Handle -> slot (open addressing, keys are handle ids, 0 = empty). Lookups only. */
typedef struct gs_map {
    u32 *keys, *vals;
    u32 cap, count;
} gs_map;

/** One view slot. */
typedef struct gs_view {
    bool live;
    de__gpu_view_params cur, prev;
    u64 cur_frame, prev_frame; /* the scene frames they were set in (0 = never) */
    de_dvec3 cur_origin, prev_origin;
} gs_view;

struct de__gpu_scene {
    de_gpu_device *dev;
    const de_allocator *alloc;
    de_log_category log;
    de__gpu_scene_assets assets;
    de_asset_system *asset_system;

    de_render_uploader *uploader; /* the renderer's, or own_uploader */
    de_render_uploader *own_uploader;
    u64 own_ring_bytes;
    u64 budget;

    de__gpu_table tables[DE__GS_TABLE_COUNT];

    /* Instances: the mesh and material slots each one holds a reference on. */
    u32 *inst_mesh, *inst_mat;
    u32 inst_cap;
    /* Meshes and materials: handle per slot, references, handle -> slot. */
    de_mesh *mesh_handle;
    de_material *mat_handle;
    u32 *mesh_refs, *mat_refs;
    u32 mesh_cap, mat_cap;
    gs_map mesh_map, mat_map;
    /* DENG-302: material instances. `mat_dirty` (a bit per material slot) marks the blocks to
     * pack at the next update; `mesh_changed` (a bit per mesh slot) the meshes whose entry changed
     * in this update (their instances re-resolve their default materials). */
    struct de__material_instances *materials;
    const struct de__material_models *material_models;
    u64 *mat_dirty;
    u64 *mesh_changed;
    u64 mat_epoch; /* the pool's epoch at the last full re-check */
    u32 perm_count[DE__GS_MATERIAL_MODELS][DE__MATERIAL_PERMUTATIONS_MAX];
    /* Views. */
    gs_view *views;
    u32 view_cap;

    /* Scatter. */
    bool has_scatter;
    de_gpu_pipeline scatter_pipe[2];
    u32 scatter_pc_bytes;
    gs_scratch scratch[SCR_COUNT];

    /* This frame. */
    const de__render_world_frame *frame;
    u64 frame_no; /* update() calls */
    u64 rw_frame; /* the render-world frame of the last update(), 0 = none */
    de_dvec3 origin;
    bool staged;
    de__upload_stage stage;
    u8 *ring_base; /* CPU address of ring offset 0 (debug_apply) */
    bool closed_valid;
    de_gpu_fence last_fence; /* the last stage's fence and value: what a full ring waits for */
    u64 last_value;
    gs_run *runs;
    u32 run_count, run_cap;
    gs_copy *copies;
    u32 copy_count, copy_cap;
    gs_scatter scatters[2 * 64];
    u32 scatter_count;
    gs_grow grows[DE__GS_TABLE_COUNT];
    bool grown[DE__GS_TABLE_COUNT];
    bool wrote_table[DE__GS_TABLE_COUNT];
    bool wrote_scratch[SCR_COUNT];
    union {
        de__gs_instance instance;
        de__gs_mesh mesh;
        de__gs_material material;
        de__gs_light light;
        de__gs_view view;
    } entry_tmp; /* one packed entry */
    bool warned_ring;

    /* The renderer's integration (DENG-176 views). */
    de_rg *graph;         /* the scene's own graph (de__gpu_scene_frame) */
    bool views_in_graphs; /* desc.views_in_view_graphs */
    bool view_staged;     /* this frame's view stage is reserved */
    de__upload_stage view_stage;
    struct gs_view_copy *view_copies; /* per view slot: its copy this frame */
    u32 view_copy_cap;
    u64 view_bytes; /* bytes the view passes of this frame staged */

    de__gpu_scene_stats stats;
};

/** One view's scene-constants copy in its view graph (de__gpu_scene_view_pass). */
typedef struct gs_view_copy {
    const struct de__gpu_scene *gs;
    u64 src; /* ring offset */
    u32 slot;
} gs_view_copy;

/* ---------- Small helpers ---------- */

static bool grow_u32(const de_allocator *a, u32 **p, u32 old_n, u32 new_n, u32 fill) {
    u32 *n = (u32 *)DE_ALLOC(a, (size_t)new_n * sizeof(u32), 16, DE_MEM_RENDER);
    if (n == NULL)
        return false;
    if (*p != NULL) {
        memcpy(n, *p, (size_t)old_n * sizeof(u32));
        DE_FREE(a, *p, (size_t)old_n * sizeof(u32));
    }
    for (u32 i = old_n; i < new_n; ++i)
        n[i] = fill;
    *p = n;
    return true;
}

static bool grow_bytes(const de_allocator *a, void **p, size_t old_b, size_t new_b) {
    void *n = DE_ALLOC(a, new_b, 16, DE_MEM_RENDER);
    if (n == NULL)
        return false;
    memset(n, 0, new_b);
    if (*p != NULL) {
        memcpy(n, *p, old_b);
        DE_FREE(a, *p, old_b);
    }
    *p = n;
    return true;
}

/* Grows a plan array to hold `need` elements (doubling; only while the scene grows). */
static bool reserve_array(const de_allocator *a, void **p, u32 *cap, u32 need, size_t elem) {
    if (need <= *cap)
        return true;
    u32 n = *cap ? *cap : 64u;
    while (n < need)
        n *= 2u;
    if (!grow_bytes(a, p, (size_t)*cap * elem, (size_t)n * elem))
        return false;
    *cap = n;
    return true;
}

static void f4(de__gs_float4 *o, f32 x, f32 y, f32 z, f32 w) {
    o->v[0] = x;
    o->v[1] = y;
    o->v[2] = z;
    o->v[3] = w;
}

/* Rows of a column-major 3x4 affine. */
static void affine_rows(de__gs_float4 rows[3], const de_affine *a) {
    for (u32 r = 0; r < 3; ++r)
        f4(&rows[r], a->m[r], a->m[3 + r], a->m[6 + r], a->m[9 + r]);
}

/* Rows of a column-major 4x4. */
static void mat4_rows(de__gs_float4 rows[4], const de_mat4 *m) {
    for (u32 r = 0; r < 4; ++r)
        f4(&rows[r], m->m[r], m->m[4 + r], m->m[8 + r], m->m[12 + r]);
}

/* ---------- Handle maps ---------- */

static u32 map_hash(u32 k) {
    k *= 0x9E3779B1u;
    return k ^ (k >> 15);
}

static u32 map_get(const gs_map *m, u32 key) {
    if (m->cap == 0)
        return DE__GS_NONE;
    const u32 mask = m->cap - 1u;
    for (u32 i = map_hash(key) & mask;; i = (i + 1u) & mask) {
        if (m->keys[i] == key)
            return m->vals[i];
        if (m->keys[i] == 0)
            return DE__GS_NONE;
    }
}

static void map_insert_raw(gs_map *m, u32 key, u32 val) {
    const u32 mask = m->cap - 1u;
    u32 i = map_hash(key) & mask;
    while (m->keys[i] != 0)
        i = (i + 1u) & mask;
    m->keys[i] = key;
    m->vals[i] = val;
    m->count++;
}

static bool map_put(const de_allocator *a, gs_map *m, u32 key, u32 val) {
    if ((m->count + 1u) * 2u > m->cap) {
        const u32 ncap = m->cap ? m->cap * 2u : 64u;
        u32 *keys = (u32 *)DE_ALLOC(a, (size_t)ncap * sizeof(u32), 16, DE_MEM_RENDER);
        u32 *vals = (u32 *)DE_ALLOC(a, (size_t)ncap * sizeof(u32), 16, DE_MEM_RENDER);
        if (keys == NULL || vals == NULL) {
            if (keys)
                DE_FREE(a, keys, (size_t)ncap * sizeof(u32));
            if (vals)
                DE_FREE(a, vals, (size_t)ncap * sizeof(u32));
            return false;
        }
        memset(keys, 0, (size_t)ncap * sizeof(u32));
        gs_map old = *m;
        m->keys = keys;
        m->vals = vals;
        m->cap = ncap;
        m->count = 0;
        for (u32 i = 0; i < old.cap; ++i)
            if (old.keys[i] != 0)
                map_insert_raw(m, old.keys[i], old.vals[i]);
        if (old.cap) {
            DE_FREE(a, old.keys, (size_t)old.cap * sizeof(u32));
            DE_FREE(a, old.vals, (size_t)old.cap * sizeof(u32));
        }
    }
    map_insert_raw(m, key, val);
    return true;
}

/* Removes `key` (backward-shift deletion: no tombstones). */
static void map_remove(gs_map *m, u32 key) {
    if (m->cap == 0)
        return;
    const u32 mask = m->cap - 1u;
    u32 i = map_hash(key) & mask;
    while (m->keys[i] != key) {
        if (m->keys[i] == 0)
            return;
        i = (i + 1u) & mask;
    }
    for (u32 j = i;;) {
        j = (j + 1u) & mask;
        if (m->keys[j] == 0)
            break;
        const u32 h = map_hash(m->keys[j]) & mask;
        /* Move j into the hole at i unless its home h lies cyclically in (i, j]. */
        const bool in_range = i <= j ? (h > i && h <= j) : (h > i || h <= j);
        if (!in_range) {
            m->keys[i] = m->keys[j];
            m->vals[i] = m->vals[j];
            i = j;
        }
    }
    m->keys[i] = 0;
    m->count--;
}

static void map_free(const de_allocator *a, gs_map *m) {
    if (m->cap) {
        DE_FREE(a, m->keys, (size_t)m->cap * sizeof(u32));
        DE_FREE(a, m->vals, (size_t)m->cap * sizeof(u32));
    }
    memset(m, 0, sizeof *m);
}

/* ---------- Asset records (the default resolver: the asset system) ---------- */

static const de_render_mesh *res_mesh(void *user, de_mesh m) {
    return de_render_mesh_get((de_asset_system *)user, m);
}
static const de_render_material *res_material(void *user, de_material m) {
    return de_render_material_get((de_asset_system *)user, m);
}
static u32 res_texture(void *user, de_texture t) {
    const de_render_texture *rt = de_render_texture_get((de_asset_system *)user, t);
    return rt != NULL ? rt->bindless : DE_GPU_BINDLESS_INVALID;
}
/* DENG-302: submesh `k`'s default material (main thread: a typed accessor). */
static de_material res_default_material(void *user, de_mesh mesh, u32 k) {
    de_asset_system *s = (de_asset_system *)user;
    const de_render_mesh *m = de_render_mesh_get(s, mesh);
    if (m == NULL || k >= m->submesh_count || m->submeshes[k].material.hash == 0)
        return (de_material){0};
    const de_material h = de_asset_material(s, m->submeshes[k].material);
    if (!DE_HANDLE_VALID(h))
        de_error_clear(); /* not in the manifest: the engine's default */
    return h;
}

/* ---------- Packing (one function per table: S14 edits pack_material and pack_light) ---------- */

static void pack_instance(const de__gpu_scene *gs, u32 s, de__gs_instance *o) {
    memset(o, 0, sizeof *o);
    o->mesh = DE__GS_NONE;
    for (u32 k = 0; k < 2; ++k)
        for (u32 c = 0; c < 4; ++c)
            o->materials[k].v[c] = DE__GS_NONE;
    const de__render_world_frame *f = gs->frame;
    if (f == NULL || s >= f->instances.count || f->instances.entity[s] == 0)
        return; /* a free slot */
    const de__rw_instances *in = &f->instances;
    affine_rows(o->transform, &in->transform[s]);
    affine_rows(o->prev_transform, &in->prev_transform[s]);
    u32 flags = DE__GS_INSTANCE_LIVE;
    if (in->flags[s] & DE__RW_INST_CAST_SHADOWS)
        flags |= DE__GS_INSTANCE_CAST_SHADOWS;
    if (in->flags[s] & DE__RW_INST_VISIBLE_IN_RT)
        flags |= DE__GS_INSTANCE_VISIBLE_IN_RT;
    if (in->flags[s] & DE__RW_INST_HAS_BOUNDS) {
        const de_aabb *b = &in->bounds[s];
        flags |= DE__GS_INSTANCE_HAS_BOUNDS;
        f4(&o->aabb_min, b->min.x, b->min.y, b->min.z, 0.0f);
        f4(&o->aabb_max, b->max.x, b->max.y, b->max.z, 0.0f);
        const f32 hx = 0.5f * (b->max.x - b->min.x), hy = 0.5f * (b->max.y - b->min.y),
                  hz = 0.5f * (b->max.z - b->min.z);
        f4(&o->sphere, b->min.x + hx, b->min.y + hy, b->min.z + hz,
           sqrtf(hx * hx + hy * hy + hz * hz));
    }
    o->flags = flags;
    o->layer_mask = in->layer_mask[s];
    o->mesh = gs->inst_mesh[s];
    for (u32 k = 0; k < DE__GS_INSTANCE_MATERIALS; ++k)
        o->materials[k / 4u].v[k % 4u] = gs->inst_mat[(size_t)s * DE__GS_INSTANCE_MATERIALS + k];
    o->entity_lo = (u32)in->entity[s];
    o->entity_hi = (u32)(in->entity[s] >> 32);
}

static void pack_light(const de__gpu_scene *gs, u32 s, de__gs_light *o) {
    memset(o, 0, sizeof *o);
    const de__render_world_frame *f = gs->frame;
    if (f == NULL || s >= f->lights.count || f->lights.entity[s] == 0)
        return;
    const DeLight *l = &f->lights.light[s];
    affine_rows(o->transform, &f->lights.transform[s]);
    f4(&o->color, l->color.x, l->color.y, l->color.z, l->intensity);
    f4(&o->params, l->range, l->spot_inner, l->spot_outer, l->source_radius);
    f4(&o->area, l->area_size.x, l->area_size.y, 0.0f, 0.0f);
    o->kind = (u32)l->kind;
    o->flags = DE__GS_LIGHT_LIVE | (l->cast_shadows ? DE__GS_LIGHT_CAST_SHADOWS : 0u);
    o->entity_lo = (u32)f->lights.entity[s];
    o->entity_hi = (u32)(f->lights.entity[s] >> 32);
}

void de__gpu_scene_pack_mesh_record(const de_render_mesh *m, de__gs_mesh *o) {
    memset(o, 0, sizeof *o);
    if (m == NULL)
        return; /* not loaded: no READY flag */
    o->vertices = m->bindless[DE_RENDER_MESH_VERTICES];
    o->meshlets = m->bindless[DE_RENDER_MESH_MESHLETS];
    o->meshlet_vertices = m->bindless[DE_RENDER_MESH_MESHLET_VERTICES];
    o->meshlet_triangles = m->bindless[DE_RENDER_MESH_MESHLET_TRIANGLES];
    o->indices = m->bindless[DE_RENDER_MESH_INDICES];
    o->vertex_count = m->vertex_count;
    o->index_count = m->index_count;
    o->meshlet_count = m->meshlet_count;
    o->submesh_count = m->submesh_count;
    o->lod_count = m->lod_count;
    o->flags = DE__GS_MESH_READY | (m->meshlet_count != 0 ? DE__GS_MESH_MESHLETS : 0u);
    f4(&o->position_offset, m->position_offset[0], m->position_offset[1], m->position_offset[2],
       m->lod_error);
    f4(&o->position_scale, m->position_scale[0], m->position_scale[1], m->position_scale[2],
       m->max_position_error);
    for (u32 k = 0; k < 2; ++k)
        f4(&o->uv_transform[k], m->uv_offset[k][0], m->uv_offset[k][1], m->uv_scale[k][0],
           m->uv_scale[k][1]);
    f4(&o->bounds_min, m->bounds_min[0], m->bounds_min[1], m->bounds_min[2], 0.0f);
    f4(&o->bounds_max, m->bounds_max[0], m->bounds_max[1], m->bounds_max[2], 0.0f);
    f4(&o->sphere, m->sphere[0], m->sphere[1], m->sphere[2], m->sphere[3]);
    for (u32 s = 0; s < DE_COOKED_STREAM_COUNT; ++s) {
        o->stream_offset[s / 4u].v[s % 4u] = m->stream_offset[s];
        o->stream_format[s / 4u].v[s % 4u] = m->stream_format[s];
    }
    const u32 lods = m->lod_count < DE__GS_MESH_LODS ? m->lod_count : DE__GS_MESH_LODS;
    for (u32 l = 0; l < lods; ++l) {
        const de_cooked_mesh_lod *src = &m->lods[l];
        de__gs_mesh_lod *d = &o->lods[l];
        d->index_first = src->index_first;
        d->index_count = src->index_count;
        d->meshlet_first = src->meshlet_first;
        d->meshlet_count = src->meshlet_count;
        d->triangle_count = src->triangle_count;
        d->vertex_count = src->vertex_count;
        d->error = src->error;
    }
}

static void pack_mesh(const de__gpu_scene *gs, de_mesh h, de__gs_mesh *o) {
    const de_render_mesh *m =
        gs->assets.mesh != NULL && DE_HANDLE_VALID(h) ? gs->assets.mesh(gs->assets.user, h) : NULL;
    de__gpu_scene_pack_mesh_record(m, o);
}

/* The material's parameter block (DENG-302: render/materials/material_instances.h). */
static void pack_material(const de__gpu_scene *gs, de_material h, de__gs_material *o) {
    const de_render_material *m = NULL;
    if (DE_HANDLE_VALID(h)) {
        if (gs->materials != NULL)
            m = de__material_instances_get(gs->materials, h);
        else if (gs->assets.material != NULL)
            m = gs->assets.material(gs->assets.user, h);
    }
    de__material_pack(m, gs->material_models, gs->assets.texture_bindless, gs->assets.user, o);
}

/* prev_view moved from its origin onto `origin` (the camera did not move; the numbers did). */
static de_mat4 rebase_view(const de_mat4 *v, de_dvec3 from, de_dvec3 to) {
    de_mat4 r = *v;
    const f64 dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    if (dx == 0.0 && dy == 0.0 && dz == 0.0)
        return r;
    for (u32 row = 0; row < 4; ++row)
        r.m[12 + row] = (f32)((f64)v->m[row] * dx + (f64)v->m[4 + row] * dy +
                              (f64)v->m[8 + row] * dz + (f64)v->m[12 + row]);
    return r;
}

static void pack_view(const de__gpu_scene *gs, u32 slot, de__gs_view *o) {
    memset(o, 0, sizeof *o);
    if (slot >= gs->view_cap || !gs->views[slot].live || gs->views[slot].cur_frame == 0)
        return;
    const gs_view *v = &gs->views[slot];
    const de_mat4 vp = de_m4_mul(&v->cur.proj, &v->cur.view);
    const de_mat4 inv = de_m4_inverse(&vp);
    de_mat4 prev_vp = vp;
    if (v->prev_frame != 0 && v->prev_frame + 1u == v->cur_frame) {
        const de_mat4 pv = rebase_view(&v->prev.view, v->prev_origin, v->cur_origin);
        prev_vp = de_m4_mul(&v->prev.proj, &pv);
    }
    mat4_rows(o->view, &v->cur.view);
    mat4_rows(o->proj, &v->cur.proj);
    mat4_rows(o->view_proj, &vp);
    mat4_rows(o->inv_view_proj, &inv);
    mat4_rows(o->prev_view_proj, &prev_vp);
    f4(&o->camera_position, v->cur.camera_position.x, v->cur.camera_position.y,
       v->cur.camera_position.z, 0.0f);
    const de_dvec3 org = v->cur_origin;
    const f32 hx = (f32)org.x, hy = (f32)org.y, hz = (f32)org.z;
    f4(&o->origin_high, hx, hy, hz, 0.0f);
    f4(&o->origin_low, (f32)(org.x - (f64)hx), (f32)(org.y - (f64)hy), (f32)(org.z - (f64)hz),
       0.0f);
    const f32 w = (f32)v->cur.width, h = (f32)v->cur.height;
    f4(&o->viewport, w, h, w > 0.0f ? 1.0f / w : 0.0f, h > 0.0f ? 1.0f / h : 0.0f);
    o->instances = gs->tables[DE__GS_INSTANCES].bindless;
    o->meshes = gs->tables[DE__GS_MESHES].bindless;
    o->materials = gs->tables[DE__GS_MATERIALS].bindless;
    o->lights = gs->tables[DE__GS_LIGHTS].bindless;
    o->instance_count = gs->tables[DE__GS_INSTANCES].count;
    o->mesh_count = gs->tables[DE__GS_MESHES].count;
    o->material_count = gs->tables[DE__GS_MATERIALS].count;
    o->light_count = gs->tables[DE__GS_LIGHTS].count;
    o->view_slot = slot;
    o->flags = v->cur.flags;
    o->pre_exposure = v->cur.pre_exposure;           /* DENG-304 */
    o->prev_pre_exposure = v->cur.prev_pre_exposure; /* DENG-304 */
}

/* Entry `slot` of table `t` as this frame uploads it. */
static void pack_entry(const de__gpu_scene *gs, de__gs_table_id t, u32 slot, void *out) {
    const de__gpu_table *tb = &gs->tables[t];
    switch (t) {
    case DE__GS_INSTANCES:
        pack_instance(gs, slot, (de__gs_instance *)out);
        break;
    case DE__GS_LIGHTS:
        pack_light(gs, slot, (de__gs_light *)out);
        break;
    default: /* meshes, materials, views: their CPU copy is current */
        memcpy(out, tb->shadow + (size_t)slot * tb->stride, tb->stride);
        break;
    }
}

/* ---------- Mesh and material references ---------- */

static de_result ensure_slot_arrays(de__gpu_scene *gs) {
    const de__gpu_table *mt = &gs->tables[DE__GS_MESHES];
    if (mt->capacity > gs->mesh_cap) {
        if (!grow_u32(gs->alloc, &gs->mesh_refs, gs->mesh_cap, mt->capacity, 0) ||
            !grow_bytes(gs->alloc, (void **)&gs->mesh_handle,
                        (size_t)gs->mesh_cap * sizeof(de_mesh),
                        (size_t)mt->capacity * sizeof(de_mesh)))
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "gpu scene: mesh slots");
        if (!grow_bytes(gs->alloc, (void **)&gs->mesh_changed,
                        (size_t)((gs->mesh_cap + 63u) / 64u) * sizeof(u64),
                        (size_t)((mt->capacity + 63u) / 64u) * sizeof(u64)))
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "gpu scene: mesh slots");
        gs->mesh_cap = mt->capacity;
    }
    const de__gpu_table *at = &gs->tables[DE__GS_MATERIALS];
    if (at->capacity > gs->mat_cap) {
        if (!grow_u32(gs->alloc, &gs->mat_refs, gs->mat_cap, at->capacity, 0) ||
            !grow_bytes(gs->alloc, (void **)&gs->mat_handle,
                        (size_t)gs->mat_cap * sizeof(de_material),
                        (size_t)at->capacity * sizeof(de_material)))
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "gpu scene: material slots");
        if (!grow_bytes(gs->alloc, (void **)&gs->mat_dirty,
                        (size_t)((gs->mat_cap + 63u) / 64u) * sizeof(u64),
                        (size_t)((at->capacity + 63u) / 64u) * sizeof(u64)))
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "gpu scene: material slots");
        gs->mat_cap = at->capacity;
    }
    return DE_OK;
}

/* DENG-302: counts a READY block (`delta` +1 or -1) under its model's resolve permutation. */
static void perm_count_add(de__gpu_scene *gs, const de__gs_material *e, i32 delta) {
    if ((e->flags & DE__GS_MATERIAL_READY) == 0 || e->model >= DE__GS_MATERIAL_MODELS)
        return;
    const u32 p = de__material_permutation_of(e->model, e->features);
    u32 *c = &gs->perm_count[e->model][p < DE__MATERIAL_PERMUTATIONS_MAX ? p : 0u];
    if (delta > 0)
        ++*c;
    else if (*c != 0)
        --*c;
}

/* A reference on the slot of handle id `id` in table `t` (a new slot for a new handle). */
static u32 ref_acquire(de__gpu_scene *gs, de__gs_table_id t, u32 id) {
    gs_map *map = t == DE__GS_MESHES ? &gs->mesh_map : &gs->mat_map;
    u32 s = map_get(map, id);
    if (s != DE__GS_NONE) {
        (t == DE__GS_MESHES ? gs->mesh_refs : gs->mat_refs)[s]++;
        return s;
    }
    de__gpu_table *tb = &gs->tables[t];
    s = de__gpu_table_alloc(tb);
    if (s == DE__GS_NONE || ensure_slot_arrays(gs) != DE_OK)
        return DE__GS_NONE;
    if (!map_put(gs->alloc, map, id, s)) {
        de__gpu_table_free(tb, s);
        return DE__GS_NONE;
    }
    if (t == DE__GS_MESHES) {
        gs->mesh_refs[s] = 1;
        gs->mesh_handle[s].id = id;
    } else {
        gs->mat_refs[s] = 1;
        gs->mat_handle[s].id = id;
        gs->mat_dirty[s / 64u] |= (u64)1 << (s % 64u); /* DENG-302: pack it */
    }
    de__gpu_table_mark(tb, s); /* a new slot holds whatever the buffer had: upload it */
    return s;
}

static void ref_release(de__gpu_scene *gs, de__gs_table_id t, u32 s) {
    if (s == DE__GS_NONE)
        return;
    u32 *refs = t == DE__GS_MESHES ? gs->mesh_refs : gs->mat_refs;
    DE_ASSERT(refs[s] != 0);
    if (--refs[s] != 0)
        return;
    de__gpu_table *tb = &gs->tables[t];
    if (t == DE__GS_MESHES) {
        map_remove(&gs->mesh_map, gs->mesh_handle[s].id);
        gs->mesh_handle[s].id = 0;
    } else {
        map_remove(&gs->mat_map, gs->mat_handle[s].id);
        gs->mat_handle[s].id = 0;
        gs->mat_dirty[s / 64u] &= ~((u64)1 << (s % 64u));
        perm_count_add(
            gs, (const de__gs_material *)(const void *)(tb->shadow + (size_t)s * tb->stride), -1);
    }
    memset(tb->shadow + (size_t)s * tb->stride, 0, tb->stride);
    de__gpu_table_free(tb, s);
}

/* Resolves instance slot `s`'s mesh and material slots from this frame (new references first,
 * then the old ones go, so an unchanged handle keeps its slot). */
static void inst_resolve(de__gpu_scene *gs, u32 s) {
    const de__rw_instances *in = &gs->frame->instances;
    const bool live = s < in->count && in->entity[s] != 0;
    const u32 old_mesh = gs->inst_mesh[s];
    u32 *mats = &gs->inst_mat[(size_t)s * DE__GS_INSTANCE_MATERIALS];
    u32 old_mats[DE__GS_INSTANCE_MATERIALS];
    memcpy(old_mats, mats, sizeof old_mats);
    gs->inst_mesh[s] = live && DE_HANDLE_VALID(in->mesh[s])
                           ? ref_acquire(gs, DE__GS_MESHES, in->mesh[s].id)
                           : DE__GS_NONE;
    for (u32 k = 0; k < DE__GS_INSTANCE_MATERIALS; ++k) {
        de_material m = in->materials[(size_t)s * DE__RW_MATERIALS + k];
        /* DENG-302 (DENG-231 note): an empty slot takes the mesh's default for that submesh. */
        if (live && !DE_HANDLE_VALID(m) && DE_HANDLE_VALID(in->mesh[s]) &&
            gs->assets.default_material != NULL)
            m = gs->assets.default_material(gs->assets.user, in->mesh[s], k);
        mats[k] =
            live && DE_HANDLE_VALID(m) ? ref_acquire(gs, DE__GS_MATERIALS, m.id) : DE__GS_NONE;
    }
    ref_release(gs, DE__GS_MESHES, old_mesh);
    for (u32 k = 0; k < DE__GS_INSTANCE_MATERIALS; ++k)
        ref_release(gs, DE__GS_MATERIALS, old_mats[k]);
}

/* Packs material slot `s` and marks it when its block changed; keeps the permutation counts.
 * Returns true when the block changed. */
static bool refresh_material(de__gpu_scene *gs, u32 s) {
    de__gpu_table *at = &gs->tables[DE__GS_MATERIALS];
    de__gs_material *e = &gs->entry_tmp.material;
    pack_material(gs, gs->mat_handle[s], e);
    gs->stats.material_packs++;
    de__gs_material *sh = (de__gs_material *)(void *)(at->shadow + (size_t)s * at->stride);
    if (memcmp(sh, e, sizeof *e) == 0)
        return false;
    const bool was = (sh->flags & DE__GS_MATERIAL_READY) != 0;
    const bool is = (e->flags & DE__GS_MATERIAL_READY) != 0;
    if (was && is &&
        (sh->model != e->model || de__material_permutation_of(sh->model, sh->features) !=
                                      de__material_permutation_of(e->model, e->features)))
        gs->stats.permutation_switches++;
    perm_count_add(gs, sh, -1);
    perm_count_add(gs, e, +1);
    memcpy(sh, e, sizeof *e);
    de__gpu_table_mark(at, s);
    return true;
}

/* Packs every referenced mesh and marks the ones whose entry changed (a new slot, a mesh that
 * finished loading or reloaded); their instances re-resolve their default materials. Then the
 * material blocks: every referenced one without a material pool or after its epoch moved
 * (published, reloaded, unloaded materials and textures; a game model registered), else only the
 * marked ones (DENG-302). */
static void refresh_small_tables(de__gpu_scene *gs) {
    de__gpu_table *mt = &gs->tables[DE__GS_MESHES];
    bool meshes_changed = false;
    for (u32 s = 0; s < mt->count; ++s) {
        if (gs->mesh_refs[s] == 0)
            continue;
        de__gs_mesh *e = &gs->entry_tmp.mesh;
        pack_mesh(gs, gs->mesh_handle[s], e);
        u8 *sh = mt->shadow + (size_t)s * mt->stride;
        if (memcmp(sh, e, sizeof *e) != 0) {
            memcpy(sh, e, sizeof *e);
            de__gpu_table_mark(mt, s);
            gs->mesh_changed[s / 64u] |= (u64)1 << (s % 64u);
            meshes_changed = true;
        }
    }
    /* DENG-302: a mesh loaded or reloaded: its instances take its (new) default materials. */
    if (meshes_changed && gs->assets.default_material != NULL && gs->frame != NULL) {
        de__gpu_table *it = &gs->tables[DE__GS_INSTANCES];
        const u32 n =
            gs->frame->instances.count < gs->inst_cap ? gs->frame->instances.count : gs->inst_cap;
        for (u32 i = 0; i < n; ++i) {
            const u32 ms = gs->inst_mesh[i];
            if (ms != DE__GS_NONE && (gs->mesh_changed[ms / 64u] >> (ms % 64u) & 1u) != 0u) {
                inst_resolve(gs, i);
                de__gpu_table_mark(it, i);
            }
        }
    }
    if (meshes_changed)
        memset(gs->mesh_changed, 0, (size_t)((gs->mesh_cap + 63u) / 64u) * sizeof(u64));

    de__gpu_table *at = &gs->tables[DE__GS_MATERIALS];
    gs->stats.material_packs = 0;
    gs->stats.permutation_switches = 0;
    const u64 epoch = gs->materials != NULL ? de__material_instances_epoch(gs->materials) : 0u;
    const bool all = gs->materials == NULL || epoch != gs->mat_epoch;
    gs->mat_epoch = epoch;
    const u32 words = (at->count + 63u) / 64u;
    if (all) {
        for (u32 s = 0; s < at->count; ++s)
            if (gs->mat_refs[s] != 0)
                (void)refresh_material(gs, s);
    } else {
        for (u32 w = 0; w < words; ++w) {
            u64 bits = gs->mat_dirty[w];
            while (bits != 0) {
                const u32 b = (u32)de__bitset_ctz64(bits);
                bits &= bits - 1u;
                const u32 s = w * 64u + b;
                if (s < at->count && gs->mat_refs[s] != 0)
                    (void)refresh_material(gs, s);
            }
        }
    }
    memset(gs->mat_dirty, 0, (size_t)((gs->mat_cap + 63u) / 64u) * sizeof(u64));
    /* DENG-236: READY alpha-tested materials; DENG-302: each model's permutation this frame. */
    u32 masks = 0;
    for (u32 s = 0; s < at->count; ++s) {
        const de__gs_material *e =
            (const de__gs_material *)(const void *)(at->shadow + (size_t)s * at->stride);
        masks += gs->mat_refs[s] != 0 && (e->flags & DE__GS_MATERIAL_READY) != 0 &&
                 e->alpha_mode == 1u; /* mask */
    }
    gs->stats.mask_materials = masks;
    for (u32 m = 0; m < DE__GS_MATERIAL_MODELS; ++m) {
        u32 sel = 0;
        for (u32 p = 0; p < DE__MATERIAL_PERMUTATIONS_MAX; ++p)
            if (gs->perm_count[m][p] != 0)
                sel = p;
        gs->stats.material_permutation[m] = sel;
    }
}

/* ---------- Create / destroy ---------- */

static de_result ensure_scratch(de__gpu_scene *gs, u32 i, u64 bytes);
static void stats_refresh(de__gpu_scene *gs);

static void scratch_free(de__gpu_scene *gs) {
    for (u32 i = 0; i < SCR_COUNT; ++i)
        if (DE_HANDLE_VALID(gs->scratch[i].buf))
            de_gpu_buffer_destroy(gs->dev, gs->scratch[i].buf);
}

void de__gpu_scene_destroy(de__gpu_scene *gs) {
    if (gs == NULL)
        return;
    const de_allocator *a = gs->alloc;
    if (gs->staged)
        de__uploader_stage_close(gs->uploader, gs->stage.record, (de_gpu_fence){0}, 0);
    if (gs->view_staged)
        de__uploader_stage_close(gs->uploader, gs->view_stage.record, (de_gpu_fence){0}, 0);
    if (gs->graph != NULL) {
        de__rg_reset(gs->graph);
        de__rg_destroy(gs->graph);
    }
    if (gs->view_copies != NULL)
        DE_FREE(a, gs->view_copies, (size_t)gs->view_copy_cap * sizeof(gs_view_copy));
    for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t)
        de__gpu_table_fini(&gs->tables[t]);
    scratch_free(gs);
    for (u32 i = 0; i < 2; ++i)
        if (DE_HANDLE_VALID(gs->scatter_pipe[i]))
            de_gpu_pipeline_destroy(gs->dev, gs->scatter_pipe[i]);
    if (gs->own_uploader != NULL) {
        de__uploader_fini(gs->own_uploader);
        DE_FREE(a, gs->own_uploader, sizeof *gs->own_uploader);
    }
    if (gs->inst_mesh)
        DE_FREE(a, gs->inst_mesh, (size_t)gs->inst_cap * sizeof(u32));
    if (gs->inst_mat)
        DE_FREE(a, gs->inst_mat, (size_t)gs->inst_cap * DE__GS_INSTANCE_MATERIALS * sizeof(u32));
    if (gs->mesh_refs)
        DE_FREE(a, gs->mesh_refs, (size_t)gs->mesh_cap * sizeof(u32));
    if (gs->mesh_handle)
        DE_FREE(a, gs->mesh_handle, (size_t)gs->mesh_cap * sizeof(de_mesh));
    if (gs->mat_refs)
        DE_FREE(a, gs->mat_refs, (size_t)gs->mat_cap * sizeof(u32));
    if (gs->mat_handle)
        DE_FREE(a, gs->mat_handle, (size_t)gs->mat_cap * sizeof(de_material));
    if (gs->mat_dirty)
        DE_FREE(a, gs->mat_dirty, (size_t)((gs->mat_cap + 63u) / 64u) * sizeof(u64));
    if (gs->mesh_changed)
        DE_FREE(a, gs->mesh_changed, (size_t)((gs->mesh_cap + 63u) / 64u) * sizeof(u64));
    if (gs->views)
        DE_FREE(a, gs->views, (size_t)gs->view_cap * sizeof(gs_view));
    map_free(a, &gs->mesh_map);
    map_free(a, &gs->mat_map);
    if (gs->runs)
        DE_FREE(a, gs->runs, (size_t)gs->run_cap * sizeof(gs_run));
    if (gs->copies)
        DE_FREE(a, gs->copies, (size_t)gs->copy_cap * sizeof(gs_copy));
    DE_FREE(a, gs, sizeof *gs);
}

static de_gpu_pipeline make_scatter(de__gpu_scene *gs, const de__gpu_scene_desc *d,
                                    de_gpu_shader_blob cs, const char *name) {
    return de_gpu_pipeline_create(
        gs->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                         .cs = cs,
                                         .push_constant_bytes = gs->scatter_pc_bytes,
                                         .threadgroup_size = {d->scatter_threadgroup[0],
                                                              d->scatter_threadgroup[1],
                                                              d->scatter_threadgroup[2]},
                                         .name = name});
}

/** The scatter push constants (scatter.slang ScatterParams). */
typedef struct gs_scatter_pc {
    u32 src, indices, dst, src_first, index_first, count;
} gs_scatter_pc;

/* ---------- The engine's scatter kernels ---------- */

/* Fills the scatter blobs of `d` with the engine shader "scene_scatter" (DENG-421,
 * render/shaders/engine_shaders.h: cooked in-process in Dev builds, the embedded build-time cook
 * otherwise, a placeholder on the null backend), or leaves them empty (copy-only). The code block
 * it allocated, if any, goes to `*code` / `*size` (freed after the pipelines are made). */
static void builtin_kernels(de__gpu_scene *gs, de__gpu_scene_desc *d, void **code, size_t *size) {
    const de_gpu_caps *caps = de_gpu_get_caps(gs->dev);
    char why[768] = "";
    if (caps->backend != DE_GPU_NULL && !caps->base_tier_ok) {
        snprintf(why, sizeof why, "below the Base tier: %s", caps->base_tier_reason);
    } else {
        static const char *const k_entries[2] = {"cs_scatter_instances", "cs_scatter_lights"};
        de__engine_shader_code es;
        if (de__engine_shader_load(gs->dev, gs->alloc, "scene_scatter", 0, k_entries, 2, &es, why,
                                   sizeof why) == DE_OK) {
            d->scatter_instances = es.entries[0].blob;
            d->scatter_lights = es.entries[1].blob;
            memcpy(d->scatter_threadgroup, es.entries[0].threadgroup,
                   sizeof d->scatter_threadgroup);
            if (d->scatter_threadgroup[0] == 0) { /* a placeholder without reflection */
                d->scatter_threadgroup[0] = DE__GS_SCATTER_GROUP;
                d->scatter_threadgroup[1] = d->scatter_threadgroup[2] = 1;
            }
            d->scatter_push_constant_bytes = es.push_constant_bytes;
            *code = es.block; /* NULL for embedded and placeholder code (static data) */
            *size = es.block_size;
            return;
        }
    }
    DE_WARN(gs->log,
            "gpu scene: no scatter kernels on %s (%s): changed entries are copied one "
            "run at a time",
            de_gpu_backend_name(caps->backend), why);
}

de_result de__gpu_scene_create(const de__gpu_scene_desc *d, de__gpu_scene **out) {
    if (out != NULL)
        *out = NULL;
    if (d == NULL || out == NULL || d->device == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__gpu_scene_create: %s",
                       d == NULL || out == NULL ? "NULL argument" : "no device");
    const de_allocator *a = d->allocator != NULL ? d->allocator : de_heap_allocator();
    de__gpu_scene *gs = DE_NEW(a, de__gpu_scene, DE_MEM_RENDER);
    if (gs == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__gpu_scene_create: no memory");
    memset(gs, 0, sizeof *gs);
    gs->dev = d->device;
    gs->alloc = a;
    gs->log = de_log_register_category("render", DE_LOG_INFO);
    gs->uploader = d->uploader;
    gs->own_ring_bytes = d->staging_bytes != 0 ? d->staging_bytes : DE__GS_OWN_RING_DEFAULT;
    gs->budget = d->budget_bytes;
    if (d->resolver.mesh != NULL) {
        gs->assets = d->resolver;
    } else if (d->assets != NULL) {
        gs->assets = (de__gpu_scene_assets){.mesh = res_mesh,
                                            .material = res_material,
                                            .texture_bindless = res_texture,
                                            .user = d->assets};
    }
    gs->asset_system = d->assets;
    if (d->resolver.mesh == NULL && d->assets != NULL)
        gs->assets.default_material = res_default_material; /* DENG-302 */
    gs->materials = d->materials;
    gs->material_models = d->material_models;
    gs->scratch[SCR_INSTANCES] =
        (gs_scratch){.stride = (u32)sizeof(de__gs_instance), .name = "gpu scene instance scatter"};
    gs->scratch[SCR_LIGHTS] =
        (gs_scratch){.stride = (u32)sizeof(de__gs_light), .name = "gpu scene light scatter"};
    gs->scratch[SCR_INDICES] = (gs_scratch){.stride = 4u, .name = "gpu scene scatter slots"};

    de_result r = DE_OK;
    for (u32 t = 0; t < DE__GS_TABLE_COUNT && r == DE_OK; ++t) {
        const u32 cap =
            d->initial_capacity[t] != 0 ? d->initial_capacity[t] : k_default_capacity[t];
        const bool shadow = t == DE__GS_MESHES || t == DE__GS_MATERIALS || t == DE__GS_VIEWS;
        r = de__gpu_table_init(&gs->tables[t], gs->dev, a, k_table_names[t], k_strides[t], cap,
                               shadow);
    }
    if (r == DE_OK)
        r = ensure_slot_arrays(gs);
    gs->views_in_graphs = d->views_in_view_graphs;
    if (r == DE_OK && d->views_in_view_graphs) {
        /* Imported in its resting state from the first frame on, so a view graph's first frame
         * and the next have the same key (DENG-172's cache). Its first barrier, SHADER_READ ->
         * COPY_DST on a buffer nothing has used, waits for nothing. */
        gs->tables[DE__GS_VIEWS].state = DE_ACCESS_SHADER_READ;
        gs->view_copy_cap = gs->tables[DE__GS_VIEWS].capacity;
        gs->view_copies = (gs_view_copy *)DE_ALLOC(
            a, (size_t)gs->view_copy_cap * sizeof(gs_view_copy), 16, DE_MEM_RENDER);
        if (gs->view_copies == NULL)
            r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__gpu_scene_create: view copies");
    }
    /* The engine's own kernels (the renderer): cooked in-process in Dev builds. */
    de__gpu_scene_desc dd = *d;
    d = &dd;
    void *cooked_code = NULL;
    size_t cooked_size = 0;
    if (r == DE_OK && d->builtin_scatter && d->scatter_instances.code == NULL)
        builtin_kernels(gs, &dd, &cooked_code, &cooked_size);
    if (r == DE_OK && d->scatter_instances.code != NULL && d->scatter_lights.code != NULL) {
        gs->scatter_pc_bytes = d->scatter_push_constant_bytes > (u32)sizeof(gs_scatter_pc)
                                   ? d->scatter_push_constant_bytes
                                   : (u32)sizeof(gs_scatter_pc);
        gs->scatter_pc_bytes = (gs->scatter_pc_bytes + 3u) & ~3u;
        gs->scatter_pipe[0] = make_scatter(gs, d, d->scatter_instances, "gpu scene scatter inst");
        gs->scatter_pipe[1] = make_scatter(gs, d, d->scatter_lights, "gpu scene scatter lights");
        if (!DE_HANDLE_VALID(gs->scatter_pipe[0]) || !DE_HANDLE_VALID(gs->scatter_pipe[1])) {
            const de_error *e = de_error_last();
            r = DE_FAIL_WRAP(e != NULL && e->result != DE_OK ? e->result : DE_ERR_INTERNAL,
                             "de__gpu_scene_create: the scatter pipelines");
        }
        gs->has_scatter = r == DE_OK;
        /* The scratch buffers exist from the start, so every frame declares the same ones. */
        for (u32 i = 0; i < SCR_COUNT && r == DE_OK; ++i)
            r = ensure_scratch(gs, i, (u64)gs->scratch[i].stride * 256u);
    }
    if (cooked_code != NULL)
        DE_FREE(a, cooked_code, cooked_size); /* the pipelines keep what they need */
    if (r == DE_OK)
        r = de__rg_create(&(de__rg_desc){.allocator = a, .device = gs->dev, .name = "gpu_scene"},
                          &gs->graph);
    if (r != DE_OK) {
        de__gpu_scene_destroy(gs);
        return r;
    }
    *out = gs;
    return DE_OK;
}

/* ---------- update ---------- */

static de_result ensure_inst_arrays(de__gpu_scene *gs) {
    const u32 cap = gs->tables[DE__GS_INSTANCES].capacity;
    if (cap <= gs->inst_cap)
        return DE_OK;
    u32 *mesh = gs->inst_mesh, *mat = gs->inst_mat;
    gs->inst_mesh = NULL;
    gs->inst_mat = NULL;
    u32 *nm = NULL, *na = NULL;
    if (!grow_u32(gs->alloc, &nm, 0, cap, DE__GS_NONE) ||
        !grow_u32(gs->alloc, &na, 0, cap * DE__GS_INSTANCE_MATERIALS, DE__GS_NONE)) {
        if (nm)
            DE_FREE(gs->alloc, nm, (size_t)cap * sizeof(u32));
        gs->inst_mesh = mesh;
        gs->inst_mat = mat;
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "gpu scene: instance references");
    }
    if (mesh != NULL) {
        memcpy(nm, mesh, (size_t)gs->inst_cap * sizeof(u32));
        memcpy(na, mat, (size_t)gs->inst_cap * DE__GS_INSTANCE_MATERIALS * sizeof(u32));
        DE_FREE(gs->alloc, mesh, (size_t)gs->inst_cap * sizeof(u32));
        DE_FREE(gs->alloc, mat, (size_t)gs->inst_cap * DE__GS_INSTANCE_MATERIALS * sizeof(u32));
    }
    gs->inst_mesh = nm;
    gs->inst_mat = na;
    gs->inst_cap = cap;
    return DE_OK;
}

de_result de__gpu_scene_update(de__gpu_scene *gs, const de__render_world_frame *f) {
    if (gs == NULL || f == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__gpu_scene_update: NULL argument");
    gs->frame_no++;
    gs->stats.frames++;
    gs->frame = f;
    gs->origin = f->origin;
    /* The lists of a frame are complete only after the frame before: a first frame, or a skipped
     * one, resolves every slot. The same frame again (Extract did not run) changes nothing. */
    const bool repeat = gs->rw_frame != 0 && f->frame == gs->rw_frame;
    const bool all = gs->rw_frame == 0 || f->frame != gs->rw_frame + 1u;
    gs->rw_frame = f->frame;

    de__gpu_table *it = &gs->tables[DE__GS_INSTANCES];
    de__gpu_table *lt = &gs->tables[DE__GS_LIGHTS];
    de_result r = de__gpu_table_reserve(it, f->instances.count);
    if (r == DE_OK)
        r = ensure_inst_arrays(gs);
    if (r == DE_OK)
        r = de__gpu_table_reserve(lt, f->lights.count);
    if (r != DE_OK) {
        gs->frame = NULL; /* nothing uploads; the next frame starts over */
        gs->rw_frame = 0;
        return r;
    }
    if (!repeat) {
        if (all) {
            for (u32 s = 0; s < f->instances.count; ++s)
                inst_resolve(gs, s);
        } else {
            for (u32 i = 0; i < f->removed_instance_count; ++i)
                inst_resolve(gs, f->removed_instances[i]);
            for (u32 i = 0; i < f->dirty_instance_count; ++i)
                inst_resolve(gs, f->dirty_instances[i]);
        }
        if (all || f->origin_changed) {
            de__gpu_table_mark_all(it);
            de__gpu_table_mark_all(lt);
        } else {
            for (u32 i = 0; i < f->removed_instance_count; ++i)
                de__gpu_table_mark(it, f->removed_instances[i]);
            for (u32 i = 0; i < f->dirty_instance_count; ++i)
                de__gpu_table_mark(it, f->dirty_instances[i]);
            for (u32 i = 0; i < f->removed_light_count; ++i)
                de__gpu_table_mark(lt, f->removed_lights[i]);
            for (u32 i = 0; i < f->dirty_light_count; ++i)
                de__gpu_table_mark(lt, f->dirty_lights[i]);
        }
    }
    refresh_small_tables(gs);
    return DE_OK;
}

/* ---------- Views ---------- */

static de_result ensure_views(de__gpu_scene *gs, u32 n) {
    if (n <= gs->view_cap)
        return DE_OK;
    u32 cap = gs->view_cap ? gs->view_cap : 8u;
    while (cap < n)
        cap *= 2u;
    if (!grow_bytes(gs->alloc, (void **)&gs->views, (size_t)gs->view_cap * sizeof(gs_view),
                    (size_t)cap * sizeof(gs_view)))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "gpu scene: views");
    gs->view_cap = cap;
    return DE_OK;
}

de_result de__gpu_scene_set_view(de__gpu_scene *gs, u32 slot, const de__gpu_view_params *p) {
    if (gs == NULL || p == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__gpu_scene_set_view: NULL argument");
    de_result r = ensure_views(gs, slot + 1u);
    if (r == DE_OK)
        r = de__gpu_table_reserve(&gs->tables[DE__GS_VIEWS],
                                  slot + 1u > gs->tables[DE__GS_VIEWS].count
                                      ? slot + 1u
                                      : gs->tables[DE__GS_VIEWS].count);
    if (r != DE_OK)
        return r;
    gs_view *v = &gs->views[slot];
    if (v->live && v->cur_frame != gs->frame_no) {
        v->prev = v->cur;
        v->prev_frame = v->cur_frame;
        v->prev_origin = v->cur_origin;
    } else if (!v->live) {
        v->prev_frame = 0;
    }
    v->live = true;
    v->cur = *p;
    v->cur_frame = gs->frame_no;
    v->cur_origin = gs->origin;
    return DE_OK;
}

void de__gpu_scene_remove_view(de__gpu_scene *gs, u32 slot) {
    if (gs == NULL || slot >= gs->view_cap || !gs->views[slot].live)
        return;
    memset(&gs->views[slot], 0, sizeof gs->views[slot]);
}

/* Packs every view and marks the changed ones (after every table has its final buffer). */
static void refresh_views(de__gpu_scene *gs) {
    de__gpu_table *vt = &gs->tables[DE__GS_VIEWS];
    for (u32 s = 0; s < vt->count; ++s) {
        de__gs_view *e = &gs->entry_tmp.view;
        pack_view(gs, s, e);
        u8 *sh = vt->shadow + (size_t)s * vt->stride;
        if (memcmp(sh, e, sizeof *e) != 0) {
            memcpy(sh, e, sizeof *e);
            de__gpu_table_mark(vt, s);
        }
    }
}

/* ---------- The plan ---------- */

static bool scatter_table(const de__gpu_scene *gs, u32 t) {
    return gs->has_scatter && (t == DE__GS_INSTANCES || t == DE__GS_LIGHTS);
}

/* Collects the runs of every table (views, meshes, materials, lights, instances: small tables
 * first, so a frame over budget still updates them) and decides what this frame takes. */
static bool plan_runs(de__gpu_scene *gs, u64 budget, u64 *bytes_out, u32 scattered[2]) {
    static const u32 order[DE__GS_TABLE_COUNT] = {DE__GS_VIEWS, DE__GS_MESHES, DE__GS_MATERIALS,
                                                  DE__GS_LIGHTS, DE__GS_INSTANCES};
    gs->run_count = 0;
    u64 bytes = 0;
    bool full = false;
    scattered[0] = scattered[1] = 0;
    for (u32 oi = 0; oi < DE__GS_TABLE_COUNT; ++oi) {
        const u32 t = order[oi];
        de__gpu_table *tb = &gs->tables[t];
        if (!tb->all_dirty && tb->dirty_count == 0)
            continue;
        const u32 most = tb->all_dirty ? 1u : tb->dirty_count;
        if (!reserve_array(gs->alloc, (void **)&gs->runs, &gs->run_cap, gs->run_count + most,
                           sizeof(gs_run)))
            return false;
        if (tb->all_dirty) {
            if (tb->count != 0)
                gs->runs[gs->run_count++] = (gs_run){.table = t, .first = 0, .len = tb->count};
        } else {
            de__gpu_table_sort_dirty(tb);
            for (u32 i = 0; i < tb->dirty_count;) {
                u32 j = i + 1u;
                while (j < tb->dirty_count && tb->dirty[j] == tb->dirty[j - 1u] + 1u)
                    ++j;
                gs->runs[gs->run_count++] =
                    (gs_run){.table = t, .first = tb->dirty[i], .len = j - i};
                i = j;
            }
        }
    }
    for (u32 i = 0; i < gs->run_count; ++i) {
        gs_run *rn = &gs->runs[i];
        const u32 stride = gs->tables[rn->table].stride;
        rn->scatter = scatter_table(gs, rn->table) && rn->len < DE__GS_COPY_MIN_RUN;
        const u64 per = (u64)stride + (rn->scatter ? 4u : 0u);
        u64 fit = full || bytes >= budget ? 0 : (budget - bytes) / per;
        rn->take = fit >= rn->len ? rn->len : (u32)fit;
        if (rn->take < rn->len)
            full = true;
        bytes += per * rn->take;
        if (rn->scatter)
            scattered[rn->table == DE__GS_LIGHTS ? 1 : 0] += rn->take;
    }
    *bytes_out = bytes;
    return true;
}

static u64 align16(u64 v) {
    return (v + 15u) & ~(u64)15u;
}

static de_result ensure_scratch(de__gpu_scene *gs, u32 i, u64 bytes) {
    gs_scratch *sc = &gs->scratch[i];
    if (bytes <= sc->bytes)
        return DE_OK;
    u64 cap = sc->bytes ? sc->bytes : (u64)sc->stride * 256u;
    while (cap < bytes)
        cap *= 2u;
    de_gpu_buffer b = de_gpu_buffer_create(
        gs->dev, &(de_gpu_buffer_desc){.size = cap,
                                       .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_DST,
                                       .memory = DE_MEM_GPU_ONLY,
                                       .stride = sc->stride,
                                       .name = sc->name});
    if (!DE_HANDLE_VALID(b)) {
        const de_error *e = de_error_last();
        return DE_FAIL_WRAP(e != NULL && e->result != DE_OK ? e->result : DE_ERR_OUT_OF_MEMORY,
                            "gpu scene: %s (%llu bytes)", sc->name, (unsigned long long)cap);
    }
    if (DE_HANDLE_VALID(sc->buf))
        de_gpu_buffer_destroy(gs->dev, sc->buf); /* deferred: earlier frames may still read it */
    sc->buf = b;
    sc->bytes = cap;
    sc->state = DE_ACCESS_NONE;
    return DE_OK;
}

static de_result ensure_uploader(de__gpu_scene *gs) {
    if (gs->uploader != NULL)
        return DE_OK;
    de_render_uploader *u = DE_NEW(gs->alloc, de_render_uploader, DE_MEM_RENDER);
    if (u == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "gpu scene: staging ring");
    memset(u, 0, sizeof *u);
    u->alloc = gs->alloc;
    u->dev = gs->dev;
    const de_result r = de__uploader_init(u, gs->own_ring_bytes, 0);
    if (r != DE_OK) {
        de__uploader_fini(u);
        DE_FREE(gs->alloc, u, sizeof *u);
        return DE_FAIL_WRAP(r, "gpu scene: staging ring");
    }
    gs->own_uploader = u;
    gs->uploader = u;
    return DE_OK;
}

/* Reserves `bytes` in the ring; when it is full, waits once for the last frame's stage. */
static de_result stage_reserve(de__gpu_scene *gs, u64 bytes) {
    de_result r = de__uploader_stage(gs->uploader, bytes, 16u, &gs->stage);
    if (r == DE_ERR_BUSY && gs->closed_valid) {
        (void)de_gpu_fence_wait(gs->dev, gs->last_fence, gs->last_value, DE__GS_RING_WAIT_NS);
        r = de__uploader_stage(gs->uploader, bytes, 16u, &gs->stage);
    }
    return r;
}

static void add_copy(de__gpu_scene *gs, u32 target, de_gpu_buffer dst, u64 src, u64 dst_off,
                     u64 size) {
    gs->copies[gs->copy_count++] =
        (gs_copy){.target = target, .dst = dst, .src = src, .dst_off = dst_off, .size = size};
}

/* Writes the accepted runs into the stage and builds the copies and scatters. */
static void plan_write(de__gpu_scene *gs, u32 scattered[2]) {
    u8 *base = gs->stage.ptr;
    const u64 ring_off = gs->stage.offset;
    const u32 ns = scattered[0] + scattered[1];
    /* Layout: [slots of the scattered entries][instance payloads][light payloads][copy runs]. */
    const u64 idx_bytes = align16((u64)ns * 4u);
    const u64 pay_off[2] = {idx_bytes, idx_bytes + (u64)scattered[0] * sizeof(de__gs_instance)};
    u64 at = pay_off[1] + (u64)scattered[1] * sizeof(de__gs_light);
    u32 *slots = (u32 *)base;
    u32 sn[2] = {0, 0};

    if (ns != 0)
        add_copy(gs, TARGET_SCRATCH(SCR_INDICES), gs->scratch[SCR_INDICES].buf, ring_off, 0,
                 (u64)ns * 4u);
    for (u32 k = 0; k < 2; ++k)
        if (scattered[k] != 0)
            add_copy(gs, TARGET_SCRATCH(k == 0 ? SCR_INSTANCES : SCR_LIGHTS),
                     gs->scratch[k == 0 ? SCR_INSTANCES : SCR_LIGHTS].buf, ring_off + pay_off[k], 0,
                     (u64)scattered[k] * (k == 0 ? sizeof(de__gs_instance) : sizeof(de__gs_light)));

    for (u32 i = 0; i < gs->run_count; ++i) {
        const gs_run *rn = &gs->runs[i];
        if (rn->take == 0)
            continue;
        de__gpu_table *tb = &gs->tables[rn->table];
        const u32 stride = tb->stride;
        gs->stats.frame_entries[rn->table] += rn->take;
        if (rn->scatter) {
            const u32 k = rn->table == DE__GS_LIGHTS ? 1u : 0u;
            for (u32 e = 0; e < rn->take; ++e) {
                const u32 slot = rn->first + e;
                slots[(k == 0 ? 0u : scattered[0]) + sn[k]] = slot;
                pack_entry(gs, (de__gs_table_id)rn->table, slot,
                           base + pay_off[k] + (u64)sn[k] * stride);
                sn[k]++;
            }
            gs->wrote_table[rn->table] = true;
            continue;
        }
        for (u32 e = 0; e < rn->take; ++e)
            pack_entry(gs, (de__gs_table_id)rn->table, rn->first + e, base + at + (u64)e * stride);
        add_copy(gs, rn->table, tb->buffer, ring_off + at, (u64)rn->first * stride,
                 (u64)rn->take * stride);
        gs->wrote_table[rn->table] = true;
        at += (u64)rn->take * stride;
    }
    /* Scatter dispatches, at most DE__GS_SCATTER_MAX_GROUPS groups each. */
    const u32 per_dispatch = DE__GS_SCATTER_GROUP * DE__GS_SCATTER_MAX_GROUPS;
    for (u32 k = 0; k < 2; ++k) {
        const u32 index_base = k == 0 ? 0u : scattered[0];
        for (u32 first = 0; first < scattered[k] && gs->scatter_count < 2u * 64u;
             first += per_dispatch) {
            const u32 n = scattered[k] - first < per_dispatch ? scattered[k] - first : per_dispatch;
            const u32 stride = k == 0 ? (u32)sizeof(de__gs_instance) : (u32)sizeof(de__gs_light);
            gs->scatters[gs->scatter_count++] =
                (gs_scatter){.table = k == 0 ? DE__GS_INSTANCES : DE__GS_LIGHTS,
                             .src_first = first,
                             .index_first = index_base + first,
                             .count = n,
                             .payload_src = ring_off + pay_off[k] + (u64)first * stride,
                             .index_src = ring_off + (u64)(index_base + first) * 4u};
        }
        if (scattered[k] != 0)
            gs->wrote_scratch[k == 0 ? SCR_INSTANCES : SCR_LIGHTS] = true;
    }
    if (ns != 0)
        gs->wrote_scratch[SCR_INDICES] = true;
}

/* ---------- Passes ---------- */

static void pass_grow(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    (void)pass;
    const de__gpu_scene *gs = (const de__gpu_scene *)user;
    for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t)
        if (gs->grown[t] && gs->grows[t].bytes != 0)
            de_gpu_cmd_copy_buffer(cmd, gs->grows[t].dst, 0, gs->grows[t].src, 0,
                                   gs->grows[t].bytes);
}

static void pass_upload(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    (void)pass;
    const de__gpu_scene *gs = (const de__gpu_scene *)user;
    for (u32 i = 0; i < gs->copy_count; ++i) {
        const gs_copy *c = &gs->copies[i];
        de_gpu_cmd_copy_buffer(cmd, c->dst, c->dst_off, gs->stage.ring, c->src, c->size);
    }
}

static void pass_scatter(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    (void)pass;
    const de__gpu_scene *gs = (const de__gpu_scene *)user;
    const u32 idx = de_gpu_buffer_bindless(gs->dev, gs->scratch[SCR_INDICES].buf);
    for (u32 i = 0; i < gs->scatter_count; ++i) {
        const gs_scatter *s = &gs->scatters[i];
        const u32 k = s->table == DE__GS_LIGHTS ? 1u : 0u;
        const gs_scratch *sc = &gs->scratch[k == 0 ? SCR_INSTANCES : SCR_LIGHTS];
        u8 pc[128];
        memset(pc, 0, sizeof pc);
        const gs_scatter_pc p = {.src = de_gpu_buffer_bindless(gs->dev, sc->buf),
                                 .indices = idx,
                                 .dst = gs->tables[s->table].bindless,
                                 .src_first = s->src_first,
                                 .index_first = s->index_first,
                                 .count = s->count};
        memcpy(pc, &p, sizeof p);
        de_gpu_cmd_set_pipeline(cmd, gs->scatter_pipe[k]);
        de_gpu_cmd_push_constants(cmd, pc, gs->scatter_pc_bytes);
        de_gpu_cmd_dispatch(cmd, (s->count + DE__GS_SCATTER_GROUP - 1u) / DE__GS_SCATTER_GROUP, 1,
                            1);
    }
}

de_result de__gpu_scene_add_passes(de__gpu_scene *gs, de_rg *g) {
    if (gs == NULL || g == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__gpu_scene_add_passes: NULL argument");
    for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t) {
        gs->stats.frame_entries[t] = 0;
        gs->wrote_table[t] = false;
        gs->grown[t] = false;
    }
    for (u32 i = 0; i < SCR_COUNT; ++i)
        gs->wrote_scratch[i] = false;
    gs->copy_count = 0;
    gs->scatter_count = 0;
    gs->run_count = 0;
    gs->stats.frame_bytes = 0;
    gs->stats.frame_copies = 0;
    gs->stats.frame_scattered = 0;
    gs->stats.frame_dispatches = 0;
    gs->stats.frame_passes = 0;
    if (gs->frame == NULL || gs->staged)
        return DE_OK; /* no update() this frame, or passes were added already */

    if (!gs->views_in_graphs)
        refresh_views(gs);
    de_result r = DE_OK;
    u64 bytes = 0;
    u32 scattered[2] = {0, 0};
    /* The budget: the desc's, else half the ring (unbounded until the scene knows its ring). */
    const u64 budget = gs->budget != 0        ? gs->budget
                       : gs->uploader != NULL ? gs->uploader->ring_size / 2u
                                              : UINT64_MAX;
    if (!plan_runs(gs, budget, &bytes, scattered))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "gpu scene: upload plan");
    bool deferred = false;
    if (bytes != 0) {
        r = ensure_uploader(gs);
        if (r == DE_OK && budget == UINT64_MAX &&
            !plan_runs(gs, gs->uploader->ring_size / 2u, &bytes, scattered))
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "gpu scene: upload plan");
        if (r == DE_OK)
            r = ensure_scratch(gs, SCR_INDICES, (u64)(scattered[0] + scattered[1]) * 4u);
        if (r == DE_OK)
            r = ensure_scratch(gs, SCR_INSTANCES, (u64)scattered[0] * sizeof(de__gs_instance));
        if (r == DE_OK)
            r = ensure_scratch(gs, SCR_LIGHTS, (u64)scattered[1] * sizeof(de__gs_light));
        if (r != DE_OK)
            return r;
        const u32 most_copies = gs->run_count + 3u;
        if (!reserve_array(gs->alloc, (void **)&gs->copies, &gs->copy_cap, most_copies,
                           sizeof(gs_copy)))
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "gpu scene: upload plan");
        const u64 total = align16((u64)(scattered[0] + scattered[1]) * 4u) + bytes;
        r = stage_reserve(gs, total);
        if (r == DE_OK) {
            gs->staged = true;
            gs->ring_base = gs->stage.ptr - gs->stage.offset;
            plan_write(gs, scattered);
            gs->stats.frame_bytes = bytes;
            gs->stats.frame_scattered = scattered[0] + scattered[1];
            gs->stats.frame_copies = gs->copy_count;
            gs->stats.frame_dispatches = gs->scatter_count;
            /* What was taken is staged; what did not fit stays marked. */
            for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t)
                de__gpu_table_clear_dirty(&gs->tables[t]);
            for (u32 i = 0; i < gs->run_count; ++i) {
                const gs_run *rn = &gs->runs[i];
                for (u32 e = rn->take; e < rn->len; ++e)
                    de__gpu_table_mark(&gs->tables[rn->table], rn->first + e);
                deferred |= rn->take < rn->len;
            }
        } else {
            /* The ring stays full (or the frame is larger than it): everything waits. */
            if (!gs->warned_ring) {
                DE_WARN(gs->log,
                        "gpu scene: no room for %llu bytes in the %llu-byte upload ring; "
                        "the changes wait (%s)",
                        (unsigned long long)total, (unsigned long long)gs->uploader->ring_size,
                        de_result_str(r));
                gs->warned_ring = true;
            }
            deferred = true;
            r = DE_OK;
        }
    }
    if (deferred)
        gs->stats.deferred_frames++;

    /* Growths copy even when nothing else could be staged. */
    bool any_grow = false;
    for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t) {
        de__gpu_table *tb = &gs->tables[t];
        if (!DE_HANDLE_VALID(tb->old))
            continue;
        gs->grown[t] = true;
        gs->grows[t] =
            (gs_grow){.src = tb->old, .dst = tb->buffer, .bytes = (u64)tb->old_count * tb->stride};
        any_grow = true;
    }
    if (!any_grow && gs->copy_count == 0)
        return DE_OK;

    /*
     * The declarations do not depend on which tables changed: every pass declares every table
     * (and the scatter pass every scratch buffer), so a frame that uploads anything has one of
     * three pass sets (upload [+ scatter], or grow + upload [+ scatter]) and the compiled graph
     * comes from the cache (DENG-172) without a heap allocation. Barriers of the tables that did
     * not change are the price.
     */
    de_rg_resource tab[DE__GS_TABLE_COUNT];
    for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t) {
        tab[t] = de_rg_import_buffer(g, gs->tables[t].buffer, gs->tables[t].state);
        de_rg_set_name(g, tab[t], k_table_names[t]);
        gs->wrote_table[t] = true;
    }
    de_rg_resource scr[SCR_COUNT];
    for (u32 i = 0; i < SCR_COUNT; ++i) {
        scr[i] = (de_rg_resource){0};
        if (gs->has_scatter) {
            scr[i] = de_rg_import_buffer(g, gs->scratch[i].buf, gs->scratch[i].state);
            gs->wrote_scratch[i] = true;
        }
    }
    if (any_grow) {
        de_rg_pass *p = de_rg_add_pass(g, "gpu_scene.grow", DE_QUEUE_GRAPHICS, pass_grow, gs);
        de_rg_pass_side_effect(p);
        for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t) {
            if (!gs->grown[t])
                continue;
            const de_rg_resource old =
                de_rg_import_buffer(g, gs->tables[t].old, gs->tables[t].old_state);
            de_rg_read(p, old, DE_ACCESS_COPY_SRC);
            de_rg_write(p, tab[t], DE_ACCESS_COPY_DST);
        }
        gs->stats.frame_passes++;
    }
    {
        de_rg_pass *p = de_rg_add_pass(g, "gpu_scene.upload", DE_QUEUE_GRAPHICS, pass_upload, gs);
        de_rg_pass_side_effect(p);
        if (gs->staged) {
            const de_rg_resource ring = de_rg_import_buffer(g, gs->stage.ring, DE_ACCESS_COPY_SRC);
            de_rg_set_name(g, ring, "upload ring");
            de_rg_read(p, ring, DE_ACCESS_COPY_SRC);
        }
        for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t)
            de_rg_write(p, tab[t], DE_ACCESS_COPY_DST);
        for (u32 i = 0; i < SCR_COUNT; ++i)
            if (DE_HANDLE_VALID(scr[i]))
                de_rg_write(p, scr[i], DE_ACCESS_COPY_DST);
        gs->stats.frame_passes++;
    }
    if (gs->has_scatter) {
        de_rg_pass *p = de_rg_add_pass(g, "gpu_scene.scatter", DE_QUEUE_GRAPHICS, pass_scatter, gs);
        de_rg_pass_side_effect(p);
        for (u32 i = 0; i < SCR_COUNT; ++i)
            de_rg_read(p, scr[i], DE_ACCESS_SHADER_READ);
        de_rg_write(p, tab[DE__GS_INSTANCES], DE_ACCESS_SHADER_WRITE);
        de_rg_write(p, tab[DE__GS_LIGHTS], DE_ACCESS_SHADER_WRITE);
        gs->stats.frame_passes++;
    }
    for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t)
        de_rg_export(g, tab[t], DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < SCR_COUNT; ++i)
        if (DE_HANDLE_VALID(scr[i]))
            de_rg_export(g, scr[i], DE_ACCESS_SHADER_READ);
    return r;
}

/* ---------- Frame end ---------- */

void de__gpu_scene_frame_end(de__gpu_scene *gs, de_rg *g, bool submitted) {
    if (gs == NULL)
        return;
    de_gpu_fence fence = {0};
    u64 value = 0;
    if (submitted && g != NULL && !de__rg_frame_end(g, &fence, &value))
        submitted = false;
    if (gs->staged) {
        de__uploader_stage_close(gs->uploader, gs->stage.record,
                                 submitted ? fence : (de_gpu_fence){0}, value);
        if (submitted) {
            gs->closed_valid = true;
            gs->last_fence = fence;
            gs->last_value = value;
        }
        gs->staged = false;
    }
    if (submitted) {
        for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t) {
            if (gs->grown[t])
                de__gpu_table_retire_old(&gs->tables[t]);
            if (gs->grown[t] || gs->wrote_table[t])
                gs->tables[t].state = DE_ACCESS_SHADER_READ;
        }
        for (u32 i = 0; i < SCR_COUNT; ++i)
            if (gs->wrote_scratch[i])
                gs->scratch[i].state = DE_ACCESS_SHADER_READ;
        gs->stats.total_bytes += gs->stats.frame_bytes;
        DE_PROFILE_PLOT("gpu_scene upload bytes", gs->stats.frame_bytes);
    } else if (gs->copy_count != 0 || gs->scatter_count != 0) {
        /* Nothing reached the GPU: upload everything again. */
        for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t)
            de__gpu_table_mark_all(&gs->tables[t]);
        gs->stats.frame_bytes = 0;
    }
    gs->copy_count = 0;
    gs->scatter_count = 0;
    for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t) {
        gs->grown[t] = false;
        gs->wrote_table[t] = false;
    }
    for (u32 i = 0; i < SCR_COUNT; ++i)
        gs->wrote_scratch[i] = false;
    gs->frame = NULL;
    stats_refresh(gs);
}

/* ---------- The renderer's frame (DENG-176 views) ---------- */

de_result de__gpu_scene_prepare(de__gpu_scene *gs, const de__render_world_frame *f) {
    if (gs == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__gpu_scene_prepare: NULL scene");
    if (f == NULL) {
        gs->frame = NULL;
        gs->frame_no++; /* views still rotate their previous matrices */
        return DE_OK;
    }
    return de__gpu_scene_update(gs, f); /* the only place that reads asset records */
}

de_result de__gpu_scene_frame(de__gpu_scene *gs, de_arena *arena, u64 frame_index) {
    if (gs == NULL || arena == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__gpu_scene_frame: NULL argument");
    if (gs->frame == NULL)
        return DE_OK; /* nothing prepared (no render world, or the update failed) */
    de_rg *g = gs->graph;
    de_result r = de__rg_begin(g, arena);
    if (r == DE_OK)
        r = de__gpu_scene_add_passes(gs, g);
    bool executed = false;
    if (r == DE_OK && gs->stats.frame_passes != 0) {
        r = de__rg_finalize(g);
        if (r == DE_OK)
            r = de__rg_compile(g);
        if (r == DE_OK) {
            r = de__rg_execute(g, &(de__rg_execute_desc){.frame_index = frame_index});
            executed = true; /* every batch is submitted, also on an error */
        }
    }
    de__gpu_scene_frame_end(gs, g, executed);
    de__rg_reset(g);
    return r == DE_OK ? DE_OK
                      : DE_FAIL_WRAP(r, "gpu scene: frame %llu", (unsigned long long)frame_index);
}

static void pass_view(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    (void)pass;
    const gs_view_copy *c = (const gs_view_copy *)user;
    const de__gpu_table *vt = &c->gs->tables[DE__GS_VIEWS];
    de_gpu_cmd_copy_buffer(cmd, vt->buffer, (u64)c->slot * vt->stride, c->gs->view_stage.ring,
                           c->src, vt->stride);
}

de_result de__gpu_scene_view_pass(de__gpu_scene *gs, de_rg *g, u32 slot,
                                  const de__gpu_view_params *p) {
    if (gs == NULL || g == NULL || p == NULL || !gs->views_in_graphs)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__gpu_scene_view_pass: %s",
                       gs != NULL && !gs->views_in_graphs ? "the scene was not made for it"
                                                          : "NULL argument");
    de__gpu_table *vt = &gs->tables[DE__GS_VIEWS];
    if (slot >= vt->capacity || slot >= gs->view_copy_cap)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de__gpu_scene_view_pass: view slot %u, the table holds %u", slot,
                       vt->capacity);
    de_result r = de__gpu_scene_set_view(gs, slot, p);
    if (r != DE_OK)
        return r;
    de__gs_view *e = &gs->entry_tmp.view;
    pack_view(gs, slot, e);
    u8 *sh = vt->shadow + (size_t)slot * vt->stride;
    if (memcmp(sh, e, sizeof *e) == 0)
        return DE_OK; /* unchanged: nothing to upload */
    if (!gs->view_staged) {
        r = ensure_uploader(gs);
        if (r == DE_OK)
            r = de__uploader_stage(gs->uploader, (u64)vt->capacity * vt->stride, 16u,
                                   &gs->view_stage);
        if (r != DE_OK)
            return DE_OK; /* the ring is full: the shadow stays, so the next frame uploads it */
        gs->view_staged = true;
    }
    memcpy(gs->view_stage.ptr + (size_t)slot * vt->stride, e, sizeof *e);
    memcpy(sh, e, sizeof *e);
    gs_view_copy *c = &gs->view_copies[slot];
    c->gs = gs;
    c->src = gs->view_stage.offset + (u64)slot * vt->stride;
    c->slot = slot;
    const de_rg_resource ring = de_rg_import_buffer(g, gs->view_stage.ring, DE_ACCESS_COPY_SRC);
    const de_rg_resource tab = de_rg_import_buffer(g, vt->buffer, vt->state);
    de_rg_set_name(g, tab, k_table_names[DE__GS_VIEWS]);
    de_rg_pass *pass = de_rg_add_pass(g, "gpu_scene.view", DE_QUEUE_GRAPHICS, pass_view, c);
    de_rg_pass_side_effect(pass);
    de_rg_read(pass, ring, DE_ACCESS_COPY_SRC);
    de_rg_write(pass, tab, DE_ACCESS_COPY_DST);
    de_rg_export(g, tab, DE_ACCESS_SHADER_READ);
    vt->state = DE_ACCESS_SHADER_READ; /* the next view graph is built after this one ran */
    gs->view_bytes += sizeof *e;
    gs->stats.frame_entries[DE__GS_VIEWS]++;
    return DE_OK;
}

void de__gpu_scene_views_end(de__gpu_scene *gs, de_gpu_fence fence, u64 value) {
    if (gs == NULL)
        return;
    if (gs->view_staged) {
        de__uploader_stage_close(gs->uploader, gs->view_stage.record, fence, value);
        gs->view_staged = false;
    }
    gs->stats.frame_bytes += gs->view_bytes;
    gs->stats.total_bytes += gs->view_bytes;
    gs->view_bytes = 0;
}

/* ---------- Readers, stats, tests ---------- */

de_rg_resource de__gpu_scene_import(de__gpu_scene *gs, de_rg *g, de__gs_table_id t) {
    if (gs == NULL || g == NULL || (u32)t >= DE__GS_TABLE_COUNT ||
        !DE_HANDLE_VALID(gs->tables[t].buffer))
        return (de_rg_resource){0};
    const de_rg_resource r = de_rg_import_buffer(g, gs->tables[t].buffer, gs->tables[t].state);
    de_rg_set_name(g, r, k_table_names[t]);
    return r;
}

de__gpu_scene_table_info de__gpu_scene_table(const de__gpu_scene *gs, de__gs_table_id t) {
    de__gpu_scene_table_info i;
    memset(&i, 0, sizeof i);
    if (gs == NULL || (u32)t >= DE__GS_TABLE_COUNT)
        return i;
    const de__gpu_table *tb = &gs->tables[t];
    i.buffer = tb->buffer;
    i.bindless = tb->bindless;
    i.stride = tb->stride;
    i.count = tb->count;
    i.capacity = tb->capacity;
    i.state = tb->state;
    return i;
}

static void stats_refresh(de__gpu_scene *gs) {
    u64 bytes = 0;
    u32 growths = 0;
    for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t) {
        gs->stats.count[t] = gs->tables[t].count;
        gs->stats.capacity[t] = gs->tables[t].capacity;
        bytes += de__gpu_table_bytes(&gs->tables[t]);
        growths += gs->tables[t].growths;
    }
    for (u32 i = 0; i < SCR_COUNT; ++i)
        bytes += gs->scratch[i].bytes;
    gs->stats.gpu_bytes = bytes;
    gs->stats.growths = growths;
    gs->stats.live_meshes = gs->mesh_map.count;
    gs->stats.live_materials = gs->mat_map.count;
}

const de__gpu_scene_stats *de__gpu_scene_stats_get(const de__gpu_scene *gs) {
    if (gs == NULL)
        return NULL;
    stats_refresh((de__gpu_scene *)gs); /* derived counters only */
    return &gs->stats;
}

void de__gpu_scene_fill_stats(const de__gpu_scene *gs, de_render_stats *s) {
    if (gs == NULL || s == NULL)
        return;
    const de__gpu_scene_stats *st = de__gpu_scene_stats_get(gs);
    s->gpu_scene_upload_bytes = st->frame_bytes;
    s->gpu_scene_instances = st->count[DE__GS_INSTANCES];
    s->gpu_scene_lights = st->count[DE__GS_LIGHTS];
    s->gpu_scene_meshes = st->live_meshes;
    s->gpu_scene_materials = st->live_materials;
    s->gpu_scene_gpu_bytes = st->gpu_bytes;
}

u32 de__gpu_scene_mesh_slot(const de__gpu_scene *gs, de_mesh m) {
    return gs != NULL && DE_HANDLE_VALID(m) ? map_get(&gs->mesh_map, m.id) : DE__GS_NONE;
}

u32 de__gpu_scene_material_slot(const de__gpu_scene *gs, de_material m) {
    return gs != NULL && DE_HANDLE_VALID(m) ? map_get(&gs->mat_map, m.id) : DE__GS_NONE;
}

void de__gpu_scene_material_changed(de__gpu_scene *gs, de_material m) {
    const u32 s = gs != NULL && DE_HANDLE_VALID(m) ? map_get(&gs->mat_map, m.id) : DE__GS_NONE;
    if (s != DE__GS_NONE && s < gs->mat_cap)
        gs->mat_dirty[s / 64u] |= (u64)1 << (s % 64u);
}

u32 de__gpu_scene_material_permutation(const de__gpu_scene *gs, u32 model) {
    return gs != NULL && model < DE__GS_MATERIAL_MODELS ? gs->stats.material_permutation[model]
                                                        : 0u;
}

bool de__gpu_scene_has_scatter(const de__gpu_scene *gs) {
    return gs != NULL && gs->has_scatter;
}

void de__gpu_scene_debug_apply(const de__gpu_scene *gs, de__gs_table_id t, u8 *mirror,
                               u32 mirror_entries) {
    if (gs == NULL || !gs->staged || (u32)t >= DE__GS_TABLE_COUNT)
        return;
    const u32 stride = gs->tables[t].stride;
    const u64 cap = (u64)mirror_entries * stride;
    for (u32 i = 0; i < gs->copy_count; ++i) {
        const gs_copy *c = &gs->copies[i];
        if (c->target == (u32)t && c->dst_off + c->size <= cap)
            memcpy(mirror + c->dst_off, gs->ring_base + c->src, (size_t)c->size);
    }
    for (u32 i = 0; i < gs->scatter_count; ++i) {
        const gs_scatter *s = &gs->scatters[i];
        if (s->table != (u32)t)
            continue;
        for (u32 e = 0; e < s->count; ++e) {
            u32 slot;
            memcpy(&slot, gs->ring_base + s->index_src + (u64)e * 4u, 4);
            if (slot < mirror_entries)
                memcpy(mirror + (u64)slot * stride,
                       gs->ring_base + s->payload_src + (u64)e * stride, stride);
        }
    }
}

void de__gpu_scene_debug_pack(const de__gpu_scene *gs, de__gs_table_id t, u32 slot, void *out) {
    if (gs == NULL || (u32)t >= DE__GS_TABLE_COUNT)
        return;
    pack_entry(gs, t, slot, out);
}

/* ---------- DENG-305: CPU reads for the forward transparent pass ---------- */

bool de__gpu_scene_instance_slots(const de__gpu_scene *gs, u32 s, u32 *mesh,
                                  const u32 **materials) {
    if (gs == NULL || s >= gs->inst_cap)
        return false;
    *mesh = gs->inst_mesh[s];
    *materials = &gs->inst_mat[(size_t)s * DE__GS_INSTANCE_MATERIALS];
    return true;
}

const void *de__gpu_scene_entry(const de__gpu_scene *gs, de__gs_table_id t, u32 slot) {
    if (gs == NULL || (t != DE__GS_MESHES && t != DE__GS_MATERIALS))
        return NULL;
    const de__gpu_table *tb = &gs->tables[t];
    return slot < tb->count ? tb->shadow + (size_t)slot * tb->stride : NULL;
}
